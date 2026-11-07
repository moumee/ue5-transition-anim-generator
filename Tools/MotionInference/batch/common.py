"""Helpers shared by the batch tools: locating the batch folder and deriving the legacy config."""
from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
MOTION = HERE.parent
PROJECT = MOTION.parents[1]
sys.path[:0] = [str(MOTION)]
import infer  # noqa: E402

DEFAULT_BATCH_DIR = PROJECT / "Saved/MotionInbetweening/Batch"


def batch_dir(value=None):
    path = Path(value or os.environ.get("MIB_BATCH_DIR") or DEFAULT_BATCH_DIR).resolve()
    path.mkdir(parents=True, exist_ok=True)
    return path


def legacy_config(config_path, destination):
    """Same model files, frame refinement and input clamp disabled (pre-refinement adapter)."""
    config_path = Path(config_path).resolve()
    config = json.loads(config_path.read_text(encoding="utf-8-sig"))
    legacy = copy.deepcopy(config)
    for key in ("context_checkpoint", "context_statistics", "detail_checkpoint", "detail_statistics"):
        if isinstance(legacy.get(key), str):
            legacy[key] = str((config_path.parent / legacy[key]).resolve())
    legacy["frame_refinement"] = None
    legacy["input_z_clamp"] = None
    destination = Path(destination)
    destination.write_text(json.dumps(legacy, ensure_ascii=False, indent=2), encoding="utf-8")
    return destination


def clip_inputs(clips_dir):
    return sorted(Path(clips_dir).glob("*/model_input.json"))
