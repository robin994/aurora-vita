#!/usr/bin/env python3
"""Compare sampled FRAME telemetry; never treat 120-frame averages as frames."""
import argparse
import json
import re
from pathlib import Path


def read_frames(path, warmup=0):
    frames = []
    seen = set()
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if "[AURORA-VITA][FRAME]" not in line:
            continue
        fields = dict(re.findall(r"\b([a-z_]+)=([0-9]+)\b", line))
        if "frame" not in fields or "total_us" not in fields:
            raise ValueError(f"incomplete FRAME record in {path}")
        record = {key: int(value) for key, value in fields.items()}
        index = record["frame"]
        if index in seen or (frames and index <= frames[-1]["frame"]):
            raise ValueError(f"multiple or unordered sessions in {path}; split the log")
        seen.add(index)
        frames.append(record)
    frames = frames[warmup:]
    if not frames:
        raise ValueError(f"no FRAME samples after warm-up in {path}; phase averages are not accepted")
    return frames


def percentile(values, quantile):
    ordered = sorted(values)
    rank = (len(ordered) - 1) * quantile
    low = int(rank)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def summarize(frames, identity):
    times = [frame["total_us"] for frame in frames]
    gaps = [b["frame"] - a["frame"] for a, b in zip(frames, frames[1:])]
    counters = ("draws", "vertices", "indices", "pipeline_translations", "vertex_translations",
                "fragment_translations", "texture_resolves", "batch_candidates", "batch_merged")
    return {
        "identity": identity,
        "sample_count": len(frames),
        "first_frame": frames[0]["frame"],
        "last_frame": frames[-1]["frame"],
        "frame_gaps": sorted(set(gaps)),
        "consecutive_samples": bool(gaps) and all(gap == 1 for gap in gaps),
        "frame_us": {"median": percentile(times, .5), "p95": percentile(times, .95),
                     "p99": percentile(times, .99), "max": max(times)},
        "mean_counters": {key: sum(frame[key] for frame in frames) / len(frames)
                          for key in counters if all(key in frame for frame in frames)},
    }


def compare(reference, candidate):
    result = {}
    for name, value in reference["frame_us"].items():
        after = candidate["frame_us"][name]
        result[name] = {"delta_us": after - value,
                        "delta_percent": (after / value - 1) * 100 if value else None}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--reference-id", required=True, help="build/hash, title, scene and config")
    parser.add_argument("--candidate-id", required=True, help="build/hash, title, scene and config")
    parser.add_argument("--warmup", type=int, default=0, help="initial logged samples to exclude in each run")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.warmup < 0:
        parser.error("warmup must be non-negative")
    try:
        reference = summarize(read_frames(args.reference, args.warmup), args.reference_id)
        candidate = summarize(read_frames(args.candidate, args.warmup), args.candidate_id)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    report = {"reference": reference, "candidate": candidate,
              "change": compare(reference, candidate),
              "scope": "CPU wall time of logged samples; identical capture settings required; no visual proof",
              "percentile_method": "linear interpolation over sample ranks"}
    rendered = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")


if __name__ == "__main__":
    main()
