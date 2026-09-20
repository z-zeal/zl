#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build"
OUT_CPP="$(mktemp /tmp/zl-native-XXXXXX.cpp)"
OUT_BIN="$(mktemp /tmp/zl-native-bench-XXXXXX)"
trap 'rm -f "$OUT_CPP" "$OUT_BIN"' EXIT

"${BUILD}/zl-ir-tests"
"${BUILD}/zl-native-compiler-tests"
"${BUILD}/zl-native-boundary-tests"
"${BUILD}/zl_language" "${ROOT}/tests/zl/valid/core_tests/AllFeatures/AllFeatures.zl" >/dev/null

"${BUILD}/zl_language" --emit-native "$OUT_CPP" "${ROOT}/tests/zl/valid/native/NumericKernel.zl"
c++ -std=c++17 -O3 -DNDEBUG -I"${ROOT}/include" "$OUT_CPP" "${ROOT}/benchmarks/native_numeric_benchmark.cpp" -o "$OUT_BIN"
"$OUT_BIN"

VM_OUTPUT=$("${BUILD}/zl_language" "${ROOT}/tests/zl/valid/native/benchmarks/Benchmark.zl")
EXPECTED=$(cat "${ROOT}/tests/zl/valid/native/benchmarks/Expected.txt")
if [[ "$VM_OUTPUT" != "$EXPECTED" ]]; then
    echo "native VM benchmark semantic mismatch: got '$VM_OUTPUT', expected '$EXPECTED'" >&2
    exit 1
fi
printf 'native VM benchmark result=%s\n' "$VM_OUTPUT"
echo 'native native gate: PASS'
