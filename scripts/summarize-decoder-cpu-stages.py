"""診断用CPU_STAGEログをGStreamerのPTS別欠落と照合する。"""

import argparse
import json
import math
import re
from pathlib import Path


FIELDS = ("coefficient_jobs_ms", "idct_jobs_ms", "cache_ms", "upload_ms",
          "vld_map_ms", "backend_ms", "finish_ms")


def distribution(rows, field):
    values = sorted(row[field] for row in rows)
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
    args = parser.parse_args()

    trial = json.loads(args.trial_json.read_text(encoding="utf-8-sig"))
    source_frames = trial["source_frames"]
    expected = source_frames * trial["loops"]
    rows = []
    for line in args.stderr_log.read_text(encoding="utf-8", errors="replace").splitlines():
        if "CPU_STAGE seq=" not in line:
            continue
        fields = dict(re.findall(r"([a-z_]+)=([0-9.]+)", line.split("CPU_STAGE ", 1)[1]))
        required = ("seq", "pts_ns", *FIELDS)
        if any(field not in fields for field in required):
            raise ValueError("CPU_STAGEの項目が不足しています: " + line)
        row = {field: float(fields[field]) for field in FIELDS}
        row.update(seq=int(fields["seq"]), pts_ns=int(fields["pts_ns"]))
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

    summary = {
        "trial_json": str(args.trial_json),
        "stderr_log": str(args.stderr_log),
        "expected_frames": expected,
        "cpu_stage_rows": len(rows),
        "rendered": trial["rendered"],
        "missing_after_decoder": len(missing),
        "field_ms": {field: distribution(rows, field) for field in FIELDS},
        "missing_rows": missing_rows,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"rows": len(rows), "rendered": trial["rendered"],
                      "missing": len(missing), "idct_jobs_ms": summary["field_ms"]["idct_jobs_ms"]},
                     ensure_ascii=False))


if __name__ == "__main__":
    main()
