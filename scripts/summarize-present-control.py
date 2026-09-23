"""診断用Present(0/1) CSVとPresentMonをQPC区間で1対1照合する。"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def numeric(value: str) -> float | None:
    if value in ("", "NA"):
        return None
    return float(value)


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    from math import ceil

    return ordered[ceil((len(ordered) - 1) * fraction)]


def summarize_trial(path: Path) -> dict:
    tag = path.name.removesuffix("-present.csv")
    root = path.parent
    raw = read_csv(path)
    pm = read_csv(root / f"{tag}-presentmon.csv")
    log = json.loads((root / f"{tag}.log").read_text(encoding="utf-8-sig"))
    assert raw and pm and log["passed"]
    assert len(raw) == log["frames"]
    frequency = int(log["qpc_frequency"])
    assert frequency > 0
    before_key = "before_copy_qpc" if "before_copy_qpc" in raw[0] else "before_qpc"
    after_key = "after_present_qpc" if "after_present_qpc" in raw[0] else "after_qpc"
    spans = [
        (int(row[before_key]) * 1000 / frequency,
         int(row[after_key]) * 1000 / frequency)
        for row in raw
    ]
    assert [int(row["index"]) for row in raw] == list(range(len(raw)))
    assert all(int(row["hresult"]) == 0 for row in raw)
    matched: dict[int, dict[str, str]] = {}
    unaligned_pm = 0
    index = 0
    for row in pm:
        clock = numeric(row["CPUStartQPCTimeInMs"])
        assert clock is not None
        while index < len(spans) and spans[index][1] < clock - 0.5:
            index += 1
        if (index == len(spans) or clock < spans[index][0] - 0.5 or
                clock > spans[index][1] + 0.5 or index in matched):
            unaligned_pm += 1
            continue
        matched[index] = row
        index += 1
    interior = range(3, len(raw) - 3)
    uncaptured = [i for i in interior if i not in matched]
    not_displayed = [i for i in interior if i in matched and
                     numeric(matched[i]["MsUntilDisplayed"]) is None]
    displayed = [i for i in interior if i in matched and
                 numeric(matched[i]["MsUntilDisplayed"]) is not None]
    intervals = [numeric(matched[i]["MsBetweenDisplayChange"]) for i in displayed]
    intervals = [value for value in intervals if value is not None]
    sync_values = {int(row["SyncInterval"]) for row in pm}
    modes = sorted({row["PresentMode"] for row in pm})
    flags = {int(row["PresentFlags"]) for row in pm}
    window_visible = sorted({int(row["window_visible"]) for row in raw}) if \
        "window_visible" in raw[0] else None
    window_minimized = sorted({int(row["window_minimized"]) for row in raw}) if \
        "window_minimized" in raw[0] else None
    window_topmost = sorted({int(row["window_topmost"]) for row in raw}) if \
        "window_topmost" in raw[0] else None
    foreground_overlap = sorted({int(row["foreground_overlap_percent"]) for row in raw}) if \
        "foreground_overlap_percent" in raw[0] else None
    state_verified = (window_visible == [1] and window_minimized == [0] and
                      (not log.get("topmost") or window_topmost == [1]))
    assert sync_values == {log["interval"]}, (tag, sync_values)
    assert flags == {0}, (tag, flags)
    return {
        "trial_set": root.name,
        "tag": tag,
        "interval": log["interval"],
        "source": log["source"],
        "topmost_requested": log.get("topmost"),
        "raw_present_calls": len(raw),
        "presentmon_rows": len(pm),
        "qpc_aligned_rows": len(matched),
        "unaligned_presentmon_rows": unaligned_pm,
        "interior_source_frames": len(interior),
        "interior_uncaptured": len(uncaptured),
        "interior_not_displayed": len(not_displayed),
        "interior_displayed": len(displayed),
        "display_p95_ms": percentile(intervals, 0.95),
        "display_p99_ms": percentile(intervals, 0.99),
        "display_max_ms": max(intervals) if intervals else None,
        "present_mode": modes,
        "window_visible_values": window_visible,
        "window_minimized_values": window_minimized,
        "window_topmost_values": window_topmost,
        "foreground_overlap_values": foreground_overlap,
        "window_condition_verified": state_verified,
        "coverage_sufficient": not uncaptured and not unaligned_pm,
        "uncaptured_indices": uncaptured,
        "not_displayed_indices": not_displayed,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="+", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if len(args.directories) > 1 and args.out is None:
        parser.error("複数ディレクトリの集計には--outが必要です")
    trials = [summarize_trial(path) for directory in args.directories for path in
              sorted(directory.glob("r*-s*-present.csv"))]
    assert trials, "試行CSVがありません"
    totals = {}
    for interval in (0, 1):
        sufficient = [trial for trial in trials if
                      trial["interval"] == interval and trial["coverage_sufficient"]]
        totals[str(interval)] = {
            "trials": len(sufficient),
            "window_verified_trials": sum(t["window_condition_verified"] for t in sufficient),
            "interior_source_frames": sum(t["interior_source_frames"] for t in sufficient),
            "interior_not_displayed": sum(t["interior_not_displayed"] for t in sufficient),
            "trial_not_displayed": [t["interior_not_displayed"] for t in sufficient],
        }
    result = {
        "method": "各presentのCopyResource前～Present復帰QPC区間とPresentMon CPU開始QPCを1対1照合。先頭末尾3枚を除外。捕捉漏れ試行は正式欠落率から除外。",
        "caveat": "独立診断swap chainであり、標準d3d11videosinkとはウィンドウ・コピー・待機経路が異なる。OS表示時刻なしの原因と製品効果を単独で証明しない。",
        "totals_by_interval": totals,
        "trials": trials,
    }
    output = args.out or args.directories[0] / "summary.json"
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(totals, ensure_ascii=False))


if __name__ == "__main__":
    main()
