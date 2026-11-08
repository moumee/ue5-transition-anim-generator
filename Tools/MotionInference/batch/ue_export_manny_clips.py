"""Unreal-side batch export of every Manny-skeleton Animation Sequence and the reference skeleton.

Run headless (editor closed):
  $env:MIB_BATCH_DIR = "<folder>"   # optional, default <project>/Saved/MotionInbetweening/Batch
  UnrealEditor-Cmd.exe <project>.uproject -run=pythonscript -script=<this file> -unattended -nullrhi
Writes <batch>/clips/<AnimName>/source_manny.json (all frames at 30 fps), <batch>/clips/skeleton.json and
<batch>/clips/manifest.json. Uses the team's unmodified exporter exactly like Scripts/export_manny_input.py.
"""
import json
import os
import runpy
import traceback

import unreal

PROJECT = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
BATCH = os.environ.get("MIB_BATCH_DIR") or os.path.join(PROJECT, "Saved", "MotionInbetweening", "Batch")
OUT = os.path.join(BATCH, "clips")
MESH_PATH = "/Game/ControlRig/Characters/Mannequins/Meshes/SKM_Manny_Simple"
MAX_SAMPLES = 600  # 20 seconds at 30 fps


def vec(v):
    return {"x": v.x, "y": v.y, "z": v.z}


def quat(q):
    return {"x": q.x, "y": q.y, "z": q.z, "w": q.w}


def export_skeleton(mesh, path):
    skeleton = mesh.get_editor_property("skeleton")
    pose = unreal.AnimPoseExtensions.get_reference_pose(skeleton)
    names, pending = [], [unreal.Name("root")]
    while pending:                                   # parents before children
        current = pending.pop(0)
        names.append(current)
        pending.extend(list(mesh.get_bone_children(current)))
    bones = []
    for index, name in enumerate(names):
        parent = mesh.get_bone_parent(name)
        t = unreal.AnimPoseExtensions.get_bone_pose(pose, name, unreal.AnimPoseSpaces.LOCAL)
        bones.append({"index": index, "name": str(name),
                      "parent": str(parent) if str(parent) not in ("None", "") else None,
                      "reference_local_transform": {"translation_cm": vec(t.get_editor_property("translation")),
                                                    "rotation_quaternion": quat(t.get_editor_property("rotation")),
                                                    "scale": vec(t.get_editor_property("scale3d"))}})
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"engine_version": unreal.SystemLibrary.get_engine_version(), "skeletal_mesh": mesh.get_path_name(),
                   "skeleton": skeleton.get_path_name(), "bone_count": len(bones), "bones": bones}, f, indent=1)


def main():
    os.makedirs(OUT, exist_ok=True)
    exporter = runpy.run_path(os.path.join(PROJECT, "Tools/MannyLafanMapping/Unreal/extract_manny_walk_sequence.py"),
                              run_name="mib_team_exporter")
    mesh = unreal.EditorAssetLibrary.load_asset(MESH_PATH)
    skeleton = mesh.get_editor_property("skeleton")
    export_skeleton(mesh, os.path.join(OUT, "skeleton.json"))
    manifest = []
    for data in unreal.AssetRegistryHelpers.get_asset_registry().get_assets_by_path("/Game", recursive=True):
        if str(data.asset_class_path.asset_name) != "AnimSequence":
            continue
        path = str(data.package_name)
        if "/Rigs/Poses/" in path or path.startswith("/Game/MotionInbetweening/"):
            continue                                 # Control Rig pose clips and our own outputs
        entry = {"asset": path, "name": str(data.asset_name)}
        try:
            anim = unreal.EditorAssetLibrary.load_asset(path)    # broken assets raise here
            if anim.get_editor_property("skeleton") != skeleton:
                entry["skipped"] = "different skeleton"
            else:
                folder = os.path.join(OUT, entry["name"])
                os.makedirs(folder, exist_ok=True)
                output = os.path.join(folder, "source_manny.json")
                exporter["export_animation"](path, output, sample_rate_hz=30.0, max_samples=MAX_SAMPLES)
                entry.update(play_length=float(anim.get_play_length()), source=output)
        except Exception:
            entry["skipped"] = traceback.format_exc().strip().splitlines()[-1]
        manifest.append(entry)
        unreal.log("MIB_CLIP " + json.dumps(entry))
    with open(os.path.join(OUT, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=1)
    unreal.log("MIB_EXPORT_DONE " + OUT)


try:
    main()
except Exception:
    unreal.log_error("MIB_EXPORT_FAILED\n" + traceback.format_exc())
    raise
