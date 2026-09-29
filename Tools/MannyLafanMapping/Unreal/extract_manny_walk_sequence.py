"""Export up to one second of the real UE5 Manny forward-walk animation."""

import json
import math
import os
import traceback

import unreal


ANIM_PATH = "/Game/ControlRig/Characters/Mannequins/Animations/Manny/MM_Walk_Fwd"
MESH_PATH = "/Game/ControlRig/Characters/Mannequins/Meshes/SKM_Manny_Simple"
SAMPLE_RATE_HZ = 30.0
MAX_SAMPLES = 31
OUTPUT_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "manny_walk_fwd_sequence.json"
)

MANNY_BONES = [
    "pelvis", "thigh_l", "calf_l", "foot_l", "ball_l",
    "thigh_r", "calf_r", "foot_r", "ball_r",
    "spine_01", "spine_02", "spine_03", "neck_01", "head",
    "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l",
    "clavicle_r", "upperarm_r", "lowerarm_r", "hand_r",
]


def vector(value):
    return [value.x, value.y, value.z]


def quaternion(value):
    return [value.x, value.y, value.z, value.w]


def transform(value):
    return {
        "translation": vector(value.get_editor_property("translation")),
        "rotation_xyzw": quaternion(value.get_editor_property("rotation")),
        "scale": vector(value.get_editor_property("scale3d")),
    }


def export_animation(anim_path, output_path, sample_rate_hz=SAMPLE_RATE_HZ, max_samples=MAX_SAMPLES):
    animation = unreal.EditorAssetLibrary.load_asset(anim_path)
    mesh = unreal.EditorAssetLibrary.load_asset(MESH_PATH)
    if animation is None or mesh is None:
        raise RuntimeError("Could not load the forward-walk animation or Manny mesh")

    options = unreal.AnimPoseEvaluationOptions()
    options.set_editor_property("optional_skeletal_mesh", mesh)
    options.set_editor_property("should_retarget", False)
    options.set_editor_property("extract_root_motion", False)
    options.set_editor_property("incorporate_root_motion_into_pose", True)
    options.set_editor_property("retrieve_additive_as_full_pose", True)

    play_length = float(animation.get_play_length())
    sample_count = min(max_samples, int(math.floor(play_length * sample_rate_hz)) + 1)
    if sample_count < 2:
        raise RuntimeError("Walk animation is too short to sample")

    frames = []
    sample_times = []
    for sample_index in range(sample_count):
        time_seconds = min(sample_index / sample_rate_hz, play_length)
        pose = animation.get_anim_pose_at_time(time_seconds, options)
        pose_names = {str(name) for name in pose.get_bone_names()}
        missing = [name for name in MANNY_BONES if name not in pose_names]
        if missing:
            raise RuntimeError("Mapped bones missing: " + ", ".join(missing))

        bones = []
        for name in MANNY_BONES:
            bones.append({
                "name": name,
                "local": transform(pose.get_bone_pose(
                    unreal.Name(name), unreal.AnimPoseSpaces.LOCAL
                )),
                "world": transform(pose.get_bone_pose(
                    unreal.Name(name), unreal.AnimPoseSpaces.WORLD
                )),
            })
        frames.append({"frame_index": sample_index, "time_seconds": time_seconds, "bones": bones})
        sample_times.append(time_seconds)

    result = {
        "engine_version": unreal.SystemLibrary.get_engine_version(),
        "animation": anim_path,
        "skeletal_mesh": MESH_PATH,
        "sample_rate_hz": sample_rate_hz,
        "sample_times_seconds": sample_times,
        "frame_indices": list(range(sample_count)),
        "frame_count": sample_count,
        "play_length_seconds": play_length,
        "bones_per_frame": len(MANNY_BONES),
        "frames": frames,
    }
    with open(output_path, "w", encoding="utf-8") as handle:
        json.dump(result, handle, ensure_ascii=False, indent=2)
    unreal.log("MANNY_ANIMATION_EXPORT_OK=" + output_path)


if __name__ == "__main__":
    try:
        export_animation(ANIM_PATH, OUTPUT_PATH)
    except Exception:
        unreal.log_error("MANNY_ANIMATION_EXPORT_FAILED\n" + traceback.format_exc())
        raise
