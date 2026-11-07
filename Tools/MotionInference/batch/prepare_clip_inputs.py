"""Convert every <batch>/clips/<name>/source_manny.json into an unnormalized model_input.json.

Calls the plugin's Scripts/prepare_manny_input.py (team mapper, unmodified). Continuity limits are
wider than the plugin defaults (15 cm / 60 deg) because jump and fall clips move faster; they are
diagnostics, not model inputs.
  python prepare_clip_inputs.py [--batch-dir <folder>]
"""
import argparse
import json

from common import PROJECT, batch_dir
import sys

sys.path.insert(0, str(PROJECT / "Plugins/MotionInbetweeningHello/Scripts"))
from prepare_manny_input import prepare  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--batch-dir")
    parser.add_argument("--max-root-step-cm", type=float, default=40.0)
    parser.add_argument("--max-joint-step-degrees", type=float, default=90.0)
    args = parser.parse_args()
    clips = batch_dir(args.batch_dir) / "clips"
    for folder in sorted(p for p in clips.iterdir() if (p / "source_manny.json").is_file()):
        job = {"output_directory": str(folder), "mapping_directory": str(PROJECT / "Tools/MannyLafanMapping"),
               "max_root_step_cm": args.max_root_step_cm, "max_joint_step_degrees": args.max_joint_step_degrees,
               "statistics_file": ""}
        try:
            result = prepare(job)
            validation = json.loads((folder / "model_input.json").read_text(encoding="utf-8"))["validation"]
            print(f"{folder.name:20s} {result['sample_count']:4d} samples  max root step "
                  f"{validation['max_root_step_cm']:6.2f} cm  max joint step {validation['max_joint_step_angle_degrees']:6.2f} deg")
        except Exception as error:
            print(f"{folder.name:20s} FAILED: {error}")


if __name__ == "__main__":
    main()
