"""Generate Baseline (legacy adapter) and Improved (given config) transitions for clip pairs.

Uses the plugin's default request: context = last 10 of the first 31 exported samples, target =
destination sample 0, N = --length. Each result is mapped to the 89-bone Manny pose with the
plugin's Scripts/prepare_manny_output.py and listed in <batch>/transitions/summary.json, which
ue_import_manny_poses.py turns into Animation Sequences.
  python make_comparison_transitions.py --config <model_config.local.json> [--batch-dir <folder>]
"""
import argparse
import json
import shutil
import sys

import numpy as np

from common import PROJECT, batch_dir, infer, legacy_config
import metrics

sys.path.insert(0, str(PROJECT / "Plugins/MotionInbetweeningHello/Scripts"))
from prepare_manny_output import prepare as prepare_output  # noqa: E402

PAIRS = [("MM_Idle", "MM_Walk_Fwd", "IdleToWalk"), ("MM_Walk_Fwd", "MM_Run_Fwd", "WalkToRun"),
         ("MM_Run_Fwd", "MM_Walk_Fwd", "RunToWalk"), ("MM_Walk_Fwd", "MM_Idle", "WalkToIdle"),
         ("MM_Idle", "MM_Run_Fwd", "IdleToRun"), ("MM_Run_Fwd", "MM_Idle", "RunToIdle"),
         ("MM_Run_Fwd", "MM_Jump", "RunToJump"), ("MM_Land", "MM_Walk_Fwd", "LandToWalk")]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", required=True)
    parser.add_argument("--batch-dir")
    parser.add_argument("--length", type=int, default=20)
    args = parser.parse_args()
    folder = batch_dir(args.batch_dir)
    clips, out = folder / "clips", folder / "transitions"
    offsets = metrics.offsets_from_skeleton(clips / "skeleton.json")
    ground_levels = []                     # per foot joint: lowest height in the source walk/run clips
    for name in ("MM_Walk_Fwd", "MM_Run_Fwd"):
        positions, rotations = metrics.load_states(clips / name / "model_input.json")
        ground_levels.append(metrics.fk(rotations, positions, offsets)[1][:, metrics.FOOT, 1].min(axis=0))
    ground = np.min(ground_levels, axis=0)
    if out.exists():
        shutil.rmtree(out)
    variants = {"Baseline": legacy_config(args.config, folder / "config_legacy.json"), "Improved": args.config}
    summary = []
    for variant, config in variants.items():
        runtime = infer.Runtime(config)
        for start_clip, end_clip, label in PAIRS:
            start, end = clips / start_clip / "model_input.json", clips / end_clip / "model_input.json"
            start_index = min(json.loads(start.read_text(encoding="utf-8"))["sample_count"], 31) - 10
            result = infer.generate(runtime, start, end, args.length, start_index=start_index, target_index=0)
            target = out / variant / label
            target.mkdir(parents=True)
            infer.save_json(target / "source_model_output.json", result)
            shutil.copy(clips / "skeleton.json", target / "skeleton.json")
            mapped = prepare_output({"output_directory": str(target), "mapping_directory": str(PROJECT / "Tools/MannyLafanMapping"),
                                     "source_file": str(target / "source_model_output.json"), "statistics_file": ""})
            if not mapped.get("success"):
                raise RuntimeError(f"Inverse mapping failed for {variant}/{label}: {mapped}")
            p, r = metrics.decode(result["predictions_tx135"])
            m = metrics.transition_metrics(p, r, 10, 10 + args.length, offsets, ground)
            diagnostics = result["inference"]["diagnostics"]
            summary.append({"variant": variant, "pair": label, "start": start_clip, "end": end_clip,
                            "length": args.length, "start_index": start_index, "frames": result["frame_count"],
                            "boundary_correction_cm": diagnostics["horizontal_boundary_correction"]["max_correction_cm"],
                            **m, "manny_pose": str(target / "manny_pose.json")})
    (out / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"{'pair':12s} {'variant':9s} {'boundary corr cm':>16s} {'foot slide cm/f':>15s} {'speed reversals':>15s} {'joint acc deg':>13s} {'min toe cm':>10s}")
    for row in sorted(summary, key=lambda r: (r["pair"], r["variant"])):
        print(f"{row['pair']:12s} {row['variant']:9s} {row['boundary_correction_cm']:16.2f} {row['foot_slide_cm_per_frame']:15.2f} "
              f"{row['root_speed_reversals']:15d} {row['joint_acceleration_deg']:13.2f} {row['min_toe_height_cm']:10.2f}")


if __name__ == "__main__":
    main()
