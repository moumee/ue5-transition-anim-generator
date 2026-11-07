"""Synchronous official Context/Detail inference for the Manny plugin contract."""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import pickle
import sys
import time

import numpy as np

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parents[1]
sys.path[:0] = [str(HERE / "vendor"), str(PROJECT / "Tools/MannyLafanMapping"),
                str(PROJECT / "Plugins/MotionInbetweeningHello/Scripts")]
from prepare_manny_input import file_hash, save_json
from prepare_manny_output import load_model_output
from run_manny_inference import read_input
from official_context_preprocess import (
    denormalize, load_stats, pack_state, project_static_channels,
    reverse_start_center_root, start_center_root, get_root_heading_offset,
)
from manny_lafan_transform import matrix6d_to_9d, matrix_to_quat_xyzw, quat_to_matrix

SUPPORTED_TRANSITION_RANGE = (5, 45)  # Product policy; not a positional-table limit.

CALIBRATION_FILE = PROJECT / "Tools/MannyLafanMapping/rest_pose_corrections_22_prototype.json"
# Model-frame refinement: per-joint roll about the bone axis (+X) on top of the
# team calibration, i.e. B' = B @ Rx(roll). Applied just before the model and
# undone right after it, so exported inputs and the inverse mapper are unchanged.
DEFAULT_FRAME_REFINEMENT = HERE / "configs/manny_frame_refinement_v1.json"
# LAFAN1 clavicles are nearly static (6D std ~0.012), so Manny shoulder motion is
# always far outside the training data; the roll refinement does not correct the
# 5-7 degree bone-direction tilt of the arm frames either. Bounding these input
# z-scores keeps a few channels from dominating the encoder (validated on all
# exported Manny/Quinn clips; see docs/frame_refinement.md at the repository root).
DEFAULT_INPUT_Z_CLAMP = {"limit": 3.0, "joints": [
    "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand",
    "RightShoulder", "RightArm", "RightForeArm", "RightHand"]}


def integer(value, label, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"{label} must be an integer in [{low}, {high}].")
    return value


def numbers(value, shape, label):
    def numeric(v):
        return all(numeric(x) for x in v) if isinstance(v, list) else type(v) in (int, float)
    if not numeric(value):
        raise ValueError(f"{label} must contain JSON numbers, not bool/string/null.")
    data = np.asarray(value, dtype=np.float64)
    if data.shape != shape or not np.isfinite(data).all():
        raise ValueError(f"{label} must be finite with shape {shape}.")
    return data


def decode_rotations(values):
    six = np.asarray(values, dtype=np.float64)
    # Existing mapper uses interleaved first two matrix columns.
    columns = six.reshape(*six.shape[:-1], 3, 2)
    a, b = columns[..., 0], columns[..., 1]
    if np.any(np.linalg.norm(a, axis=-1) < 1e-7) or np.any(np.linalg.norm(np.cross(a, b), axis=-1) < 1e-7):
        raise ValueError("Rotation6D contains a zero or collinear basis.")
    return matrix6d_to_9d(six)


def text_sha256_variants(path):
    """SHA-256 of a text file with LF and with CRLF line endings.

    Git may check the calibration out with either ending (core.autocrlf), so
    recorded hashes are compared against both. The first item is the LF hash.
    """
    lf = Path(path).read_bytes().replace(b"\r\n", b"\n")
    return [hashlib.sha256(lf).hexdigest(), hashlib.sha256(lf.replace(b"\n", b"\r\n")).hexdigest()]


def roll_matrix(degrees):
    """Rotation about the joint's local +X (bone) axis."""
    angle = np.radians(degrees)
    c, s = np.cos(angle), np.sin(angle)
    return np.array([[1.0, 0.0, 0.0], [0.0, c, -s], [0.0, s, c]])


def load_frame_refinement(path, calibration):
    """Read per-joint bone-axis rolls; return (22x3x3 frame changes, provenance)."""
    path = Path(path)
    if not path.is_file():
        raise ValueError(f"Missing frame refinement file: {path}")
    document = load_model_output(path)
    if document.get("schema") != "motion_inbetweening.frame_refinement.v1":
        raise ValueError("Unsupported frame refinement schema.")
    names = calibration["joint_order"]
    if document.get("joint_order") != names:
        raise ValueError("Frame refinement joint order differs from the team calibration.")
    calibration_hashes = text_sha256_variants(CALIBRATION_FILE)
    if document.get("calibration_sha256") not in calibration_hashes:
        raise ValueError("Frame refinement was estimated for a different calibration file. "
                         "Re-run estimate_frame_refinement.py.")
    rolls = document.get("roll_degrees")
    if not isinstance(rolls, dict) or not rolls:
        raise ValueError("Frame refinement needs a non-empty roll_degrees object.")
    frames = np.repeat(np.eye(3)[None], len(names), axis=0)
    for name, value in rolls.items():
        # A root roll would change the heading alignment, so only child joints are allowed.
        if name not in names[1:]:
            raise ValueError(f"Invalid frame refinement joint: {name}")
        if type(value) not in (int, float) or not np.isfinite(value) or abs(value) > 180:
            raise ValueError(f"Roll for {name} must be finite degrees in [-180, 180].")
        frames[names.index(name)] = roll_matrix(value)
    return frames, {"file": str(path.resolve()), "sha256": file_hash(path),
                    "calibration_sha256": calibration_hashes[0], "calibration_sha256_variants": calibration_hashes,
                    "roll_degrees": dict(rolls)}


def refine_rotations(rotations, frames, parents, inverse=False):
    """Re-express parent-local rotations in refined joint frames.

    With B'_j = B_j @ C_j the team mapper would produce l'_j = C_p^T l_j C_j,
    so this is exactly equivalent to re-exporting with the refined calibration.
    ``inverse=True`` maps model outputs back to the original calibration frames.
    """
    rotations = np.asarray(rotations, dtype=np.float64)
    result = rotations.copy()
    for joint, parent in enumerate(parents):
        parent_frame = frames[parent] if parent >= 0 else np.eye(3)
        if inverse:
            result[:, joint] = parent_frame @ rotations[:, joint] @ frames[joint].T
        else:
            result[:, joint] = parent_frame.T @ rotations[:, joint] @ frames[joint]
    return result


def parse_input_z_clamp(value, joint_order):
    if value is None:
        return None
    if not isinstance(value, dict) or set(value) != {"limit", "joints"}:
        raise ValueError("input_z_clamp must be null or an object with limit and joints.")
    limit, joints = value["limit"], value["joints"]
    if type(limit) not in (int, float) or not np.isfinite(limit) or limit <= 0:
        raise ValueError("input_z_clamp.limit must be a positive number.")
    if (not isinstance(joints, list) or not joints or len(set(joints)) != len(joints)
            or any(name not in joint_order[1:] for name in joints)):
        raise ValueError("input_z_clamp.joints must list distinct non-root model joints.")
    columns = np.concatenate([np.arange(joint_order.index(n) * 6, joint_order.index(n) * 6 + 6) for n in joints])
    return {"limit": float(limit), "joints": list(joints), "columns": columns}


def clamp_input_z(states, mean, std, clamp):
    """Clip selected rotation channels to mean +- limit * std (z-score clamp)."""
    states = np.asarray(states, dtype=np.float64)
    columns, limit = clamp["columns"], clamp["limit"]
    z = (states[:, columns] - mean[columns]) / std[columns]
    before = np.abs(z)
    result = states.copy()                  # untouched values stay bit-identical
    result[:, columns] = np.where(before > limit, mean[columns] + np.clip(z, -limit, limit) * std[columns],
                                  states[:, columns])
    return result, before


def source_hashes():
    manifest = load_model_output(HERE / "vendor/source_manifest.json")
    for entry in manifest["files"]:
        if file_hash(HERE / "vendor" / entry["path"]) != entry["sha256"]:
            raise ValueError("Vendored model source differs from its pinned manifest: " + entry["path"])
    return {"model_source_commit": manifest["commit"],
            "vendor_manifest_sha256": file_hash(HERE / "vendor/source_manifest.json"),
            "adapter_sha256": file_hash(Path(__file__)),
            "architecture_sha256": {kind: file_hash(HERE / "configs" / (kind + ".json")) for kind in ("context", "detail")},
            "model_manifest_sha256": file_hash(HERE / "model_manifest.json"),
            "mapping_sha256": {name: file_hash(PROJECT / "Tools/MannyLafanMapping" / name)
                               for name in ("official_context_preprocess.py", "manny_lafan_transform.py")}}


class Runtime:
    def __init__(self, config_path):
        import torch
        from motion_inbetween.model import ContextTransformer, DetailTransformer
        self.torch = torch
        self.config_path = Path(config_path).resolve()
        self.config = load_model_output(self.config_path)
        if self.config.get("schema") != "motion_inbetweening.model_config.v1":
            raise ValueError("Unsupported model config schema.")
        self.pipeline = self.config.get("pipeline", "context_detail")
        if self.pipeline not in ("context", "context_detail"):
            raise ValueError("pipeline must be context or context_detail.")
        if self.config.get("device", "cpu") != "cpu":
            raise ValueError("This validated adapter supports device=cpu.")
        self.placement = self.config.get("target_placement", "endpoint_velocity")
        if self.placement not in ("preserve_exported", "continue_context", "endpoint_velocity"):
            raise ValueError("Unknown target_placement policy.")
        torch.set_num_threads(integer(self.config.get("cpu_threads", 4), "cpu_threads", 1, 32))
        self.provenance = source_hashes()
        self.calibration = load_model_output(CALIBRATION_FILE)
        # Omitted -> bundled refinement; null -> disabled (legacy behaviour); string -> path
        # relative to this config file.
        if "frame_refinement" not in self.config:
            refinement_path = DEFAULT_FRAME_REFINEMENT
        elif self.config["frame_refinement"] is None:
            refinement_path = None
        elif isinstance(self.config["frame_refinement"], str) and self.config["frame_refinement"]:
            refinement_path = (self.config_path.parent / self.config["frame_refinement"]).resolve()
        else:
            raise ValueError("frame_refinement must be omitted, null or a file path.")
        self.refine, self.refinement_info = (None, None) if refinement_path is None else \
            load_frame_refinement(refinement_path, self.calibration)
        self.clamp = parse_input_z_clamp(self.config.get("input_z_clamp", DEFAULT_INPUT_Z_CLAMP),
                                         self.calibration["joint_order"])
        pinned = load_model_output(HERE / "model_manifest.json")["files"]
        self.files, self.hashes = {}, {}
        required = ["context_checkpoint", "context_statistics"]
        if self.pipeline == "context_detail":
            required += ["detail_checkpoint", "detail_statistics"]
        for key in required:
            path_value = self.config.get(key)
            if not isinstance(path_value, str) or not path_value:
                raise ValueError(f"Missing model file setting: {key}")
            path = (self.config_path.parent / path_value).resolve()
            if not path.is_file():
                raise ValueError(f"Missing {key}: {path}")
            actual = file_hash(path)
            if actual != pinned[key]["sha256"]:
                raise ValueError(f"{key} SHA-256 does not match the pinned official model files.")
            self.files[key], self.hashes[key] = path, actual
        # Read known statistics only after verifying the pinned digest.
        self.mean, self.std = load_stats(self.files["context_statistics"])
        mask = (self.std[:132] < 1e-6).reshape(22, 6)
        if np.any(mask.any(axis=1) != mask.all(axis=1)) or np.any(self.std[132:] < 1e-6):
            raise ValueError("Unsupported partially static channels or static root positions.")
        self.static_joints = np.flatnonzero(mask.all(axis=1))
        self.context_config = load_model_output(HERE / "configs/context.json")
        self.indices = self.context_config["indices"]
        self.context = self._load(ContextTransformer, self.context_config, "context_checkpoint")
        self.detail = None
        self.detail_mean = self.detail_std = None
        if self.pipeline == "context_detail":
            with self.files["detail_statistics"].open("rb") as stream:
                stats = pickle.load(stream)["state"]
            self.detail_mean, self.detail_std = (np.asarray(stats[k], dtype=np.float64) for k in ("mean", "std"))
            if (self.detail_mean.shape != (135,) or self.detail_std.shape != (135,) or
                    not np.isfinite(self.detail_mean).all() or not np.isfinite(self.detail_std).all() or
                    np.any(self.detail_std <= 0) or not np.array_equal(self.detail_std < 1e-6, self.std < 1e-6)):
                raise ValueError("Detail state statistics are invalid or incompatible with Context static channels.")
            cfg = load_model_output(HERE / "configs/detail.json")
            if cfg["indices"] != self.indices:
                raise ValueError("Context/Detail state layouts differ.")
            self.detail = self._load(DetailTransformer, cfg, "detail_checkpoint")

    def _load(self, model_type, config, key):
        checkpoint = self.torch.load(self.files[key], map_location="cpu", weights_only=True)
        model = model_type(config["model"])
        model.load_state_dict(checkpoint["model"], strict=True)
        return model.eval()

    def input(self, path):
        calibration = load_model_output(PROJECT / "Tools/MannyLafanMapping/rest_pose_corrections_22_prototype.json")
        document, _ = read_input(path, calibration, str(self.files["context_statistics"]))
        states = np.asarray(document["vectors_tx135"], dtype=np.float64)
        if document["normalization_applied"]:
            states = denormalize(states, self.mean, self.std)
            original = document.get("unprojected_static_rotations")
            if not isinstance(original, dict) or original.get("joint_indices") != self.static_joints.tolist():
                raise ValueError("Normalized input lacks original static-joint rotations. Re-export with the updated plugin.")
            six = numbers(original.get("rotations_txjx6"), (len(states), len(self.static_joints), 6), "unprojected_static_rotations")
            decode_rotations(six)
            states[:, :132].reshape(-1, 22, 6)[:, self.static_joints] = six
        rotations = decode_rotations(states[:, :132].reshape(-1, 22, 6))
        return document, states[:, -3:].copy(), rotations

    def predict_window(self, positions, rotations, transition, post_process=True):
        from motion_inbetween.train import context_model as context
        from motion_inbetween.train import detail_model as detail
        torch = self.torch
        target = 10 + transition
        states = pack_state(rotations, positions)
        projected, channels = project_static_channels(states, self.mean, self.std)
        unclamped = projected                           # diagnostics describe the actual input
        z = (projected - self.mean) / self.std          # model-input z-scores before any clamp
        known_frames = np.r_[0:10, target:target+2]
        clamp_report = None
        if self.clamp is not None:
            projected, before = clamp_input_z(projected, self.mean, self.std, self.clamp)
            known = before[known_frames].reshape(len(known_frames), -1, 6)
            clamp_report = {"limit": self.clamp["limit"],
                            "clamped_known_values": {name: int((known[:, i] > self.clamp["limit"]).sum())
                                                     for i, name in enumerate(self.clamp["joints"])},
                            "known_max_abs_z_before": {name: float(known[:, i].max())
                                                       for i, name in enumerate(self.clamp["joints"])}}
        projected_rotations = decode_rotations(projected[:, :132].reshape(-1, 22, 6))
        position_tensor = torch.zeros(1, len(states), 22, 3, dtype=torch.float32)
        position_tensor[0, :, 0] = torch.tensor(projected[:, -3:], dtype=torch.float32)
        rotation_tensor = torch.tensor(projected_rotations[None], dtype=torch.float32)
        mean_ctx, std_ctx = (torch.tensor(v, dtype=torch.float32) for v in (self.mean, self.std))
        device = position_tensor.device
        mask_ctx = context.get_attention_mask(len(states), 10, target, device)
        contacts = None
        if self.pipeline == "context":
            new_pos, new_rot = context.evaluate(
                model=self.context, positions=position_tensor, rotations=rotation_tensor,
                seq_slice=slice(10, target), indices=self.indices, mean=mean_ctx, std=std_ctx,
                atten_mask=mask_ctx, post_process=post_process)
        else:
            if not post_process:
                raise ValueError("The Detail pipeline uses Context and Detail post_process=True.")
            new_pos, new_rot, contacts = detail.evaluate(
                detail_model=self.detail, context_model=self.context, positions=position_tensor,
                rotations=rotation_tensor, foot_contact=torch.zeros(1, len(states), 4, dtype=position_tensor.dtype, device=device),
                seq_slice=slice(10, target), indices=self.indices, mean_ctx=mean_ctx, std_ctx=std_ctx,
                mean_state=torch.tensor(self.detail_mean, dtype=torch.float32),
                std_state=torch.tensor(self.detail_std, dtype=torch.float32),
                atten_mask=detail.get_attention_mask(len(states), target, device),
                atten_mask_ctx=mask_ctx, post_process=True)
        if not torch.isfinite(new_pos).all() or not torch.isfinite(new_rot).all():
            raise ValueError("Model produced non-finite poses.")
        row, channel = np.unravel_index(np.abs(z[known_frames]).argmax(), z[known_frames].shape)
        frame = int(known_frames[row])
        return new_pos[0, :, 0].numpy().astype(np.float64), new_rot[0].numpy().astype(np.float64), {
            "projected_static_channels": channels.tolist(),
            "max_abs_context_z": float(np.max(np.abs(z))),
            "known_input_max_abs_context_z": {"window_frame": frame, "channel": int(channel),
                "value": float(unclamped[frame, channel]), "mean": float(self.mean[channel]),
                "std": float(self.std[channel]), "signed_z": float(z[frame, channel])},
            "generated_contacts": None if contacts is None else contacts[0, 10:target].tolist(),
            "input_z_clamp": clamp_report,
        }


def in_frame(positions, rotations, alignment):
    offset = np.asarray(alignment["position_offset_xz"], dtype=np.float64)
    turn = np.asarray(alignment["rotation_offset"], dtype=np.float64)
    p, r = positions.copy(), rotations.copy()
    p[:, [0, 2]] -= offset
    p = np.einsum("ij,tj->ti", turn, p)
    r[:, 0] = np.einsum("ij,tjk->tik", turn, r[:, 0])
    return p, r


def place_target(start_pos, start_rot, end_pos, end_rot, transition, policy):
    p, r = end_pos.copy(), end_rot.copy()
    if policy in ("continue_context", "endpoint_velocity"):
        # Last three observed horizontal steps; the target is N+1 intervals away.
        velocity = np.mean(np.diff(start_pos[-4:, [0, 2]], axis=0), axis=0)
        start_heading, _ = get_root_heading_offset(start_rot[-1, 0])
        end_heading, _ = get_root_heading_offset(r[0, 0])
        turn = start_heading.T @ end_heading
        if policy == "endpoint_velocity":
            end_velocity = (turn @ (p[1] - p[0]))[[0, 2]]
            velocity = (velocity + end_velocity) * .5
        desired = start_pos[-1].copy()
        desired[[0, 2]] += velocity * (transition + 1)
        desired[1] = p[0, 1]
        p = np.einsum("ij,tj->ti", turn, p - p[0]) + desired
        r[:, 0] = np.einsum("ij,tjk->tik", turn, r[:, 0])
    elif policy != "preserve_exported":
        raise ValueError("Unknown target placement.")
    return p, r


def match_horizontal_boundary_steps(positions, start_pos, end_pos, transition):
    """Blend two endpoint residuals; keep model vertical motion and rotations.

    The official constant-offset postprocess balances two residuals but does
    not enforce both. A linear residual blend preserves the learned interior
    path shape while matching the observed adjacent horizontal steps exactly.
    This is trajectory correction, not a foot-contact solver.
    """
    target = 10 + transition
    result = positions.copy()
    start_step = start_pos[-1, [0, 2]] - start_pos[-2, [0, 2]]
    end_step = end_pos[1, [0, 2]] - end_pos[0, [0, 2]]
    first = start_pos[-1, [0, 2]] + start_step - result[10, [0, 2]]
    last = end_pos[0, [0, 2]] - end_step - result[target-1, [0, 2]]
    weight = np.linspace(0., 1., transition)[:, None]
    correction = (1 - weight) * first + weight * last
    result[10:target, [0, 2]] += correction
    return result, {"policy": "linear_endpoint_residual", "start_delta_xz_cm": first.tolist(),
                    "end_delta_xz_cm": last.tolist(),
                    "max_correction_cm": float(np.linalg.norm(correction, axis=1).max())}


def slerp_matrix(a, b, weight):
    qa, qb = matrix_to_quat_xyzw(a), matrix_to_quat_xyzw(b)
    dot = float(np.dot(qa, qb))
    if dot < 0:
        qb, dot = -qb, -dot
    if dot > .9995:
        q = qa * (1 - weight) + qb * weight
    else:
        angle = np.arccos(np.clip(dot, -1, 1))
        q = (np.sin((1 - weight) * angle) * qa + np.sin(weight * angle) * qb) / np.sin(angle)
    return quat_to_matrix(dict(zip("xyzw", q / np.linalg.norm(q))))


def generate(runtime, start_path, end_path, transition, start_index=None, target_index=0):
    integer(transition, "transition_frames", *SUPPORTED_TRANSITION_RANGE)
    start, sp, sr = runtime.input(start_path)
    end, ep, er = runtime.input(end_path)
    start_index = len(sp) - 10 if start_index is None else start_index
    integer(start_index, "start_context_start_index", 0, len(sp) - 10)
    integer(target_index, "end_target_index", 0, len(ep) - 2)
    sp, sr = sp[start_index:start_index+10], sr[start_index:start_index+10]
    ep, er = ep[target_index:target_index+2], er[target_index:target_index+2]
    ep, er = reverse_start_center_root(ep, er, end["alignment"]["position_offset_xz"], end["alignment"]["rotation_offset"])
    ep, er = in_frame(ep, er, start["alignment"])
    ep, er = place_target(sp, sr, ep, er, transition, runtime.placement)
    refine = getattr(runtime, "refine", None)
    refinement_info = getattr(runtime, "refinement_info", None)
    clamp = getattr(runtime, "clamp", None)
    if refine is not None:
        parents = start["parents"]
        # The refinement is defined relative to one calibration; refuse inputs exported with another.
        for document, label in ((start, "start"), (end, "end")):
            recorded = (document.get("mapping_source_sha256") or {}).get(CALIBRATION_FILE.name)
            if recorded is not None and recorded not in refinement_info["calibration_sha256_variants"]:
                raise ValueError(f"The {label} input was exported with a different calibration than the "
                                 "frame refinement expects. Re-export it or re-estimate the refinement.")
    # Unknown frames are placeholders. Their actual predictions come from the models.
    positions = np.concatenate([sp, np.repeat(sp[-1:], transition, axis=0), ep])
    rotations = np.concatenate([sr, np.repeat(sr[-1:], transition, axis=0), er])
    if refine is not None:
        rotations = refine_rotations(rotations, refine, parents)
    centered_p, centered_r, offset, turn, _ = start_center_root(positions, rotations, 10)
    out_p, out_r, diagnostics = runtime.predict_window(centered_p, centered_r, transition)
    out_p, out_r = reverse_start_center_root(out_p, out_r, offset, turn)
    if refine is not None:
        out_r = refine_rotations(out_r, refine, parents, inverse=True)
    target = 10 + transition
    if runtime.placement == "endpoint_velocity":
        out_p, diagnostics["horizontal_boundary_correction"] = match_horizontal_boundary_steps(out_p, sp, ep, transition)
    # The model has no learned toe variation. Restore observed endpoints and
    # interpolate the original toe rotations through the generated interval.
    out_p[:10], out_r[:10] = sp, sr
    out_p[target:], out_r[target:] = ep, er
    for j in runtime.static_joints:
        for frame in range(10, target):
            out_r[frame, j] = slerp_matrix(sr[-1, j], er[0, j], (frame - 9) / (transition + 1))
    states = pack_state(out_r[:-1], out_p[:-1])
    expected_context, expected_target = pack_state(sr, sp), pack_state(er[:1], ep[:1])
    if not np.isfinite(states).all() or not np.allclose(states[:10], expected_context, atol=1e-6, rtol=0) or not np.allclose(states[-1:], expected_target, atol=1e-6, rtol=0):
        raise ValueError("Output violated the context/target pose invariant.")
    root_steps = np.linalg.norm(np.diff(out_p[:-1], axis=0), axis=-1)
    start_velocity = np.diff(sp[-4:, [0, 2]], axis=0).mean(axis=0)
    end_velocity = ep[1, [0, 2]] - ep[0, [0, 2]]
    speed_product = np.linalg.norm(start_velocity) * np.linalg.norm(end_velocity)
    velocity_angle = None if speed_product < 1e-10 else float(np.degrees(np.arccos(np.clip(np.dot(start_velocity, end_velocity) / speed_product, -1, 1))))
    diagnostics["target_placement"] = {"start_velocity_xz_cm_per_sample": start_velocity.tolist(),
        "target_next_velocity_xz_cm_per_sample": end_velocity.tolist(),
        "velocity_angle_degrees": velocity_angle,
        "displacement_xz_cm": (ep[0, [0, 2]] - sp[-1, [0, 2]]).tolist(),
        "transition_horizontal_steps_cm": np.linalg.norm(np.diff(out_p[9:target+1, [0, 2]], axis=0), axis=1).tolist()}
    largest_z = diagnostics.get("known_input_max_abs_context_z")
    if largest_z:
        channel, frame = largest_z["channel"], largest_z["window_frame"]
        largest_z.update(source="start" if frame < 10 else "destination",
            source_frame=start_index + frame if frame < 10 else target_index + frame - target,
            joint=start["joint_order"][channel//6] if channel < 132 else "Hips position",
            component=channel % 6 if channel < 132 else channel - 132)
    relative = np.swapaxes(out_r[:-2], -1, -2) @ out_r[1:-1]
    angles = np.degrees(np.arccos(np.clip((np.trace(relative, axis1=-2, axis2=-1) - 1) / 2, -1, 1)))
    diagnostics.update(max_root_step_cm=float(root_steps.max()), max_joint_step_degrees=float(angles.max()),
        context_max_abs_error=float(np.max(np.abs(states[:10] - expected_context))),
        target_max_abs_error=float(np.max(np.abs(states[-1:] - expected_target))),
        boundary_root_steps_cm=[float(root_steps[9]), float(root_steps[-1])],
        static_joint_policy="original_endpoint_slerp", static_joint_indices=runtime.static_joints.tolist())
    return {"schema": "motion_inbetweening.model_output.v1", "predictions_tx135": states.tolist(),
        "frame_count": len(states), "sample_rate_hz": 30, "normalization_applied": False,
        "position_space": "lafan_start_centered", "rotation_space": "parent_local",
        "alignment": copy.deepcopy(start["alignment"]), "joint_order": start["joint_order"], "parents": start["parents"],
        "inference": {"pipeline": runtime.pipeline, "device": "cpu", "target_placement": runtime.placement,
            "context_frames": 10, "transition_frames": transition, "internal_window_samples": 12 + transition,
            "start_context_start_index": start_index, "end_target_index": target_index,
            "alignment_reference": "original_start_input_frame_index",
            "end_target_next_index": target_index + 1, "context_post_process": True, "detail_post_process": runtime.detail is not None,
            "frame_refinement": refinement_info,
            "input_z_clamp": None if clamp is None else {"limit": clamp["limit"], "joints": clamp["joints"]},
            "model_files_sha256": runtime.hashes, "source": runtime.provenance, "config_snapshot": runtime.config,
            "config_sha256": file_hash(runtime.config_path), "python": sys.version.split()[0],
            "torch": runtime.torch.__version__, "numpy": np.__version__, "diagnostics": diagnostics}}


def run_request(request_path):
    started = time.perf_counter()
    request = load_model_output(request_path)
    if request.get("schema") != "motion_inbetweening.inference_request.v1":
        raise ValueError("Unsupported request schema.")
    if not isinstance(request.get("request_id"), str) or not request["request_id"]:
        raise ValueError("Missing request_id.")
    transition = integer(request.get("transition_frames"), "transition_frames", *SUPPORTED_TRANSITION_RANGE)
    if type(request.get("sample_rate_hz")) not in (int, float) or request["sample_rate_hz"] != 30:
        raise ValueError("sample_rate_hz must be 30.")
    integer(request.get("context_frames"), "context_frames", 10, 10)
    integer(request.get("expected_output_samples"), "expected_output_samples", 11+transition, 11+transition)
    integer(request.get("start_context_start_index"), "start_context_start_index", 0, 1000000)
    integer(request.get("end_target_index"), "end_target_index", 0, 1000000)
    paths = {}
    for key in ("start_input_file", "end_input_file", "output_file", "model_config_file"):
        if not isinstance(request.get(key), str) or not Path(request[key]).is_absolute():
            raise ValueError(f"{key} must be an absolute path.")
        paths[key] = Path(request[key]).resolve()
    output = paths["output_file"]
    if output.exists():
        raise ValueError("Refusing to overwrite an existing output; use a fresh request directory.")
    for key, label in (("start_input_file", "start"), ("end_input_file", "end")):
        if file_hash(paths[key]) != request.get("source_sha256", {}).get(label):
            raise ValueError(f"{label} input SHA-256 differs from the request.")
    runtime = Runtime(paths["model_config_file"])
    result = generate(runtime, paths["start_input_file"], paths["end_input_file"], transition,
                      start_index=request.get("start_context_start_index"), target_index=request.get("end_target_index"))
    result["inference_request_id"] = request["request_id"]
    result["inference"].update(request_snapshot=request, elapsed_seconds=time.perf_counter()-started)
    if output.exists():
        raise ValueError("Output appeared during inference; refusing to overwrite.")
    save_json(output, result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", required=True)
    args = parser.parse_args()
    try:
        result = run_request(Path(args.request).resolve())
    except Exception as error:
        print(f"Inference failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps({"success": True, "pipeline": result["inference"]["pipeline"], "frame_count": result["frame_count"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
