#!/usr/bin/env python3
"""A/B compare two book_bench binaries.

Runs A and B alternately (A, B, A, B, ...) pinned to one core, collects the
ns_per_op counter of every benchmark, and prints median, range and delta.
A delta counts only when the two ranges do not overlap. When a benchmark also
reports p50_ns/p99_ns/p999_ns (the BM_Lat_* latency benchmarks), each
percentile gets its own table with the same median, range and verdict rules.

usage: compare.py A_BIN B_BIN [--runs N] [--cpu C] [--filter REGEX] [--json OUT]
"""
import argparse
import json
import statistics
import subprocess
import sys

PERCENTILES = ("p50_ns", "p99_ns", "p999_ns")


def verdict_of(a, b):
    if max(b) < min(a):
        return "B faster"
    if min(b) > max(a):
        return "B slower"
    return "no measurable difference"


def summarize(a, b):
    am, bm = statistics.median(a), statistics.median(b)
    return {"a": a, "b": b, "a_median": am, "b_median": bm,
            "delta_pct": (bm - am) / am * 100, "verdict": verdict_of(a, b)}


def print_row(label, width, r):
    a, b = r["a"], r["b"]
    print(f"{label:{width}} {r['a_median']:9.2f} {min(a):8.2f}-{max(a):<8.2f} {r['b_median']:9.2f} "
          f"{min(b):8.2f}-{max(b):<8.2f} {r['delta_pct']:+7.1f}%  {r['verdict']}")


def run(binary, cpu, flt):
    cmd = ["taskset", "-c", str(cpu), binary, "--benchmark_format=json"]
    if flt:
        cmd.append(f"--benchmark_filter={flt}")
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    res = {}
    for b in json.loads(out)["benchmarks"]:
        name = b["name"].split("/")[0]
        if b.get("error_occurred"):
            sys.exit(f"{binary}: {name} failed: {b.get('error_message')}")
        res[name] = (b["ns_per_op"], b["ops_total"], {k: b[k] for k in PERCENTILES if k in b})
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--cpu", type=int, default=3)
    ap.add_argument("--filter", default="")
    ap.add_argument("--json")
    args = ap.parse_args()

    samples = {"A": {}, "B": {}}
    pct = {"A": {}, "B": {}}
    for i in range(args.runs):
        for side, binary in (("A", args.a), ("B", args.b)):
            for name, (ns, ops, extra) in run(binary, args.cpu, args.filter).items():
                if ops < 1_000_000:
                    sys.exit(f"{side}: {name} did only {ops} ops")
                samples[side].setdefault(name, []).append(ns)
                for k, v in extra.items():
                    pct[side].setdefault((name, k), []).append(v)
        print(f"round {i + 1}/{args.runs} done", file=sys.stderr)

    print(f"{'benchmark':22} {'A median':>9} {'A range':>17} {'B median':>9} {'B range':>17} {'delta':>8}  verdict")
    rows = {}
    for name in samples["A"]:
        a, b = samples["A"][name], samples["B"].get(name, [])
        if not b:
            continue
        rows[name] = summarize(a, b)
        print_row(name, 22, rows[name])
    for k in PERCENTILES:
        names = [n for n in rows if (n, k) in pct["A"] and (n, k) in pct["B"]]
        if not names:
            continue
        print(f"\n{k}")
        for name in names:
            rows[name][k] = summarize(pct["A"][(name, k)], pct["B"][(name, k)])
            print_row(name, 22, rows[name][k])
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"a": args.a, "b": args.b, "runs": args.runs, "cpu": args.cpu, "rows": rows}, f, indent=1)


if __name__ == "__main__":
    main()
