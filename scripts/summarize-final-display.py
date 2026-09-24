"""最終実表示試行を同一PTSのdecoder入力・出力とCPU段階で判定する。"""

import argparse
import csv
import json
import re
from collections import defaultdict
from pathlib import Path


def per_loop_pts(path):
    rows = defaultdict(set)
    last_pts = {}
    loop = defaultdict(int)
    with path.open(newline="", encoding="utf-8-sig") as stream:
        for row in csv.DictReader(stream):
            stage = row["stage"]
            if stage not in ("compressed", "decoded") or not row["pts_ns"]:
                continue
            pts = int(row["pts_ns"])
            if stage in last_pts and pts < last_pts[stage]:
                loop[stage] += 1
            last_pts[stage] = pts
            rows[stage].add((loop[stage], pts))
    return rows


def cpu_stages(path, source_frames):
    result = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "CPU_STAGE seq=" not in line:
            continue
        fields = dict(re.findall(r"([a-z_]+)=([0-9.]+)", line.split("CPU_STAGE ", 1)[1]))
        seq = int(fields["seq"])
        result[(seq // source_frames, int(fields["pts_ns"]))] = {
            "seq": seq,
            "lock_ms": float(fields["retire_device_lock_ms"]),
            "flush_ms": float(fields["retire_flush_ms"]),
            "map_ms": float(fields["retire_map_ms"]),
        }
    return result


def window_stats(path):
    with path.open(newline="", encoding="utf-8-sig") as stream:
        rows = list(csv.DictReader(stream))
    observed = [row for row in rows if row["window_found"] == "1"]
    return {
        "present_rows": len(rows),
        "window_observed": len(observed),
        "window_visible": sum(row["window_visible"] == "1" for row in observed),
        "window_foreground": sum(row["window_foreground"] == "1" for row in observed),
        "window_background": sum(row["window_foreground"] == "0" for row in observed),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trial_json", type=Path)
    parser.add_argument("stages_csv", type=Path)
    parser.add_argument("present_csv", type=Path)
    parser.add_argument("stderr_log", type=Path)
    parser.add_argument("--source-frames", type=int, required=True)
    parser.add_argument("--position", choices=("foreground", "background"), required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    trial = json.loads(args.trial_json.read_text(encoding="utf-8-sig"))
    pts = per_loop_pts(args.stages_csv)
    expected = args.source_frames * trial["loops"]
    missing = sorted(pts["compressed"] - pts["decoded"])
    extra = sorted(pts["decoded"] - pts["compressed"])
    cpu = cpu_stages(args.stderr_log, args.source_frames)
    windows = window_stats(args.present_csv)
    position_met = (windows["window_observed"] == windows["present_rows"] and
                    (windows["window_foreground"] == windows["present_rows"]
                     if args.position == "foreground" else
                     windows["window_background"] == windows["present_rows"]))
    result = {
        "trial": args.trial_json.stem,
        "display_path": trial["display_path"],
        "position": args.position,
        "loops": trial["loops"],
        "source_frames": args.source_frames,
        "expected_decoder_frames": expected,
        "compressed": len(pts["compressed"]),
        "decoded": len(pts["decoded"]),
        "decoder_qos_missing": len(missing),
        "decoder_unexpected_output": len(extra),
        "cpu_stage_rows": len(cpu),
        "sink_drop": trial["dropped"],
        "sink_rendered": trial["rendered"],
        "present_count": trial["present_count"],
        "present_interval_p99_ms": trial["interval_p99_ms"],
        "present_interval_max_ms": trial["interval_max_ms"],
        "present_sync_interval": trial["present_sync_interval"],
        "sink_clock_sync": trial["sink_clock_sync"],
        "decoder_qos_enabled": not trial["decoder_no_qos"],
        "normal_sink_policy": not trial["lossless_sink_policy"],
        "window": windows,
        "position_met": position_met,
        "missing_frames": [
            {"loop": loop, "pts_ns": frame_pts, "cpu": cpu.get((loop, frame_pts))}
            for loop, frame_pts in missing
        ],
    }
    result["valid"] = (len(pts["compressed"]) == expected and
                       len(cpu) == expected and not extra and
                       trial["present_sync_interval"] == 0 and
                       trial["sink_clock_sync"] and not trial["decoder_no_qos"] and
                       not trial["lossless_sink_policy"] and
                       position_met)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
