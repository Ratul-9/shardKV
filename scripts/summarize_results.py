#!/usr/bin/env python3
"""Summarise the Phase 2 raw result files into median tables.

Usage: scripts/summarize_results.py [results_dir=bench/results]
Reads phase2_inprocess_closed.txt, phase2_inprocess_open.txt, phase2_grpc.txt.
Prints markdown tables. Throughput cells are the median of the repetitions with (min-max).
"""
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def blocks(path):
    """Yield (label_dict, [lines]) for each '## k=v k=v' block."""
    label, lines = None, []
    for raw in Path(path).read_text().splitlines():
        if raw.startswith("## "):
            if label is not None:
                yield label, lines
            label = dict(kv.split("=", 1) for kv in raw[3:].split())
            lines = []
        elif label is not None:
            lines.append(raw)
    if label is not None:
        yield label, lines


def throughput(lines):
    for l in lines:
        m = re.match(r"throughput=(\d+) ops/s", l)
        if m:
            return int(m.group(1))
    return None


def latency(lines):
    out = {}
    for l in lines:
        m = re.match(r"offered=(\S+) achieved=(\S+) ops/s", l)
        if m:
            out["offered"], out["achieved"] = float(m.group(1)), float(m.group(2))
        if l.startswith("latency ns:"):
            for k, v in re.findall(r"(\w+)=([\d.]+)", l):
                out[k] = float(v)
        m = re.match(r"rpc_errors=(\d+)", l)
        if m:
            out["errors"] = int(m.group(1))
    return out


def fmt_ns(v):
    if v >= 1e6:
        return f"{v / 1e6:.2f} ms"
    if v >= 1e3:
        return f"{v / 1e3:.1f} us"
    return f"{v:.0f} ns"


def cell(vals):
    med = statistics.median(vals)
    if med >= 1e6:
        return f"{med / 1e6:.2f}M ({min(vals) / 1e6:.2f}-{max(vals) / 1e6:.2f})"
    return f"{med / 1e3:.1f}K ({min(vals) / 1e3:.1f}-{max(vals) / 1e3:.1f})"


def closed_table(path, group_keys, row_keys, col_key, title):
    data = defaultdict(list)
    for label, lines in blocks(path):
        t = throughput(lines)
        if t is not None and label.get("mode", "closed") == "closed":
            data[(tuple(label.get(k) for k in group_keys), tuple(label.get(k) for k in row_keys),
                  label[col_key])].append(t)
    for grp in sorted({g for g, _, _ in data}):
        print(f"\n{title} {dict(zip(group_keys, grp))}\n")
        cols = sorted({c for g, _, c in data if g == grp}, key=int)
        print("| " + " | ".join(["config"] + [f"{col_key}={c}" for c in cols]) + " |")
        print("|" + "---|" * (len(cols) + 1))
        for row in sorted({r for g, r, _ in data if g == grp}):
            cells = [cell(data[(grp, row, c)]) if (grp, row, c) in data else "" for c in cols]
            print("| " + " | ".join([" ".join(row)] + cells) + " |")


def open_table(path, row_keys, col_key, group_keys, title):
    rows = defaultdict(dict)
    for label, lines in blocks(path):
        lat = latency(lines)
        if "p50" in lat:
            rows[(tuple(label.get(k) for k in group_keys), tuple(label.get(k) for k in row_keys))][
                label[col_key]] = lat
    for grp in sorted({g for g, _ in rows}):
        print(f"\n{title} {dict(zip(group_keys, grp))}\n")
        print("| config | clients | achieved ops/s | p50 | p99 | p999 | max | errors |")
        print("|---|---|---|---|---|---|---|---|")
        for (g, row), by_col in sorted(rows.items()):
            if g != grp:
                continue
            for c in sorted(by_col, key=int):
                l = by_col[c]
                print(f"| {' '.join(row)} | {c} | {l['achieved']:.0f} | {fmt_ns(l['p50'])} | "
                      f"{fmt_ns(l['p99'])} | {fmt_ns(l['p999'])} | {fmt_ns(l['max'])} | "
                      f"{int(l.get('errors', 0))} |")


def main():
    d = Path(sys.argv[1] if len(sys.argv) > 1 else "bench/results")
    names = ("phase2_inprocess_closed.txt", "phase2_inprocess_open.txt",
             "phase2_inprocess_open_before_sleepfix.txt", "phase2_grpc_closed.txt",
             "phase2_grpc_open.txt", "phase2_grpc_open_before_sleepfix.txt")
    for name in names:
        if not (d / name).exists():
            continue
        print(f"\n# {name}")
        print("\n".join(l for l in (d / name).read_text().splitlines()
                         if l.startswith("# commit") or "load averages" in l))

    def have(n):
        return (d / n).exists()

    if have("phase2_inprocess_closed.txt"):
        print("\n## In-process closed loop (max throughput, ops/s, median of reps, (min-max))")
        closed_table(d / "phase2_inprocess_closed.txt", ["keys"], ["store"], "clients", "keys")
    for n, title in (("phase2_inprocess_open.txt", "In-process open loop (after sleep fix)"),
                     ("phase2_inprocess_open_before_sleepfix.txt",
                      "In-process open loop (BEFORE sleep fix, biased)")):
        if have(n):
            print(f"\n## {title}")
            open_table(d / n, ["store"], "clients", ["rate"], "rate")
    if have("phase2_grpc_closed.txt"):
        print("\n## gRPC closed loop (max throughput, ops/s)")
        closed_table(d / "phase2_grpc_closed.txt", [], ["server", "workers"], "clients", "gRPC")
    for n, title in (("phase2_grpc_open.txt", "gRPC open loop at 5000 ops/s (after sleep fix)"),
                     ("phase2_grpc_open_before_sleepfix.txt",
                      "gRPC open loop at 5000 ops/s (BEFORE sleep fix, biased)")):
        if have(n):
            print(f"\n## {title}")
            open_table(d / n, ["server", "workers"], "clients", ["rate"], "rate")


if __name__ == "__main__":
    main()
