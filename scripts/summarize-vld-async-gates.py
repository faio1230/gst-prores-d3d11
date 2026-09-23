"""独立ステージの表示・回収待ち・direct性能をdecoder採用ゲートで集約する。"""

import argparse
import json
from pathlib import Path
from statistics import median


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def by_version(rows, version):
    return [row for row in rows if row["version"] == version]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--display", type=Path, required=True)
    parser.add_argument("--direct", type=Path, required=True)
    parser.add_argument("--wait", type=Path, nargs="+", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    display = load(args.display)
    require(len(display) == 8, "表示比較は4組8試行が必要です")
    for pair in range(4):
        rows = [row for row in display if row["tag"].startswith(f"p{pair}-")]
        require({row["version"] for row in rows} == {"old", "new"} and len(rows) == 2,
                f"表示比較の第{pair}組が不完全です")
    for row in display:
        require(row["expected_frames"] == 4800 and
                row["rendered"] + row["decoder_qos_missing"] == 4800,
                f"表示比較のフレーム数が不正です: {row['tag']}")
    old_qos = [row["decoder_qos_missing"] for row in by_version(display, "old")]
    new_qos = [row["decoder_qos_missing"] for row in by_version(display, "new")]
    display_gate = all(value == 0 for value in new_qos) and median(new_qos) <= median(old_qos)

    direct = load(args.direct)
    require(direct["pairs"] >= 4 and len(direct["results"]) == 2,
            "direct性能比較の組数または解像度が不足しています")
    require({row["input"] for row in direct["results"]} ==
            {"synthetic-1080p60-hq.mov", "synthetic-2160p60-hq.mov"},
            "direct性能比較の素材が不正です")
    direct_gate = all(row["old_median_fps"] > 0 and row["new_median_fps"] > 0
                      and row["change_percent"] >= -3.0 for row in direct["results"])

    waits = [load(path) for path in args.wait]
    require(all(len(rows) == 4 for rows in waits) and len(waits) >= 2,
            "回収待ち比較は交互2組以上が必要です")
    wait_rows = [row for trial in waits for row in trial]
    for row in wait_rows:
        require(row["expected_frames"] == 4800 and
                row["rendered"] + row["decoder_qos_missing"] +
                row.get("sink_dropped", 0) == 4800,
                f"回収待ち試行のフレーム数が不正です: {row['tag']}")
    timing = {}
    for version in ("old", "new"):
        rows = by_version(wait_rows, version)
        require(len(rows) == len(wait_rows) // 2, "回収待ちの旧新版件数が不一致")
        timing[version] = {
            "wait_p99_median_ms": median(row["wait_p99_ms"] for row in rows),
            "wait_max_median_ms": median(row["wait_max_ms"] for row in rows),
            "wait_global_max_ms": max(row["wait_max_ms"] for row in rows),
            "decode_p99_median_ms": median(row["decode_p99_ms"] for row in rows),
            "trials": [{**{key: row[key] for key in
                            ("tag", "wait_p99_ms", "wait_max_ms", "decode_p99_ms",
                             "decoder_qos_missing")},
                        "sink_dropped": row.get("sink_dropped", 0)}
                       for row in rows],
        }
    wait_gate = (timing["new"]["wait_p99_median_ms"] < timing["old"]["wait_p99_median_ms"]
                 and timing["new"]["wait_max_median_ms"] < timing["old"]["wait_max_median_ms"]
                 and timing["new"]["wait_global_max_ms"] < timing["old"]["wait_global_max_ms"]
                 and timing["new"]["decode_p99_median_ms"] < timing["old"]["decode_p99_median_ms"])

    summary = {
        "display": {"old_qos_missing": old_qos, "new_qos_missing": new_qos,
                    "old_median": median(old_qos), "new_median": median(new_qos),
                    "all_new_zero_and_no_median_regression": display_gate},
        "direct": {"results": direct["results"], "within_3_percent_or_better": direct_gate},
        "timing": {"old": timing["old"], "new": timing["new"],
                   "p99_and_max_and_decode_p99_improved": wait_gate,
                   "diagnostic_cpu_logging": True},
        "os_dwm_display": "decoder採用ゲート外の参考値。今回の集計に含めない",
        "passed": display_gate and direct_gate and wait_gate,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"passed": summary["passed"], "display": display_gate,
                      "direct": direct_gate, "timing": wait_gate}, ensure_ascii=False))


if __name__ == "__main__":
    main()
