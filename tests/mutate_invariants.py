#!/usr/bin/env python3
"""Prove tests/invariant_test.cpp catches each invariant break.

For every mutant: copy include/, apply one edit to fastbook.hpp, build
invariant_test against the copy, and require a failing run. Each mutant runs
twice: as is, and with CheckInvariants() stubbed to report success, so the
public-API checks must catch the break on their own.

usage: tests/mutate_invariants.py [workdir]
"""
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
WORK = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/fb-invariant-mutants")

MUTANTS = {
    "I1/I2 sorted: new levels always inserted at the worst end":
        ("ladder.Insert(pos, key, order.price)", "ladder.Insert(0, key, order.price)"),
    "I3 crossed: a taker at the best opposite price does not trade":
        ("limit <= opposite.key[opposite.size - 1]", "limit < opposite.key[opposite.size - 1]"),
    "I4 aggregates: resting add does not grow the level total":
        ("    level.total_quantity += order.quantity;\n", ""),
    "I5 exactly one: cancel also drops the next order of the level":
        ("    level.total_quantity -= nodes_[h].qty;\n    Unlink(level, h);\n",
         "    level.total_quantity -= nodes_[h].qty;\n    Unlink(level, h);\n"
         "    if (nodes_[h].next != kNone) {\n      level.total_quantity -= nodes_[nodes_[h].next].qty;\n"
         "      Unlink(level, nodes_[h].next);\n    }\n"),
    "I6 time priority: new orders join the head of the level":
        ("    (level.orders_tail == kNone ? level.orders_head : nodes_[level.orders_tail].next) = h;\n"
         "    level.orders_tail = h;\n",
         "    node = Node{level.orders_head, kNone, order.order_id, order.quantity};\n"
         "    (level.orders_head == kNone ? level.orders_tail : nodes_[level.orders_head].prev) = h;\n"
         "    level.orders_head = h;\n"),
    "I6 price priority: matching takes the worst crossing level first":
        ("      PriceLevel &level = opposite.levels[opposite.slot[opposite.size - 1]];",
         "      uint32_t pick = opposite.LowerBound(limit);\n"
         "      PriceLevel &level = opposite.levels[opposite.slot[pick]];"),
}

STUB = ("  std::string_view CheckInvariants() const {\n",
        "  std::string_view CheckInvariants() const {\n    if (true) return \"\";\n")


def build_and_run(inc: pathlib.Path, exe: pathlib.Path) -> tuple[bool, str]:
    subprocess.run(["g++", "-std=c++23", "-O1", "-w", f"-I{inc}", str(ROOT / "tests/invariant_test.cpp"),
                    "-o", str(exe)], check=True)
    try:
        out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        failed = [l.split()[1] for l in out.stdout.splitlines() if l.startswith("FAILED")]
        return out.returncode != 0, ", ".join(failed) or out.stdout.strip().splitlines()[-1]
    except subprocess.TimeoutExpired:
        return True, "timeout (hang)"


def main() -> int:
    WORK.mkdir(parents=True, exist_ok=True)
    ok = True
    for name, (old, new) in MUTANTS.items():
        for stubbed in (False, True):
            inc = WORK / "include"
            shutil.rmtree(inc, ignore_errors=True)
            shutil.copytree(ROOT / "include", inc)
            src = (inc / "fastbook.hpp").read_text()
            assert src.count(old) == 1, f"{name}: anchor not found exactly once"
            src = src.replace(old, new)
            if stubbed:
                assert src.count(STUB[0]) == 1
                src = src.replace(STUB[0], STUB[1])
            (inc / "fastbook.hpp").write_text(src)
            caught, detail = build_and_run(inc, WORK / "invariant_test")
            ok &= caught
            tag = "CheckInvariants stubbed" if stubbed else "as is"
            print(f"{'CAUGHT ' if caught else 'MISSED '} {name} [{tag}]: {detail}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
