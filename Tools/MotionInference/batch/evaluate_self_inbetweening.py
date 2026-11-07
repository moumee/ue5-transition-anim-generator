"""Self in-betweening benchmark on exported Manny clips (ground truth is known).

For every clip, hide N frames between a 10-frame context and a target taken from the same clip,
let the model fill them, and compare with the original frames and with plain interpolation
(root lerp + local SLERP, the RMI/LaFAN1 baseline). Compares the legacy adapter (frame refinement
and input clamp disabled) with the given config (defaults: refinement + clamp on).
  python evaluate_self_inbetweening.py --config <model_config.local.json> [--batch-dir <folder>]
Clips whose name starts with --holdout-prefix (default MF_ = Quinn) are reported separately because
they were not used to estimate configs/manny_frame_refinement_v1.json.
"""
import argparse
import json

import numpy as np

from common import batch_dir, clip_inputs, infer, legacy_config
import metrics


def windows(frames, length, per_clip):
    last = frames - (12 + length)
    if last < 0:
        return []
    return sorted(set(np.linspace(0, last, min(per_clip, last + 1)).round().astype(int).tolist()))


def evaluate(runtime, path, truth, length, starts, offsets):
    rows = []
    true_p, true_r = truth
    for s in starts:
        t = s + 10 + length
        result = infer.generate(runtime, path, path, length, start_index=s, target_index=t)
        p, r = metrics.decode(result["predictions_tx135"])
        rows.append(metrics.reconstruction_errors(p, r, true_p[s:t + 1], true_r[s:t + 1], slice(10, 10 + length), offsets))
    return rows


def interpolation(truth, length, starts, offsets):
    rows = []
    true_p, true_r = truth
    for s in starts:
        t = s + 10 + length
        p, r = true_p[s:t + 1].copy(), true_r[s:t + 1].copy()
        for k in range(1, length + 1):
            w = k / (length + 1)
            p[9 + k] = (1 - w) * true_p[s + 9] + w * true_p[t]
            r[9 + k] = np.stack([infer.slerp_matrix(true_r[s + 9, j], true_r[t, j], w) for j in range(22)])
        rows.append(metrics.reconstruction_errors(p, r, true_p[s:t + 1], true_r[s:t + 1], slice(10, 10 + length), offsets))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", required=True)
    parser.add_argument("--batch-dir")
    parser.add_argument("--lengths", type=int, nargs="+", default=[10, 15, 20])
    parser.add_argument("--windows-per-clip", type=int, default=9)
    parser.add_argument("--holdout-prefix", default="MF_")
    args = parser.parse_args()
    folder = batch_dir(args.batch_dir)
    offsets = metrics.offsets_from_skeleton(folder / "clips/skeleton.json")
    configs = {"legacy": legacy_config(args.config, folder / "config_legacy.json"), "refined": args.config}
    runtimes = {}
    for name, path in configs.items():
        runtimes[name] = infer.Runtime(path)
        runtimes[name].placement = "preserve_exported"   # the target stays where it is in the clip
    results = []
    for path in clip_inputs(folder / "clips"):
        truth = metrics.load_states(path)
        for length in args.lengths:
            starts = windows(len(truth[0]), length, args.windows_per_clip)
            if not starts:
                continue
            for name, runtime in runtimes.items():
                for row in evaluate(runtime, path, truth, length, starts, offsets):
                    results.append({"clip": path.parent.name, "length": length, "method": name, **row})
            for row in interpolation(truth, length, starts, offsets):
                results.append({"clip": path.parent.name, "length": length, "method": "interpolation", **row})
    (folder / "evaluation.json").write_text(json.dumps(results, indent=1), encoding="utf-8")
    groups = {"all clips": lambda c: True, f"held-out ({args.holdout_prefix}*)": lambda c: c.startswith(args.holdout_prefix),
              "estimation set": lambda c: not c.startswith(args.holdout_prefix),
              "walk/run": lambda c: "Walk" in c or "Run" in c}
    keys = ["position_cm", "root_cm", "hands_cm", "rotation_deg_legs", "rotation_deg_arms", "rotation_deg_clavicles"]
    for label, keep in groups.items():
        chosen = [r for r in results if keep(r["clip"])]
        if not chosen:
            continue
        base = np.mean([r["position_cm"] for r in chosen if r["method"] == "interpolation"])
        print(f"\n== {label} (lengths {args.lengths})")
        print(f"{'method':14s} " + " ".join(f"{k:>22s}" for k in keys) + "   position / interpolation")
        for method in ("interpolation", "legacy", "refined"):
            rows = [r for r in chosen if r["method"] == method]
            means = {k: float(np.mean([r[k] for r in rows])) for k in keys}
            print(f"{method:14s} " + " ".join(f"{means[k]:22.2f}" for k in keys) + f"   {means['position_cm'] / base:.3f}")


if __name__ == "__main__":
    main()
