"""Unreal-side adapter for the team's unmodified Manny exporter."""
import json
import math
from pathlib import Path
import runpy

FULL_POSE_MAX_SAMPLES = 600  # 20 seconds at 30 fps


def export_full_pose(animation, mesh, output_path, sample_rate_hz=30.0, max_samples=FULL_POSE_MAX_SAMPLES):
    """Write the local translation and rotation of every skeleton bone for the whole clip.

    The team exporter writes the 22 mapped bones only. The output stage takes the remaining bones (fingers, twist,
    upper spine) from this file, and needs the whole clip rather than the model's 31 samples to follow a loop
    past its end.
    """
    import unreal

    options = unreal.AnimPoseEvaluationOptions()
    options.set_editor_property("optional_skeletal_mesh", mesh)
    options.set_editor_property("should_retarget", False)
    options.set_editor_property("extract_root_motion", False)
    options.set_editor_property("incorporate_root_motion_into_pose", True)
    options.set_editor_property("retrieve_additive_as_full_pose", True)
    names, pending = [], [unreal.Name("root")]       # the mesh's bones (the skeleton asset also lists corrective bones)
    while pending:
        current = pending.pop(0)
        names.append(current)
        pending.extend(list(mesh.get_bone_children(current)))
    play_length = float(animation.get_play_length())
    times = [index / sample_rate_hz for index in range(min(max_samples, int(math.floor(play_length * sample_rate_hz)) + 1))]
    samples = []
    for time_seconds in times:
        pose = animation.get_anim_pose_at_time(time_seconds, options)
        row = []
        for name in names:
            local = unreal.AnimPoseExtensions.get_bone_pose(pose, name, unreal.AnimPoseSpaces.LOCAL)
            t, q = local.get_editor_property("translation"), local.get_editor_property("rotation")
            row.append([t.x, t.y, t.z, q.x, q.y, q.z, q.w])
        samples.append(row)
    Path(output_path).write_text(json.dumps({
        "sample_rate_hz": sample_rate_hz, "play_length_seconds": play_length, "sample_times_seconds": times,
        "bones": [str(name) for name in names],
        "layout": "per sample, per bone: local translation cm xyz, local rotation xyzw", "local": samples}), encoding="utf-8")


def export_job(job_path):
    import unreal

    job = json.loads(Path(job_path).read_text(encoding="utf-8-sig"))
    directory = Path(job["output_directory"])
    exporter_path = Path(job["mapping_directory"]) / "Unreal" / "extract_manny_walk_sequence.py"
    exporter = runpy.run_path(str(exporter_path), run_name="mib_team_exporter")
    animation = unreal.EditorAssetLibrary.load_asset(job["animation"])
    mesh = unreal.EditorAssetLibrary.load_asset(exporter["MESH_PATH"])
    if animation is None or mesh is None:
        raise ValueError("The selected animation or Manny mesh could not be loaded.")
    if animation.get_editor_property("skeleton") != mesh.get_editor_property("skeleton"):
        raise ValueError("Select an Animation Sequence using the Manny skeleton.")

    output = directory / "source_manny.json"
    exporter["export_animation"](
        job["animation"], str(output), sample_rate_hz=30.0, max_samples=int(job["max_samples"])
    )
    source = json.loads(output.read_text(encoding="utf-8"))
    if source["frame_count"] < 10:
        raise ValueError("At least 10 samples at 30 fps are required for the model context.")
    export_full_pose(animation, mesh, directory / "full_local_pose.json")
    unreal.log("MIB_MANNY_SOURCE_READY=" + str(output))
