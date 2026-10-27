#!/usr/bin/env python3
"""A/B compare two book_bench binaries.

Runs A and B alternately (A, B, A, B, ...) pinned to one core, collects the
ns_per_op counter of every benchmark, and prints median, range and delta.
A delta counts only when the two ranges do not overlap.

usage: compare.py A_BIN B_BIN [--runs N] [--cpu C] [--filter REGEX] [--json OUT]
"""
import argparse
import json
import statistics
import subprocess
import sys


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
        res[name] = (b["ns_per_op"], b["ops_total"])
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
    for i in range(args.runs):
        for side, binary in (("A", args.a), ("B", args.b)):
            for name, (ns, ops) in run(binary, args.cpu, args.filter).items():
                if ops < 1_000_000:
                    sys.exit(f"{side}: {name} did only {ops} ops")
                samples[side].setdefault(name, []).append(ns)
        print(f"round {i + 1}/{args.runs} done", file=sys.stderr)

    print(f"{'benchmark':22} {'A median':>9} {'A range':>17} {'B median':>9} {'B range':>17} {'delta':>8}  verdict")
    rows = {}
    for name in samples["A"]:
        a, b = samples["A"][name], samples["B"].get(name, [])
        if not b:
            continue
        am, bm = statistics.median(a), statistics.median(b)
        delta = (bm - am) / am * 100
        if max(b) < min(a):
            verdict = "B faster"
        elif min(b) > max(a):
            verdict = "B slower"
        else:
            verdict = "no measurable difference"
        rows[name] = {"a": a, "b": b, "a_median": am, "b_median": bm, "delta_pct": delta, "verdict": verdict}
        print(f"{name:22} {am:9.2f} {min(a):8.2f}-{max(a):<8.2f} {bm:9.2f} {min(b):8.2f}-{max(b):<8.2f} {delta:+7.1f}%  {verdict}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"a": args.a, "b": args.b, "runs": args.runs, "cpu": args.cpu, "rows": rows}, f, indent=1)


if __name__ == "__main__":
    main()
