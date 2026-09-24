"""3面リング版の独立ステージ交互4組をdecoder側ゲートで判定する。"""

import argparse
import json
import statistics
from pathlib import Path


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def paired_records(directory):
    rows = load(directory / "trial-summary.json")
    order = [f"p{pair}-{label}" for pair in range(4)
             for label in (("old", "new") if pair % 2 == 0 else ("new", "old"))]
    if [row["tag"] for row in rows] != order:
        raise ValueError(f"交互4組の順序・試行数が不一致: {directory}")
    return rows


def medians(rows, field):
    return {label: statistics.median(row[field] for row in rows
                                     if row["version"] == label)
            for label in ("old", "new")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--display-dir", required=True, type=Path)
    parser.add_argument("--direct-dir", required=True, type=Path)
    parser.add_argument("--wait-dir", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    display = paired_records(args.display_dir)
    for row in display:
        if row["rendered"] + row["decoder_qos_missing"] + row["sink_dropped"] != row["expected_frames"]:
            raise ValueError(f"表示のPTS枚数が不一致: {row['tag']}")
    decoder_zero = all(row["decoder_qos_missing"] == 0 for row in display
                       if row["version"] == "new")

    direct = load(args.direct_dir / "summary.json")
    if direct["pairs"] != 4 or len(direct["results"]) != 2:
        raise ValueError("directの交互4組または素材数が不一致")
    direct_ok = all(row["change_percent"] >= -3.0 for row in direct["results"])

    wait = paired_records(args.wait_dir)
    wait_p99 = medians(wait, "wait_p99_ms")
    wait_p99_9 = medians(wait, "wait_p99_9_ms")
    decode_p99 = medians(wait, "decode_p99_ms")
    new_wait = [row for row in wait if row["version"] == "new"]
    if any(len(row["top_retire_wait_rows"]) != 5 or
           row["retire_internal_over_20ms_count"] is None for row in new_wait):
        raise ValueError("新版の上位5行または内部待ち内訳が不足")
    internal_over_20_trials = sum(row["retire_internal_over_20ms_count"] > 0
                                  for row in new_wait)
    top_wait_global5 = sorted(
        ({"trial": row["tag"], **item} for row in new_wait
         for item in row["top_retire_wait_rows"]),
        key=lambda item: item["retire_wait_ms"], reverse=True)[:5]
    conditions = {
        "decoder_qos_all_zero": decoder_zero,
        "direct_both_at_least_minus_3_percent": direct_ok,
        "wait_p99_new_at_most_old": wait_p99["new"] <= wait_p99["old"],
        "wait_p99_9_new_at_most_old": wait_p99_9["new"] <= wait_p99_9["old"],
        "decode_p99_new_at_most_old": decode_p99["new"] <= decode_p99["old"],
        "internal_over_20ms_fewer_than_2_trials": internal_over_20_trials < 2,
    }
    result = {
        "passed": all(conditions.values()), "conditions": conditions,
        "display_trials": display,
        "direct": direct["results"],
        "wait_p99_ms_medians": wait_p99,
        "wait_p99_9_ms_medians": wait_p99_9,
        "decode_p99_ms_medians": decode_p99,
        "internal_over_20ms_trials": internal_over_20_trials,
        "top_wait_global5": top_wait_global5,
        "wait_trials": wait,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                        encoding="utf-8")
    print(json.dumps({"passed": result["passed"], "conditions": conditions,
                      "wait_p99_ms_medians": wait_p99,
                      "wait_p99_9_ms_medians": wait_p99_9,
                      "decode_p99_ms_medians": decode_p99}, ensure_ascii=False))


if __name__ == "__main__":
    main()
