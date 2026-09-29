import json
import os
import traceback

import unreal


MESH_PATH = "/Game/ControlRig/Characters/Mannequins/Meshes/SKM_Manny_Simple"
SKELETON_PATH = "/Game/ControlRig/Characters/Mannequins/Meshes/SK_Mannequin"
OUTPUT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "manny_skeleton_ue58.json")


def vec(value):
    return {"x": value.x, "y": value.y, "z": value.z}


def quat(value):
    return {"x": value.x, "y": value.y, "z": value.z, "w": value.w}


def transform_dict(value):
    translation = value.get_editor_property("translation")
    rotation = value.get_editor_property("rotation")
    scale = value.get_editor_property("scale3d")
    return {
        "translation_cm": vec(translation),
        "rotation_quaternion": quat(rotation),
        "scale": vec(scale),
    }


def main():
    mesh = unreal.EditorAssetLibrary.load_asset(MESH_PATH)
    skeleton = unreal.EditorAssetLibrary.load_asset(SKELETON_PATH)
    if mesh is None:
        raise RuntimeError("Could not load skeletal mesh: " + MESH_PATH)
    if skeleton is None:
        raise RuntimeError("Could not load skeleton: " + SKELETON_PATH)

    pose = unreal.AnimPoseExtensions.get_reference_pose(skeleton)
    skeleton_bone_names = list(unreal.AnimPoseExtensions.get_bone_names(pose))

    # The Skeleton asset contains corrective bones used by the full Manny mesh.
    # SKM_Manny_Simple only contains a subset, so traverse the mesh hierarchy
    # itself instead of assuming every Skeleton bone belongs to this mesh.
    bone_names = []
    pending = [unreal.Name("root")]
    while pending:
        current = pending.pop(0)
        bone_names.append(current)
        pending.extend(list(mesh.get_bone_children(current)))

    bones = []
    for index, bone_name in enumerate(bone_names):
        parent_name = mesh.get_bone_parent(bone_name)
        local_transform = unreal.AnimPoseExtensions.get_bone_pose(
            pose, bone_name, unreal.AnimPoseSpaces.LOCAL
        )
        bones.append(
            {
                "index": index,
                "name": str(bone_name),
                "parent": str(parent_name) if str(parent_name) not in ("None", "") else None,
                "reference_local_transform": transform_dict(local_transform),
            }
        )

    result = {
        "engine_version": unreal.SystemLibrary.get_engine_version(),
        "skeletal_mesh": MESH_PATH,
        "skeleton": SKELETON_PATH,
        "bone_count": len(bones),
        "skeleton_asset_bone_count": len(skeleton_bone_names),
        "bones": bones,
    }
    with open(OUTPUT_PATH, "w", encoding="utf-8") as output_file:
        json.dump(result, output_file, ensure_ascii=False, indent=2)
    unreal.log("MANNY_EXPORT_OK=" + OUTPUT_PATH)


try:
    main()
except Exception:
    unreal.log_error("MANNY_EXPORT_FAILED\n" + traceback.format_exc())
    raise
