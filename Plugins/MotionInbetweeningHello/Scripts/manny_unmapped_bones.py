"""Give the bones the mapper does not drive the motion of the source clips.

The inverse mapping writes 22 Manny bones and leaves the rest of the skeleton (fingers, twist bones, spine_04,
spine_05, neck_02) at the reference pose, so they snap when a transition is joined to its source clips. With the
clips' full local poses at hand those bones copy the start clip over the context frames and reach the destination
clip's pose on the target frame. In between they crossfade from the start clip playing on to the destination clip
leading up to its target sample, so their speed is continuous at both joins. Mapped bones below a filled bone
(clavicles, neck_01, head) get new local rotations that keep their component-space orientation, so the pose the
model produced is not rotated.

Data flow: export_manny_input.py writes full_local_pose.json, prepare_manny_input.py copies it into the model
input, infer.py turns the two inputs into a per-frame reference (build_reference) and prepare_manny_output.py
applies it (apply). Inputs without the pose leave the bones at the reference pose as before.
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np

from manny_root_motion import matrix_to_quat, quat_to_matrix

SOURCE_FILE = "full_local_pose.json"
REFERENCE_SCHEMA = "motion_inbetweening.unmapped_bone_reference.v1"


def load_source_pose(directory):
    """The clip's full local pose written next to source_manny.json, or None for exports made without it."""
    path = Path(directory) / SOURCE_FILE
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else None


def slerp(a, b, u):
    a, b = np.asarray(a, dtype=float), np.asarray(b, dtype=float)
    dot = float(a @ b)
    if dot < 0.0:
        b, dot = -b, -dot
    if dot > 0.9995:
        q = a + u * (b - a)
    else:
        angle = np.arccos(dot)
        q = (np.sin((1.0 - u) * angle) * a + np.sin(u * angle) * b) / np.sin(angle)
    return q / np.linalg.norm(q)


def angle_degrees(a, b):
    return float(np.degrees(2.0 * np.arccos(min(1.0, abs(float(np.asarray(a) @ np.asarray(b)))))))


class Clip:
    """A full local pose export that can be sampled before its first and after its last sample.

    A clip whose pose closes on itself (its period follows from play_length_seconds) wraps around; any other clip
    holds its first or last sample.
    """

    def __init__(self, pose, names, label):
        column = {name: i for i, name in enumerate(pose["bones"])}
        self.label = label
        self.local = np.asarray(pose["local"], dtype=float)[:, [column[name] for name in names]]
        if self.local.ndim != 3 or self.local.shape[2] != 7 or not np.isfinite(self.local).all():
            raise ValueError(f"The {label} clip's full local pose must be samples x bones x 7 finite numbers.")
        self.local[:, :, 3:] /= np.linalg.norm(self.local[:, :, 3:], axis=-1, keepdims=True)
        self.count = len(self.local)
        self.period = int(round(float(pose.get("play_length_seconds", 0.0)) * float(pose.get("sample_rate_hz", 30.0))))
        self.cyclic = False
        if 3 <= self.period <= self.count:
            def step(a, b):
                return max(angle_degrees(p, q) for p, q in zip(self.local[a, :, 3:], self.local[b, :, 3:]))
            # The step that closes the loop is no larger than a few ordinary steps.
            self.cyclic = step(self.period - 1, 0) <= 3.0 * max(step(self.period - 1, self.period - 2), step(1, 0)) + 0.5

    def require(self, index):
        if not 0 <= index < self.count:
            raise ValueError(f"The {self.label} clip's full local pose has {self.count} samples; sample {index} is needed.")

    def sample(self, index):
        return self.local[index % self.period if self.cyclic else min(max(index, 0), self.count - 1)]


def build_reference(start, end, start_index, target_index, transition, context=10):
    """Per-frame local poses for a transition, from two model inputs; None unless both carry source_full_pose.

    Frame f < context is start sample start_index + f, the last frame is end sample target_index, and the frames
    between blend the start clip playing on into the end clip arriving at its target sample.
    """
    first, second = start.get("source_full_pose"), end.get("source_full_pose")
    if not first or not second:
        return None
    shared = set(second["bones"])
    names = [name for name in first["bones"] if name in shared]
    onward, arriving = Clip(first, names, "start"), Clip(second, names, "end")
    onward.require(start_index)
    onward.require(start_index + context - 1)
    arriving.require(target_index)
    count = context + transition + 1
    frames = []
    for frame in range(count):
        if frame < context:
            row = onward.sample(start_index + frame)
        elif frame == count - 1:
            row = arriving.sample(target_index)
        else:
            a, b = onward.sample(start_index + frame), arriving.sample(target_index - (count - 1 - frame))
            u = (frame - (context - 1)) / (count - context)
            u = u * u * (3.0 - 2.0 * u)
            row = np.concatenate([(1.0 - u) * a[:, :3] + u * b[:, :3],
                                  np.asarray([slerp(p, q, u) for p, q in zip(a[:, 3:], b[:, 3:])])], axis=1)
        frames.append(row.tolist())
    return {"schema": REFERENCE_SCHEMA,
            "description": "Local pose of every bone from the source clips: copied on the pinned frames, blended between them.",
            "layout": "per frame, per bone: local translation cm xyz, local rotation xyzw",
            "bones": names, "pinned_frames": list(range(context)) + [count - 1],
            "start_clip_wraps": onward.cyclic, "end_clip_wraps": arriving.cyclic, "local": frames}


def apply(converted, skeleton, reference):
    """Rewrite converted['full_local_pose_frames'] in place; the report is returned and stored in the document."""
    if reference is None:
        report = {"source": "reference_pose", "filled_bones": 0}
        converted["unmapped_bones"] = report
        return report
    frames = converted["full_local_pose_frames"]
    names = [bone["name"] for bone in frames[0]["bones"]]
    index = {name: i for i, name in enumerate(names)}
    parent = {bone["name"]: bone["parent"] for bone in skeleton["bones"]}
    order = [bone["name"] for bone in skeleton["bones"] if bone["name"] in index]        # parents first

    def below_pelvis(name):
        while name is not None:
            name = parent[name]
            if name == "pelvis":
                return True
        return False

    if reference.get("schema") != REFERENCE_SCHEMA:
        raise ValueError("Unsupported unmapped bone reference.")
    column = {name: i for i, name in enumerate(reference["bones"])}
    local = np.asarray(reference["local"], dtype=float)
    if local.shape != (len(frames), len(column), 7) or not np.isfinite(local).all():
        raise ValueError(f"The unmapped bone reference must hold {len(frames)} frames x {len(column)} bones x 7 finite numbers.")
    mapped = set(converted["manny_bone_order"])
    filled = [name for name in order if below_pelvis(name) and name not in mapped]
    missing = [name for name in filled if name not in column]
    if missing:
        raise ValueError(f"The unmapped bone reference lacks bones: {missing[:5]}")
    pinned_frames = set(reference["pinned_frames"])

    pinned_error = 0.0
    previous = {}
    for frame_index, frame in enumerate(frames):
        bones = frame["bones"]
        old, new, changed = {}, {}, {}
        for name in order:
            matrix = quat_to_matrix(bones[index[name]]["local_rotation_xyzw"])
            above = parent[name] if parent[name] in old else None
            old[name] = matrix if above is None else old[above] @ matrix
            if name in filled:
                row = local[frame_index, column[name]]
                rotation = row[3:] / np.linalg.norm(row[3:])
                if name in previous and previous[name] @ rotation < 0.0:
                    rotation = -rotation
                bones[index[name]]["local_translation_cm"] = [float(v) for v in row[:3]]
                matrix, changed[name] = quat_to_matrix(rotation), True
            elif above is not None and changed.get(above):
                # Keep the orientation the mapper gave this bone; its children then need no change.
                matrix = new[above].T @ old[name]
                rotation = matrix_to_quat(matrix, previous.get(name))
                changed[name] = False
            else:
                new[name] = old[name] if above is None else new[above] @ matrix
                changed[name] = bool(above is not None and changed.get(above))
                continue
            bones[index[name]]["local_rotation_xyzw"] = [float(v) for v in rotation]
            previous[name] = np.asarray(rotation, dtype=float)
            new[name] = matrix if above is None else new[above] @ matrix
        if frame_index in pinned_frames:
            # Where the source clips are known, the whole body below the pelvis should now equal them.
            for name in order:
                if below_pelvis(name) and name in column:
                    row = local[frame_index, column[name]]
                    pinned_error = max(pinned_error, angle_degrees(bones[index[name]]["local_rotation_xyzw"], row[3:] / np.linalg.norm(row[3:])))

    report = {"source": "source_clips", "filled_bones": len(filled), "pinned_frames": sorted(pinned_frames),
              "start_clip_wraps": bool(reference.get("start_clip_wraps")), "end_clip_wraps": bool(reference.get("end_clip_wraps")),
              "max_rotation_difference_from_source_on_pinned_frames_degrees": pinned_error}
    converted["unmapped_bones"] = report
    return report
