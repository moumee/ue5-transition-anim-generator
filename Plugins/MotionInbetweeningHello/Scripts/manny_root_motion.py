"""Move generated locomotion from the pelvis onto the root bone (Unreal root motion).

The inverse mapper leaves ``root`` at its reference pose and puts all travel on ``pelvis``.
Here the root gets a planar track (ground XY + yaw) and the pelvis is re-expressed relative
to it, so every bone keeps its component-space pose while the root carries the motion.

Where the source clips are known (``root_motion_reference`` in the model output) the root is
pinned to the source clips' own root at the context and target frames: there the saved pose is
the source pose, root-relative, so the transition joins root-motion clips without a shift.
Between pinned frames the root position follows the pelvis, with the pelvis-to-root offset
blended from one side to the other and smoothed so gait sway stays on the pelvis; the root yaw
is the least-bending curve through the pinned yaws, so body turning also stays on the pelvis.

Without a reference (model outputs that do not come from exported clips, or inputs exported
before root motion support) nothing is known about the source roots: the root then keeps the
exported axes (no yaw) and follows the smoothed ground projection of the pelvis.
"""
import numpy as np

REFERENCE_SCHEMA = "motion_inbetweening.root_motion_reference.v1"
# Second-difference penalty of the position smoother, for 30 fps samples. 300 passes about 30% of
# a jog's pelvis sway (0.7 s cycle) and 70% of a walk's (1.07 s). On the Jog->Walk/Idle transitions
# measured (N = 15..60) the pelvis stayed within 6 cm of its reference spot over the root; 1000 and
# above let the root cut 15-25 cm away from the pelvis where the speed changes.
POSITION_SMOOTHNESS = 300.0


def quat_to_matrix(q):
    x, y, z, w = np.asarray(q, dtype=np.float64) / np.linalg.norm(q)
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def matrix_to_quat(m, previous=None):
    """XYZW quaternion; the sign follows ``previous`` so keys do not flip hemisphere."""
    trace = m[0, 0] + m[1, 1] + m[2, 2]
    if trace > 0:
        s = 2 * np.sqrt(trace + 1)
        q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, s / 4]
    else:
        i = int(np.argmax(np.diag(m)))
        j, k = (i + 1) % 3, (i + 2) % 3
        s = 2 * np.sqrt(max(1 + m[i, i] - m[j, j] - m[k, k], 0.0))
        q = [0.0, 0.0, 0.0, (m[k, j] - m[j, k]) / s]
        q[i], q[j], q[k] = s / 4, (m[j, i] + m[i, j]) / s, (m[k, i] + m[i, k]) / s
    q = np.asarray(q, dtype=np.float64)
    q /= np.linalg.norm(q)
    if previous is not None and float(np.dot(q, previous)) < 0:
        q = -q
    return q


def yaw_matrix(angle):
    c, s = np.cos(angle), np.sin(angle)
    return np.array([[c, -s, 0.0], [s, c, 0.0], [0.0, 0.0, 1.0]])


def numeric_array(value, shape, label):
    def numeric(v):
        return all(numeric(x) for x in v) if isinstance(v, list) else type(v) in (int, float)
    if not isinstance(value, list) or not numeric(value):
        raise ValueError(f"{label} must be an array of JSON numbers.")
    try:
        data = np.asarray(value, dtype=np.float64)
    except (ValueError, TypeError, OverflowError) as error:
        raise ValueError(f"{label} must be a rectangular array of finite numbers.") from error
    if data.shape != shape or not np.isfinite(data).all():
        raise ValueError(f"{label} must be finite with shape {list(shape)}.")
    return data


def quaternions(value, count, label):
    data = numeric_array(value, (count, 4), label)
    norms = np.linalg.norm(data, axis=1)
    if np.any(np.abs(norms - 1.0) > 1e-3):
        raise ValueError(f"{label} must contain normalized XYZW quaternions.")
    return data / norms[:, None]


# ---- input side: what the source clips' roots do ----------------------------------------------

def source_root_tracks(source):
    """Per-sample root track of an exported clip, from the pelvis local and component poses.

    ``pelvis`` is a child of ``root``, so root = pelvis_component * inverse(pelvis_local).
    """
    pelvis_translation, pelvis_rotation, root_translation, root_rotation = [], [], [], []
    pelvis_q = root_q = None
    for frame in source["frames"]:
        pelvis = next((bone for bone in frame["bones"] if bone["name"] == "pelvis"), None)
        if pelvis is None:
            raise ValueError("The exported clip has no pelvis bone.")
        local_r = quat_to_matrix(pelvis["local"]["rotation_xyzw"])
        local_t = np.asarray(pelvis["local"]["translation"], dtype=np.float64)
        world_r = quat_to_matrix(pelvis["world"]["rotation_xyzw"])
        world_t = np.asarray(pelvis["world"]["translation"], dtype=np.float64)
        root_r = world_r @ local_r.T
        root_t = world_t - root_r @ local_t
        pelvis_q, root_q = matrix_to_quat(local_r, pelvis_q), matrix_to_quat(root_r, root_q)
        pelvis_translation.append(local_t.tolist())
        pelvis_rotation.append(pelvis_q.tolist())
        root_translation.append(root_t.tolist())
        root_rotation.append(root_q.tolist())
    return {
        "description": "Source clip root bone and the pelvis relative to it, per exported sample "
                       "(Unreal component space, cm, XYZW).",
        "pelvis_in_root": {"translation_cm": pelvis_translation, "rotation_xyzw": pelvis_rotation},
        "root": {"translation_cm": root_translation, "rotation_xyzw": root_rotation},
    }


def build_reference(start, end, start_index, target_index, transition, context=10):
    """Reference for one transition: the source pelvis-in-root at the context and target samples.

    Returns None unless both inputs carry ``source_root_motion`` (exports made before root
    motion support do not); the output stage then derives the root from the pelvis alone.
    """
    def read_tracks(document, label):
        source = document.get("source_root_motion")
        if source is None:
            return None
        count = len(document["vectors_tx135"])
        parsed = {}
        for group in ("pelvis_in_root", "root"):
            entry = source.get(group) if isinstance(source, dict) else None
            if not isinstance(entry, dict):
                raise ValueError(f"{label} source_root_motion.{group} is missing.")
            name = f"{label} source_root_motion.{group}"
            parsed[group] = (numeric_array(entry.get("translation_cm"), (count, 3), name + ".translation_cm"),
                             quaternions(entry.get("rotation_xyzw"), count, name + ".rotation_xyzw"))
        return parsed

    start_tracks, end_tracks = read_tracks(start, "start"), read_tracks(end, "end")
    if start_tracks is None or end_tracks is None:
        return None
    start_t, start_q = start_tracks["pelvis_in_root"]
    end_t, end_q = end_tracks["pelvis_in_root"]
    root_t, root_q = end_tracks["root"]
    delta = None
    if target_index + 1 < len(root_t):
        # Root step of the destination clip right after the target, in the target root's frame.
        here, following = quat_to_matrix(root_q[target_index]), quat_to_matrix(root_q[target_index + 1])
        delta = {"translation_cm": (here.T @ (root_t[target_index + 1] - root_t[target_index])).tolist(),
                 "rotation_xyzw": matrix_to_quat(here.T @ following).tolist()}
    return {"schema": REFERENCE_SCHEMA,
            "description": "Source clips' pelvis relative to their root at the listed output frames; "
                           "next_root_delta is the destination root step after the last frame.",
            "frames": list(range(context)) + [context + transition],
            "pelvis_in_root": {
                "translation_cm": start_t[start_index:start_index + context].tolist() + [end_t[target_index].tolist()],
                "rotation_xyzw": start_q[start_index:start_index + context].tolist() + [end_q[target_index].tolist()]},
            "next_root_delta": delta}


def parse_reference(reference, frame_count):
    if not isinstance(reference, dict) or reference.get("schema") != REFERENCE_SCHEMA:
        raise ValueError("Unsupported root_motion_reference schema.")
    frames = reference.get("frames")
    if (not isinstance(frames, list) or len(frames) < 2 or any(type(f) is not int for f in frames)
            or any(b <= a for a, b in zip(frames, frames[1:])) or frames[0] != 0 or frames[-1] != frame_count - 1):
        raise ValueError("root_motion_reference.frames must be increasing output frame indices "
                         "that include the first and the last frame.")
    pelvis = reference.get("pelvis_in_root")
    if not isinstance(pelvis, dict):
        raise ValueError("root_motion_reference.pelvis_in_root is missing.")
    parsed = {"frames": frames,
              "translation": numeric_array(pelvis.get("translation_cm"), (len(frames), 3),
                                           "root_motion_reference.pelvis_in_root.translation_cm"),
              "rotation": quaternions(pelvis.get("rotation_xyzw"), len(frames),
                                      "root_motion_reference.pelvis_in_root.rotation_xyzw"),
              "next": None}
    delta = reference.get("next_root_delta")
    if delta is not None:
        if not isinstance(delta, dict):
            raise ValueError("root_motion_reference.next_root_delta must be an object or null.")
        parsed["next"] = (
            numeric_array(delta.get("translation_cm"), (3,), "root_motion_reference.next_root_delta.translation_cm"),
            quaternions([delta.get("rotation_xyzw")], 1, "root_motion_reference.next_root_delta.rotation_xyzw")[0])
    return parsed


# ---- output side: solve the root track and re-express the pose ---------------------------------

def smooth(values, fixed, weight):
    """Fit ``values`` with a second-difference penalty of ``weight``; ``fixed`` rows are kept.

    Fixed rows still enter the penalty, so the curve leaves and reaches them without a kink.
    ``weight=None`` ignores the free rows' values and fills them with the least-bending curve
    through the fixed rows (needs fixed rows on both sides of every free row).
    """
    values = np.asarray(values, dtype=np.float64)
    free = ~np.asarray(fixed, dtype=bool)
    count = len(values)
    if count < 3 or not free.any():
        return values.copy()
    difference = np.zeros((count - 2, count))
    rows = np.arange(count - 2)
    difference[rows, rows], difference[rows, rows + 1], difference[rows, rows + 2] = 1.0, -2.0, 1.0
    unknown, known = difference[:, free], difference[:, ~free]
    bend = -unknown.T @ (known @ values[~free])
    result = values.copy()
    if weight is None:
        result[free] = np.linalg.solve(unknown.T @ unknown, bend)
    else:
        result[free] = np.linalg.solve(np.eye(int(free.sum())) + weight * unknown.T @ unknown,
                                       values[free] + weight * bend)
    return result


def solve_root(pelvis_xy, anchors=None, following=None, default_offset=(0.0, 0.0), smoothness=POSITION_SMOOTHNESS):
    """Planar root track (XY, unwrapped yaw per frame) for a pelvis ground path.

    ``anchors`` maps frame -> (root XY, root yaw) where the root is known and must include the
    first and last frame; ``following`` is the known root (XY, yaw) one sample after the last
    frame, used only to match the exit velocity. Without anchors the yaw is zero and the root
    is the smoothed pelvis path shifted by ``default_offset``, exact at both ends.
    """
    pelvis_xy = np.asarray(pelvis_xy, dtype=np.float64)
    count = len(pelvis_xy)
    fixed = np.zeros(count, dtype=bool)
    frames = sorted(anchors or {})
    yaw_tail, xy_tail = [], np.zeros((0, 2))
    if frames:
        if frames[0] != 0 or frames[-1] != count - 1:
            raise ValueError("Known root frames must include the first and the last frame.")
        fixed[frames] = True
        anchor_xy = np.array([anchors[f][0] for f in frames], dtype=np.float64)
        anchor_yaw = np.unwrap([anchors[f][1] for f in frames])
        if following is not None:
            # Same turn of the circle as the last known yaw.
            yaw_tail = [anchor_yaw[-1] + (following[1] - anchor_yaw[-1] + np.pi) % (2 * np.pi) - np.pi]
            xy_tail = np.asarray(following[0], dtype=np.float64)[None]
        pinned = np.r_[fixed, [True] * len(yaw_tail)]
        yaw = np.zeros(count)
        yaw[frames] = anchor_yaw
        yaw = smooth(np.r_[yaw, yaw_tail], pinned, None)[:count]
        # Known pelvis-to-root offsets in the root's own frame, blended across the unknown frames.
        offset = np.stack([yaw_matrix(-angle)[:2, :2] @ (xy - pelvis_xy[f])
                           for f, xy, angle in zip(frames, anchor_xy, anchor_yaw)])
        offset = np.stack([np.interp(np.arange(count), frames, offset[:, axis]) for axis in range(2)], axis=1)
    else:
        # Nothing known: start and end exactly under the pelvis, so the root travels as far as it does.
        fixed[[0, count - 1]] = True
        pinned = fixed
        yaw = np.zeros(count)
        offset = np.tile(np.asarray(default_offset, dtype=np.float64), (count, 1))
    estimate = pelvis_xy + np.stack([yaw_matrix(angle)[:2, :2] @ o for angle, o in zip(yaw, offset)])
    if frames:
        estimate[frames] = anchor_xy
    xy = smooth(np.concatenate([estimate, xy_tail]), pinned, smoothness)[:count]
    return xy, yaw


def _reference_transform(bone):
    transform = bone["reference_local_transform"]

    def values(value, keys):
        return [value[k] for k in keys] if isinstance(value, dict) else list(value)
    return (quat_to_matrix(values(transform["rotation_quaternion"], "xyzw")),
            np.asarray(values(transform["translation_cm"], "xyz"), dtype=np.float64))


def apply_root_motion(converted, skeleton, reference=None, smoothness=POSITION_SMOOTHNESS):
    """Rewrite ``full_local_pose_frames`` so the root bone carries the ground motion.

    Component-space poses of every bone are unchanged up to one rigid transform of the whole
    clip: the root track is rebased so the first frame's root is at the reference pose.
    Returns the report also stored as ``converted["root_motion"]``.
    """
    frames = converted["full_local_pose_frames"]
    bones = skeleton["bones"]
    names = [bone["name"] for bone in bones]
    roots = [i for i, bone in enumerate(bones) if bone.get("parent") in (None, "", "None")]
    if len(roots) != 1 or "pelvis" not in names or bones[names.index("pelvis")].get("parent") != names[roots[0]]:
        raise ValueError("Root motion needs a single root bone with pelvis as its direct child.")
    root_index, pelvis_index = roots[0], names.index("pelvis")
    root_ref_r, root_ref_t = _reference_transform(bones[root_index])
    _, pelvis_ref_t = _reference_transform(bones[pelvis_index])
    reference_xy = (root_ref_t + root_ref_r @ pelvis_ref_t)[:2]      # pelvis ground point in the reference pose

    count = len(frames)
    pelvis_r, pelvis_t = np.zeros((count, 3, 3)), np.zeros((count, 3))
    for index, frame in enumerate(frames):
        root, pelvis = frame["bones"][root_index], frame["bones"][pelvis_index]
        if root["name"] != names[root_index] or pelvis["name"] != "pelvis":
            raise ValueError("Pose frames do not follow the skeleton bone order.")
        parent_r = quat_to_matrix(root["local_rotation_xyzw"])
        parent_t = np.asarray(root["local_translation_cm"], dtype=np.float64)
        pelvis_r[index] = parent_r @ quat_to_matrix(pelvis["local_rotation_xyzw"])
        pelvis_t[index] = parent_t + parent_r @ np.asarray(pelvis["local_translation_cm"], dtype=np.float64)

    anchors, following, source_tilt, source_height = {}, None, 0.0, 0.0
    if reference is not None:
        parsed = parse_reference(reference, count)

        def ground_motion(root_r, root_t):
            # Root bone transform = motion * reference root transform; keep the ground-plane part.
            motion_r = root_r @ root_ref_r.T
            motion_t = root_t - motion_r @ root_ref_t
            tilt = float(np.degrees(np.arccos(np.clip(motion_r[2, 2], -1.0, 1.0))))
            return (motion_t[:2], float(np.arctan2(motion_r[1, 0], motion_r[0, 0]))), tilt, abs(float(motion_t[2]))

        for frame, translation, quaternion in zip(parsed["frames"], parsed["translation"], parsed["rotation"]):
            root_r = pelvis_r[frame] @ quat_to_matrix(quaternion).T
            root_t = pelvis_t[frame] - root_r @ translation
            anchors[frame], tilt, height = ground_motion(root_r, root_t)
            source_tilt, source_height = max(source_tilt, tilt), max(source_height, height)
        if parsed["next"] is not None:
            delta_t, delta_q = parsed["next"]
            following = ground_motion(root_r @ quat_to_matrix(delta_q), root_t + root_r @ delta_t)[0]

    xy, yaw = solve_root(pelvis_t[:, :2], anchors, following, default_offset=-reference_xy, smoothness=smoothness)

    root_q = pelvis_q = None
    rebase = yaw_matrix(-yaw[0])
    origin = np.array([xy[0, 0], xy[0, 1], 0.0])
    worst_offset = 0.0
    for index, frame in enumerate(frames):
        turn = yaw_matrix(yaw[index])
        root_r = turn @ root_ref_r
        root_t = turn @ root_ref_t + np.array([xy[index, 0], xy[index, 1], 0.0])
        local_r = root_r.T @ pelvis_r[index]
        local_t = root_r.T @ (pelvis_t[index] - root_t)
        worst_offset = max(worst_offset, float(np.linalg.norm(local_t[:2] - pelvis_ref_t[:2])))
        # Saved root: the same motion measured from the first frame.
        root_q = matrix_to_quat(rebase @ root_r, root_q)
        pelvis_q = matrix_to_quat(local_r, pelvis_q)
        frame["bones"][root_index].update(local_translation_cm=(rebase @ (root_t - origin)).tolist(),
                                          local_rotation_xyzw=root_q.tolist())
        frame["bones"][pelvis_index].update(local_translation_cm=local_t.tolist(), local_rotation_xyzw=pelvis_q.tolist())

    steps = np.linalg.norm(np.diff(xy, axis=0), axis=1)
    report = {
        "enabled": True,
        "source": "source_clip_roots" if anchors else "pelvis_projection",
        "reference_frames": sorted(anchors),
        "exit_velocity_matched": following is not None,
        "position_smoothness": smoothness,
        "first_frame_rebased_from": {"translation_cm": origin.tolist(), "yaw_degrees": float(np.degrees(yaw[0]))},
        "total_translation_cm": float(np.linalg.norm(xy[-1] - xy[0])),
        "total_yaw_degrees": float(np.degrees(yaw[-1] - yaw[0])),
        "max_root_step_cm": float(steps.max()) if len(steps) else 0.0,
        "max_pelvis_offset_from_reference_cm": worst_offset,
        # A source root that leaves the ground plane keeps that part on the pelvis.
        "source_root_max_tilt_degrees": source_tilt,
        "source_root_max_height_cm": source_height,
        "passed": bool(np.isfinite(xy).all() and np.isfinite(yaw).all()),
    }
    converted["root_motion"] = report
    return report
