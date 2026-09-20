$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root "build"
$OutCpp = Join-Path $env:TEMP "zl-native-$PID.cpp"
$OutExe = Join-Path $env:TEMP "zl-native-bench-$PID.exe"
try {
    & (Join-Path $Build "zl-ir-tests.exe")
    & (Join-Path $Build "zl-native-compiler-tests.exe")
    & (Join-Path $Build "zl-native-boundary-tests.exe")
    & (Join-Path $Build "zl_language.exe") (Join-Path $Root "tests\zl\valid\core_tests\AllFeatures\AllFeatures.zl") | Out-Null
    & (Join-Path $Build "zl_language.exe") --emit-native $OutCpp (Join-Path $Root "tests\zl\valid\native\NumericKernel.zl")
    & c++ -std=c++17 -O3 -DNDEBUG -I (Join-Path $Root "include") $OutCpp (Join-Path $Root "benchmarks\native_numeric_benchmark.cpp") -o $OutExe
    & $OutExe
    $vm = & (Join-Path $Build "zl_language.exe") (Join-Path $Root "tests\zl\valid\native\benchmarks\Benchmark.zl")
    $expected = (Get-Content (Join-Path $Root "tests\zl\valid\native\benchmarks\Expected.txt") -Raw).Trim()
    if ($vm.Trim() -ne $expected) { throw "native VM benchmark semantic mismatch: got '$($vm.Trim())', expected '$expected'" }
    Write-Host "native VM benchmark result=$($vm.Trim())"
    Write-Host "native native gate: PASS"
}
finally {
    Remove-Item $OutCpp, $OutExe -ErrorAction SilentlyContinue
}
