"""Unreal-side adapter for the team's unmodified Manny exporter."""
import json
from pathlib import Path
import runpy


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
    unreal.log("MIB_MANNY_SOURCE_READY=" + str(output))
