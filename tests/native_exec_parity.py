#!/usr/bin/env python3
"""P1-6 end-to-end parity: emitted machine code vs the VM on one program.

Runs tests/zl/valid/native/NativeExecBench.zl four times through zl_language:
each kernel (all-integer `sumSquares`, all-double `poly`) once through the
native execution driver (`--run-native`), which maps the emitted x86-64 bytes
and calls them for real, and once as the VM twin inside the program's own
loop. For each pair the driver line and the VM line must agree *as printed* -
same result, same accumulated loop total - which for the double pair means
same bits, because both sides render the shortest round-trip decimal of the
same binary64 value. The Python interpreter is a third opinion on the unit.

The driver must also refuse what it cannot honestly call (`--call main`) and
name what it compiled instead. Wall times are printed for the record and not
asserted - the sandbox is shared and milliseconds swing. On any platform this
is not x86-64 Linux the driver refuses to exist by design, so the test skips.
"""

import platform
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FIXTURE = REPO / "tests" / "zl" / "valid" / "native" / "NativeExecBench.zl"
ITERS = "10000"
LINE = re.compile(r"(\w[\w-]*): (\S+) result=(\S+) iters=(\d+) total=(\S+) (?:native|vm)_ms=([\d.]+)")

failures = []


def require(cond, msg):
    if not cond:
        failures.append(msg)
        print(f"FAIL  {msg}")


def parse(line_text, tag, name):
    for line in line_text.splitlines():
        m = LINE.match(line.strip())
        if m and m.group(2) == name:
            return m
    require(False, f"{tag}: no '{name}' line in output: {line_text!r}")
    return None


def main() -> int:
    zl = sys.argv[1]
    if sys.platform != "linux" or platform.machine() != "x86_64":
        print(f"SKIP  native execution driver is x86-64 Linux only (host: {sys.platform}/{platform.machine()})")
        return 0

    vm = subprocess.run([zl, str(FIXTURE)], capture_output=True, text=True, timeout=300)
    require(vm.returncode == 0, f"VM run exited {vm.returncode}: {vm.stderr.strip()[:200]}")
    require("ok" in vm.stdout.splitlines()[-1:], "fixture did not end with its ok line")

    for name, extra, expect in (
        ("sumSquares", ["--int64", "100"], "328350"),
        ("poly", ["--double", "0.7"], repr(0.7 * 1.5 + 0.1)),
    ):
        native = subprocess.run(
            [zl, "--run-native", str(FIXTURE), "--call", name, *extra, "--iters", ITERS],
            capture_output=True, text=True, timeout=300,
        )
        require(native.returncode == 0, f"--run-native {name} exited {native.returncode}: {native.stderr.strip()[:200]}")
        n = parse(native.stdout, "driver", f"NativeExecBench.{name}(int)" if name == "sumSquares" else f"NativeExecBench.{name}(double)")
        v = parse(vm.stdout, "vm", f"NativeExecBench.{name}Vm(int)" if name == "sumSquares" else f"NativeExecBench.{name}Vm(double)")
        if n and v:
            require(n.group(4) == ITERS and v.group(4) == ITERS, f"{name}: iteration counts drifted")
            require(n.group(3) == v.group(3), f"{name}: tier disagreement on the unit: driver {n.group(3)} vs VM {v.group(3)}")
            require(n.group(5) == v.group(5), f"{name}: tier disagreement on the loop total: driver {n.group(5)} vs VM {v.group(5)}")
            require(n.group(3) == expect, f"{name}: driver unit {n.group(3)} != independently computed {expect}")
            print(f"pass  {name}: unit={n.group(3)} total={n.group(5)} "
                  f"(native {float(n.group(6)):.1f} ms vs VM {float(v.group(6)):.1f} ms over {ITERS} calls)")

    refused = subprocess.run(
        [zl, "--run-native", str(FIXTURE), "--call", "main"],
        capture_output=True, text=True, timeout=300,
    )
    require(refused.returncode != 0, "the driver must refuse a function it did not compile")
    require("no native function matches" in refused.stderr and "sumSquares" in refused.stderr,
            f"refusal must name the compiled set, got: {refused.stderr!r}")

    if failures:
        print(f"native-exec parity: {len(failures)} failure(s)")
        return 1
    print("native-exec parity: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
