"""Unreal-side: create Animation Sequences from <batch>/transitions/summary.json (manny_pose.json files).

Mirrors Plugins/MotionInbetweeningHello/Source/.../MotionInbetweeningAnimSequence.cpp: SK_Mannequin skeleton,
SKM_Manny_Simple preview, 30 fps, one track per reference bone, root motion off. Assets go to
/Game/MotionInbetweening/<Baseline|Improved>/AN_<Pair>_N<length>_<variant>; a previous asset with the
same name in those folders is replaced. Each asset is re-evaluated after saving and compared with the JSON.
  $env:MIB_BATCH_DIR = "<folder>"   # optional
  UnrealEditor-Cmd.exe <project>.uproject -run=pythonscript -script=<this file> -unattended -nullrhi
"""
import json
import math
import os
import traceback

import unreal

PROJECT = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
BATCH = os.environ.get("MIB_BATCH_DIR") or os.path.join(PROJECT, "Saved", "MotionInbetweening", "Batch")
SUMMARY = os.path.join(BATCH, "transitions", "summary.json")
REPORT = os.path.join(BATCH, "transitions", "ue_import_report.json")
MESH_PATH = "/Game/ControlRig/Characters/Mannequins/Meshes/SKM_Manny_Simple"
ROOT_FOLDER = "/Game/MotionInbetweening"


def create(entry, mesh, skeleton):
    frames = json.load(open(entry["manny_pose"], encoding="utf-8"))["full_local_pose_frames"]
    folder = f"{ROOT_FOLDER}/{entry['variant']}"
    name = f"AN_{entry['pair']}_N{entry['length']}_{entry['variant']}"
    if unreal.EditorAssetLibrary.does_asset_exist(f"{folder}/{name}"):
        unreal.EditorAssetLibrary.delete_asset(f"{folder}/{name}")
    factory = unreal.AnimSequenceFactory()
    factory.set_editor_property("target_skeleton", skeleton)
    factory.set_editor_property("preview_skeletal_mesh", mesh)
    sequence = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, unreal.AnimSequence, factory)
    sequence.set_editor_property("enable_root_motion", False)
    controller = sequence.controller              # IAnimationDataController (UE 5.8 Python)
    if hasattr(controller, "initialize_model"):
        controller.initialize_model()
    controller.open_bracket(unreal.Text("Populate Manny transition"), False)
    controller.set_frame_rate(unreal.FrameRate(30, 1), False)
    controller.set_number_of_frames(unreal.FrameNumber(len(frames) - 1), False)
    bones = [bone["name"] for bone in frames[0]["bones"]]
    for index, bone in enumerate(bones):
        keys = [frame["bones"][index] for frame in frames]
        controller.add_bone_curve(unreal.Name(bone), False)
        controller.set_bone_track_keys(
            unreal.Name(bone),
            [unreal.Vector(*k["local_translation_cm"]) for k in keys],
            [unreal.Quat(*k["local_rotation_xyzw"]) for k in keys],
            [unreal.Vector(*k["local_scale"]) for k in keys], False)
    if hasattr(controller, "notify_populated"):   # C++-only in UE 5.8; closing the bracket finalizes
        controller.notify_populated()
    controller.close_bracket(False)
    unreal.EditorAssetLibrary.save_loaded_asset(sequence, False)

    options = unreal.AnimPoseEvaluationOptions()
    options.set_editor_property("optional_skeletal_mesh", mesh)
    worst_t = worst_r = 0.0
    for frame_index in (0, len(frames) // 2, len(frames) - 1):
        pose = sequence.get_anim_pose_at_time(frame_index / 30.0, options)
        for data in frames[frame_index]["bones"]:
            local = unreal.AnimPoseExtensions.get_bone_pose(pose, unreal.Name(data["name"]), unreal.AnimPoseSpaces.LOCAL)
            t, q = local.get_editor_property("translation"), local.get_editor_property("rotation")
            worst_t = max(worst_t, math.dist((t.x, t.y, t.z), data["local_translation_cm"]))
            dot = abs(sum(a * b for a, b in zip((q.x, q.y, q.z, q.w), data["local_rotation_xyzw"])))
            worst_r = max(worst_r, 2 * math.degrees(math.acos(min(1.0, dot))))
    return {"asset": sequence.get_path_name(), "frames": len(frames), "tracks": len(bones),
            "play_length": float(sequence.get_play_length()),
            "verify_max_translation_error_cm": worst_t, "verify_max_rotation_error_deg": worst_r}


def main():
    mesh = unreal.EditorAssetLibrary.load_asset(MESH_PATH)
    skeleton = mesh.get_editor_property("skeleton")
    report = []
    for entry in json.load(open(SUMMARY, encoding="utf-8")):
        try:
            item = create(entry, mesh, skeleton)
        except Exception:
            item = {"pair": entry["pair"], "variant": entry["variant"], "error": traceback.format_exc()}
            unreal.log_error("MIB_ASSET_FAILED " + json.dumps(item))
        report.append(item)
        unreal.log("MIB_ASSET " + json.dumps(item))
    with open(REPORT, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=1)
    unreal.log("MIB_IMPORT_DONE " + REPORT)


main()
