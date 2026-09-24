"""最終実表示試行を同一PTSのdecoder入力・出力とCPU段階で判定する。"""

import argparse
import csv
import json
import math
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


def percentile(values, fraction):
    values = sorted(values)
    return values[math.ceil((len(values) - 1) * fraction)] if values else None


def window_stats(path, loops, position, pts, cpu, source_frames, sink_loops):
    with path.open(newline="", encoding="utf-8-sig") as stream:
        rows = list(csv.DictReader(stream))
    observed = [row for row in rows if row["window_found"] == "1"]
    all_stats = {
        "present_rows": len(rows),
        "window_observed": len(observed),
        "window_visible": sum(row["window_visible"] == "1" for row in observed),
        "window_foreground": sum(row["window_foreground"] == "1" for row in observed),
        "window_background": sum(row["window_foreground"] == "0" for row in observed),
    }
    by_loop = defaultdict(list)
    for row in rows:
        by_loop[int(row["loop"])].append(row)
    loop_stats = []
    last_state = None
    for loop in range(loops):
        current = by_loop[loop]
        states = [
            (row["window_foreground"], row.get("window_left"), row.get("window_top"),
             row["window_width"], row["window_height"])
            for row in current
        ]
        transition = len(set(states)) > 1 or bool(states and last_state and states[0] != last_state)
        if states:
            last_state = states[-1]
        visible = bool(current) and all(row["window_found"] == "1" and
                                       row["window_visible"] == "1" and
                                       row["window_minimized"] == "0" for row in current)
        wanted = "1" if position == "foreground" else "0"
        position_met = bool(current) and all(row["window_foreground"] == wanted for row in current)
        rect_present = bool(current) and all(row.get("window_left") is not None and
                                             row.get("window_top") is not None and
                                             int(row["window_width"]) > 0 and
                                             int(row["window_height"]) > 0 for row in current)
        count_ok = (sum(key[0] == loop for key in pts["compressed"]) == source_frames and
                    len([key for key in pts["decoded"] if key[0] == loop]) <= source_frames and
                    len([key for key in cpu if key[0] == loop]) == source_frames)
        if loop == 0:
            category = "warmup"
        elif transition:
            category = "state_transition"
        elif not (visible and position_met and rect_present and count_ok):
            category = "invalid_window_or_stream"
        else:
            category = "valid"
        missing = sorted(frame_pts for stage_loop, frame_pts in
                         pts["compressed"] - pts["decoded"] if stage_loop == loop)
        interval = [float(row["interval_ms"]) for row in current
                    if int(row["present_index"]) > (30 if source_frames >= 480 else 2)]
        loop_stats.append({
            "loop": loop,
            "category": category,
            "state_transition": transition,
            "visible": visible,
            "position_met": position_met,
            "rect_present": rect_present,
            "decoder_input": sum(key[0] == loop for key in pts["compressed"]),
            "decoder_output": sum(key[0] == loop for key in pts["decoded"]),
            "decoder_qos_missing": len(missing),
            "missing_frames": [
                {"pts_ns": frame_pts, "cpu": cpu.get((loop, frame_pts))}
                for frame_pts in missing
            ],
            "sink_drop": sink_loops[loop]["dropped"],
            "sink_rendered": sink_loops[loop]["rendered"],
            "present_rows": len(current),
            "present_interval_p99_ms": percentile(interval, .99),
            "present_interval_max_ms": max(interval) if interval else None,
        })
    return all_stats, loop_stats


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
    windows, loop_stats = window_stats(args.present_csv, trial["loops"], args.position,
                                       pts, cpu, args.source_frames, trial["per_loop"])
    valid_loops = [row for row in loop_stats if row["category"] == "valid"]
    excluded_loops = [row for row in loop_stats if row["category"] != "valid"]
    invalid_loops = [row for row in loop_stats if row["category"] == "invalid_window_or_stream"]
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
        "preplay_window_confirmed": trial.get("require_visible_foreground", False),
        "preplay_wait_ms": trial.get("foreground_ready_wait_ms"),
        "loop_stats": loop_stats,
        "valid_loops": len(valid_loops),
        "valid_decoder_qos_missing": sum(row["decoder_qos_missing"] for row in valid_loops),
        "excluded_decoder_qos_missing": sum(row["decoder_qos_missing"] for row in excluded_loops),
        "state_transition_loops": [row["loop"] for row in loop_stats
                                   if row["category"] == "state_transition"],
        "invalid_loops": [row["loop"] for row in invalid_loops],
        "missing_frames": [
            {"loop": loop, "pts_ns": frame_pts, "cpu": cpu.get((loop, frame_pts))}
            for loop, frame_pts in missing
        ],
    }
    result["valid"] = (len(pts["compressed"]) == expected and
                       len(cpu) == expected and not extra and bool(valid_loops) and
                       not invalid_loops and
                       trial.get("require_visible_foreground", False) and
                       trial["present_sync_interval"] == 0 and
                       trial["sink_clock_sync"] and not trial["decoder_no_qos"] and
                       not trial["lossless_sink_policy"])
    result["gate_passed"] = (result["valid"] and
                             result["valid_decoder_qos_missing"] == 0)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
