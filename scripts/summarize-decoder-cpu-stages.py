"""診断用CPU_STAGEログをGStreamerのPTS別欠落と照合する。"""

import argparse
import csv
import json
import math
import re
from pathlib import Path


FIELDS = ("coefficient_jobs_ms", "idct_jobs_ms", "cache_ms", "upload_ms",
          "vld_map_ms", "backend_ms", "finish_ms")


def distribution(rows, field):
    values = sorted(row[field] for row in rows)
    if not values:
        return {"p50": None, "p95": None, "p99": None, "max": None}
    return {
        "p50": values[math.floor((len(values) - 1) * 0.50)],
        "p95": values[math.floor((len(values) - 1) * 0.95)],
        "p99": values[math.floor((len(values) - 1) * 0.99)],
        "max": values[-1],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trial_json", type=Path)
    parser.add_argument("stderr_log", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--presentmon-summary", type=Path,
                        help="同じ試行のPresentMon PTS照合結果をCPU段階と結合")
    args = parser.parse_args()

    trial = json.loads(args.trial_json.read_text(encoding="utf-8-sig"))
    source_frames = trial["source_frames"]
    expected = source_frames * trial["loops"]
    rows = []
    gpu_rows = []
    gpu_disjoint = 0
    for line in args.stderr_log.read_text(encoding="utf-8", errors="replace").splitlines():
        if "GPU_STAGE_DISJOINT" in line:
            gpu_disjoint += 1
        elif "GPU_STAGE pts_ns=" in line:
            match = re.search(
                r"GPU_STAGE pts_ns=(\d+) vld_ms=([0-9.]+) idct_ms=([0-9.]+)"
                r"(?: copy_ms=([0-9.]+) vld_to_copy_ms=([0-9.]+))?", line)
            if not match or (" copy_ms=" in line and match[4] is None):
                raise ValueError("GPU_STAGEの項目が不足しています: " + line)
            gpu = {"pts_ns": int(match[1]), "vld_ms": float(match[2]),
                   "idct_ms": float(match[3])}
            if match[4] is not None:
                gpu.update(copy_ms=float(match[4]), vld_to_copy_ms=float(match[5]))
                if gpu["copy_ms"] < 0 or gpu["vld_to_copy_ms"] < gpu["copy_ms"]:
                    raise ValueError("GPU copy区間の時刻が不正です: " + line)
            gpu_rows.append(gpu)
        if "CPU_STAGE seq=" not in line:
            continue
        fields = dict(re.findall(r"([a-z_]+)=([0-9.]+)", line.split("CPU_STAGE ", 1)[1]))
        required = ("seq", "pts_ns", *FIELDS)
        if any(field not in fields for field in required):
            raise ValueError("CPU_STAGEの項目が不足しています: " + line)
        row = {field: float(fields[field]) for field in FIELDS}
        row.update(seq=int(fields["seq"]), pts_ns=int(fields["pts_ns"]))
        if "copy_ready_wait_ms" in fields:
            row["copy_ready_wait_ms"] = float(fields["copy_ready_wait_ms"])
        if "map_attempts" in fields:
            row["map_attempts"] = int(fields["map_attempts"])
        for optional in ("idct_layout_rebuilt", "idct_quant_slices_changed", "idct_gpu_upload"):
            if optional in fields:
                row[optional] = int(fields[optional])
        rows.append(row)

    if len(rows) != expected or any(row["seq"] != seq for seq, row in enumerate(rows)):
        raise ValueError(f"CPU_STAGEの連番・枚数が不一致: {len(rows)}/{expected}")
    by_position = {(row["seq"] // source_frames, row["pts_ns"]): row for row in rows}
    if len(by_position) != expected:
        raise ValueError("周回内のPTSが重複しています")
    missing = trial["stages"]["missing_after_decoder"]
    if trial["stages"]["missing_before_decoder"] or trial["stages"]["missing_after_converter"]:
        raise ValueError("欠落段階がdecoder出力前だけではありません")
    if trial["rendered"] + len(missing) != expected:
        raise ValueError("renderedと欠落枚数の合計が予定枚数と不一致")
    missing_rows = []
    for loop, pts_ns in missing:
        row = by_position.get((loop, pts_ns))
        if row is None:
            raise ValueError(f"欠落PTSにCPU_STAGEがありません: {loop}, {pts_ns}")
        previous = rows[row["seq"] - 1] if row["seq"] % source_frames else None
        missing_rows.append({"loop": loop, "pts_ns": pts_ns, "seq": row["seq"],
                             "previous": previous, "missing_input": row})

    dropped_sequences = {item["seq"] for item in missing_rows}
    candidates = [row for row in rows if row["seq"] % source_frames]
    high_previous_idct = [row for row in candidates
                          if rows[row["seq"] - 1]["idct_jobs_ms"] >= 8.0]
    low_previous_idct = [row for row in candidates
                         if rows[row["seq"] - 1]["idct_jobs_ms"] < 8.0]

    summary = {
        "trial_json": str(args.trial_json),
        "stderr_log": str(args.stderr_log),
        "expected_frames": expected,
        "cpu_stage_rows": len(rows),
        "rendered": trial["rendered"],
        "missing_after_decoder": len(missing),
        "field_ms": {field: distribution(rows, field) for field in FIELDS},
        "missing_rows": missing_rows,
        "qos_association_previous_idct_8ms": {
            "threshold_ms": 8.0,
            "high_previous_idct": {
                "frames": len(high_previous_idct),
                "decoder_qos_missing": sum(row["seq"] in dropped_sequences
                                           for row in high_previous_idct)},
            "low_previous_idct": {
                "frames": len(low_previous_idct),
                "decoder_qos_missing": sum(row["seq"] in dropped_sequences
                                           for row in low_previous_idct)},
        },
    }
    copy_ready_count = sum("copy_ready_wait_ms" in row for row in rows)
    if copy_ready_count not in (0, expected):
        raise ValueError("copy完了待ちのCPU記録が一部フレームで欠けています")
    if copy_ready_count:
        summary["field_ms"]["copy_ready_wait_ms"] = distribution(
            rows, "copy_ready_wait_ms")
    if all("idct_gpu_upload" in row for row in rows):
        summary["idct_cache"] = {
            "layout_rebuild_frames": sum(row["idct_layout_rebuilt"] for row in rows),
            "quant_slices_changed_total": sum(row["idct_quant_slices_changed"] for row in rows),
            "gpu_job_upload_frames": sum(row["idct_gpu_upload"] for row in rows),
            "gpu_job_upload_skipped_frames": sum(1 - row["idct_gpu_upload"] for row in rows),
        }
    if gpu_rows or gpu_disjoint:
        if gpu_disjoint or len(gpu_rows) != expected or any(
                gpu["pts_ns"] != cpu["pts_ns"] for cpu, gpu in zip(rows, gpu_rows)):
            raise ValueError("CPU/GPU段階のPTS・枚数またはdisjointが不一致")
        summary["gpu_stage"] = {
            "vld_ms": distribution(gpu_rows, "vld_ms"),
            "idct_ms": distribution(gpu_rows, "idct_ms"),
            "largest_cpu_map_wait": [
                {"seq": cpu["seq"], "pts_ns": cpu["pts_ns"],
                 "vld_map_ms": cpu["vld_map_ms"], "gpu_vld_ms": gpu_rows[cpu["seq"]]["vld_ms"]}
                for cpu in sorted(rows, key=lambda item: item["vld_map_ms"], reverse=True)[:20]
            ],
            "qos_missing_previous": [
                {"seq": item["previous"]["seq"],
                 "pts_ns": item["previous"]["pts_ns"],
                 "vld_map_ms": item["previous"]["vld_map_ms"],
                 "gpu_vld_ms": gpu_rows[item["previous"]["seq"]]["vld_ms"]}
                for item in missing_rows if item["previous"] is not None
            ],
        }
        with_copy = sum("copy_ms" in gpu for gpu in gpu_rows)
        if with_copy not in (0, expected):
            raise ValueError("GPU copy区間の記録が一部フレームで欠けています")
        if with_copy:
            summary["gpu_stage"]["copy_ms"] = distribution(gpu_rows, "copy_ms")
            summary["gpu_stage"]["vld_to_copy_ms"] = distribution(
                gpu_rows, "vld_to_copy_ms")
            for key in ("largest_cpu_map_wait", "qos_missing_previous"):
                for item in summary["gpu_stage"][key]:
                    gpu = gpu_rows[item["seq"]]
                    item["gpu_copy_ms"] = gpu["copy_ms"]
                    item["gpu_vld_to_copy_ms"] = gpu["vld_to_copy_ms"]
                    if copy_ready_count:
                        cpu = rows[item["seq"]]
                        item["copy_ready_wait_ms"] = cpu["copy_ready_wait_ms"]
                        item["map_attempts"] = cpu.get("map_attempts")
    if args.presentmon_summary:
        present = json.loads(args.presentmon_summary.read_text(encoding="utf-8-sig"))
        if (present["trials"] != 1 or present["coverage_sufficient_trials"] != 1
                or present["pts_aligned_trials"] != 1
                or present["interior_uncaptured_aligned"] != 0):
            raise ValueError("PresentMonの単一試行・完全PTS捕捉が成立していません")
        alignment = present["trial_details"][0]["pts_alignment"]
        os_missing = alignment["interior_not_displayed_frames"]
        if len(os_missing) != alignment["interior_captured_but_not_displayed"]:
            raise ValueError("OS未表示の個別PTSと合計が不一致")
        qos_missing = {(loop, pts) for loop, pts in missing}
        os_missing_rows = []
        for item in os_missing:
            key = (item["loop"], item["pts_ns"])
            row = by_position.get(key)
            if row is None or key in qos_missing:
                raise ValueError(f"OS未表示のCPU段階を照合できません: {key}")
            previous = rows[row["seq"] - 1] if row["seq"] % source_frames else None
            os_missing_rows.append({"loop": key[0], "pts_ns": key[1],
                                    "seq": row["seq"], "cpu_stage": row,
                                    "previous_cpu_stage": previous,
                                    "display_event": item})
        signal_csv = args.trial_json.with_suffix(".present.csv")
        with signal_csv.open(newline="", encoding="utf-8-sig") as stream:
            signals = {(int(item["loop"]), int(item["pts_ns"]))
                       for item in csv.DictReader(stream) if item.get("pts_ns")}
        bounds = {}
        for loop, pts_ns in signals:
            first, last = bounds.get(loop, (pts_ns, pts_ns))
            bounds[loop] = (min(first, pts_ns), max(last, pts_ns))
        interior = {(loop, pts_ns) for loop, pts_ns in signals
                    if pts_ns not in bounds[loop]}
        if len(interior) != alignment["interior_source_frames"]:
            raise ValueError("Present信号CSVとOS集計の内側PTSが不一致")
        os_keys = {(item["loop"], item["pts_ns"]) for item in os_missing}
        if not os_keys <= interior:
            raise ValueError("OS未表示PTSが内側範囲外です")
        displayed_rows = [by_position[key] for key in interior - os_keys]
        if len(displayed_rows) != alignment["interior_confirmed_displayed"]:
            raise ValueError("OS表示済みPTSをCPU段階へ全件照合できません")
        headroom_groups = {
            "gpu_ready_at_least_5ms_early": [item for item in os_missing_rows
                                              if item["display_event"]["gpu_headroom_to_next_display_ms"] is not None
                                              and item["display_event"]["gpu_headroom_to_next_display_ms"] >= 5],
            "gpu_completed_after_next_display": [item for item in os_missing_rows
                                                   if item["display_event"]["gpu_headroom_to_next_display_ms"] is not None
                                                   and item["display_event"]["gpu_headroom_to_next_display_ms"] < 0],
            "other_or_unknown": [item for item in os_missing_rows
                                 if item["display_event"]["gpu_headroom_to_next_display_ms"] is None
                                 or 0 <= item["display_event"]["gpu_headroom_to_next_display_ms"] < 5],
        }
        if sum(map(len, headroom_groups.values())) != len(os_missing_rows):
            raise ValueError("OS未表示のGPU完了時刻群が不一致")
        summary["presentmon"] = {
            "summary_path": str(args.presentmon_summary),
            "interior_frames": alignment["interior_source_frames"],
            "os_not_displayed": len(os_missing_rows),
            "cpu_stage_ms_by_display": {
                "displayed": {field: distribution(displayed_rows, field) for field in FIELDS},
                "not_displayed": {field: distribution([row["cpu_stage"] for row in os_missing_rows], field)
                                  for field in FIELDS},
            },
            "not_displayed_by_gpu_headroom": {
                name: {"frames": len(group),
                       "next_present_before_next_display": sum(
                           item["display_event"].get("next_present_before_next_display") is True
                           for item in group),
                       "idct_jobs_ms": distribution([item["cpu_stage"] for item in group],
                                                    "idct_jobs_ms")}
                for name, group in headroom_groups.items()
            },
            "per_loop": [
                {"loop": loop,
                 "decoder_qos_missing": sum(item["loop"] == loop for item in missing_rows),
                 "os_not_displayed": sum(item["loop"] == loop for item in os_missing_rows),
                 "gpu_ready_at_least_5ms_early": sum(
                     item["loop"] == loop for item in headroom_groups["gpu_ready_at_least_5ms_early"]),
                 "gpu_completed_after_next_display": sum(
                     item["loop"] == loop for item in headroom_groups["gpu_completed_after_next_display"])}
                for loop in range(trial["loops"])
            ],
            "os_not_displayed_rows": os_missing_rows,
        }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"rows": len(rows), "rendered": trial["rendered"],
                      "missing": len(missing), "idct_jobs_ms": summary["field_ms"]["idct_jobs_ms"]},
                     ensure_ascii=False))


if __name__ == "__main__":
    main()
