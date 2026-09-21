#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${ZL_BIND_BIN:-$ROOT/build/zl-bind}"
TMP="$(mktemp -d)"
export ROOT TMP
trap 'rm -rf "$TMP"' EXIT
"$BIN" "$ROOT/tests/zl-bind/fixtures/example_native.h" Demo "$TMP"
grep -q '^version=3$' "$TMP/Demo.zlbind"
grep -q '^namespace=Demo$' "$TMP/Demo.zlbind"
grep -q '^abi=' "$TMP/Demo.zlbind"
grep -q '^pointer_bits=' "$TMP/Demo.zlbind"
grep -q '^int64_bits=64$' "$TMP/Demo.zlbind"
grep -q '^double_bits=64$' "$TMP/Demo.zlbind"
grep -q '^func|zl_add|int|2|' "$TMP/Demo.zlbind"
grep -q '^class|Counter|constructor=1|ownership=unique|errors=exception|handles=integer$' "$TMP/Demo.zlbind"
grep -q '^method|Counter.value|int|1|const=true$' "$TMP/Demo.zlbind"
grep -q '^method|Counter.increment|void|2|const=false$' "$TMP/Demo.zlbind"
grep -q '^method|Counter.fail|int|1|const=true$' "$TMP/Demo.zlbind"
grep -q 'native exception' "$TMP/Demo_bindings.cpp"
grep -q 'Counter_new_binding' "$TMP/Demo_bindings.cpp"
grep -q 'Counter_value_binding' "$TMP/Demo_bindings.cpp"
grep -q 'NativeFunctionRegistrar registrar' "$TMP/Demo_bindings.cpp"
# Extension bindings declare their arity on the NativeFunction: without it the
# compiler cannot validate call sites and the VM cannot pop arguments (both
# read NativeFunction::arity(), which throws for catalog-less bindings).
grep -q 'zl_add_binding, 2' "$TMP/Demo_bindings.cpp"
grep -q 'Counter_new_binding, 1' "$TMP/Demo_bindings.cpp"
grep -q 'Counter_value_binding, 1' "$TMP/Demo_bindings.cpp"
grep -q 'Counter_increment_binding, 2' "$TMP/Demo_bindings.cpp"
grep -q 'Counter_close_binding, 1' "$TMP/Demo_bindings.cpp"
grep -q '"Demo.Counter_new"' "$TMP/Demo_bindings.cpp"
grep -q '"Demo.Counter_value"' "$TMP/Demo_bindings.cpp"
grep -q '"Demo.Counter_increment"' "$TMP/Demo_bindings.cpp"
grep -q '"Demo.Counter_close"' "$TMP/Demo_bindings.cpp"
# Typed field-by-field struct schemas (P2-7): a manifest entry per struct and
# per named field, a compile-time layout table in the C++, get/set bindings
# per field, and a facade whose accessors are typed by field.
grep -q '^struct|DemoConfig|fields=4|storage=zero-initialized|handles=integer$' "$TMP/Demo.zlbind"
grep -q '^field|DemoConfig.flag|bool|ctype=bool|access=getter+setter$' "$TMP/Demo.zlbind"
grep -q '^field|DemoConfig.count|int|ctype=int32_t|access=getter+setter$' "$TMP/Demo.zlbind"
grep -q '^field|DemoConfig.ratio|double|ctype=double|access=getter+setter$' "$TMP/Demo.zlbind"
grep -q '^field|DemoConfig.mode|int|ctype=uint16_t|access=getter+setter$' "$TMP/Demo.zlbind"
grep -q 'struct ZlFieldSchema' "$TMP/Demo_bindings.cpp"
grep -q 'offsetof(DemoConfig, ratio)' "$TMP/Demo_bindings.cpp"
grep -q 'static_assert(offsetof(DemoConfig, flag) + sizeof(bool) <= sizeof(DemoConfig)' "$TMP/Demo_bindings.cpp"
grep -q 'DemoConfig_get_ratio_binding' "$TMP/Demo_bindings.cpp"
grep -q 'DemoConfig_set_mode_binding' "$TMP/Demo_bindings.cpp"
grep -q '"Demo.DemoConfig_get_flag"' "$TMP/Demo_bindings.cpp"
grep -q '"Demo.DemoConfig_offset_count"' "$TMP/Demo_bindings.cpp"
grep -q '"Demo.DemoConfig_size"' "$TMP/Demo_bindings.cpp"
grep -q 'DemoConfig_new_binding, 0' "$TMP/Demo_bindings.cpp"
grep -q 'DemoConfig_set_ratio_binding, 2' "$TMP/Demo_bindings.cpp"
grep -q 'class DemoConfig' "$TMP/Demo.zl"
grep -q 'public func ratio(): double' "$TMP/Demo.zl"
grep -q 'public func set_mode(int value)' "$TMP/Demo.zl"
grep -q 'Demo.DemoConfig_get_count(this.handle)' "$TMP/Demo.zl"
grep -q '^## Struct schemas$' "$TMP/Demo.md"
grep -q '| .mode. | .uint16_t. | .int. |' "$TMP/Demo.md"
# The facade is real, parseable ZL: type-first fields and parameters, and
# calls to the flattened runtime names.
grep -q 'int handle' "$TMP/Demo.zl"
grep -q 'func Counter(int start)' "$TMP/Demo.zl"
grep -q 'Demo.Counter_new(start)' "$TMP/Demo.zl"
grep -q 'Demo.Counter_value(this.handle)' "$TMP/Demo.zl"
grep -q 'class Counter' "$TMP/Demo.zl"
grep -q 'class Counter' "$TMP/Demo.zl"
grep -q '^# Demo native bindings$' "$TMP/Demo.md"
grep -q '## ABI requirements' "$TMP/Demo.md"
grep -q 'Counter' "$TMP/Demo.md"

cp "$ROOT/tests/zl-bind/fixtures/example_native.h" "$TMP/example_native.h"
cp "$ROOT/tests/zl-bind/fixtures/example_native.cpp" "$TMP/example_native.cpp"
g++ -std=c++17 -I"$ROOT/include" -I"$TMP" -c "$TMP/Demo_bindings.cpp" -o "$TMP/Demo_bindings.o"
g++ -std=c++17 -I"$ROOT/include" -I"$TMP" -c "$TMP/example_native.cpp" -o "$TMP/example_native.o"
# The registry test binds into the real runtime, so link the same translation
# units zl_language is built from (src/ minus main.cpp). Compiled in parallel;
# object names are path-flattened to stay unique across subdirectories.
find "$ROOT/src" -name '*.cpp' ! -name 'main.cpp' -print0 |
    xargs -0 -P "$(nproc)" -n 1 sh -c '
        src_file="$1"
        rel="${src_file#"$ROOT"/}"
        obj="$TMP/$(echo "$rel" | tr "/" "_").o"
        g++ -std=c++17 -I"$ROOT/include" -c "$src_file" -o "$obj"
    ' sh
ZL_RUNTIME_OBJECTS="$(find "$TMP" -maxdepth 1 -name 'src_*.o' | sort | tr "\n" " ")"
[[ -n "$ZL_RUNTIME_OBJECTS" ]] || { echo "runtime objects failed to build" >&2; exit 1; }
cat > "$TMP/registry_test.cpp" <<'CPP'
#include "zl/vm/native.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
int main() {
    const char* names[] = {
        "Demo.zl_add", "Demo.Counter_new", "Demo.Counter_value",
        "Demo.Counter_increment", "Demo.Counter_fail", "Demo.Counter_close"
    };
    for (const char* name : names) if (!zl::findNativeFunction(name)) return 1;
    // Extension bindings have no catalog signature; their declared arity is
    // what the compiler validates against and the VM pops. This used to throw
    // std::logic_error ("no catalog entry"), making every extension native
    // uncallable from ZL.
    const std::size_t expectedArities[] = {2, 1, 1, 2, 1, 1};
    for (int i = 0; i < 6; ++i) {
        const auto idx = zl::findNativeFunction(names[i]);
        if (zl::nativeFunctionTable()[*idx].arity() != expectedArities[i]) return 10 + i;
    }
    auto make = zl::findNativeFunction("Demo.Counter_new");
    auto value = zl::findNativeFunction("Demo.Counter_value");
    auto increment = zl::findNativeFunction("Demo.Counter_increment");
    auto fail = zl::findNativeFunction("Demo.Counter_fail");
    auto close = zl::findNativeFunction("Demo.Counter_close");
    auto handle = zl::nativeFunctionTable()[*make].fn({std::int64_t(7)});
    if (std::get<std::int64_t>(zl::nativeFunctionTable()[*value].fn({handle})) != 7) return 2;
    zl::nativeFunctionTable()[*increment].fn({handle, std::int64_t(5)});
    if (std::get<std::int64_t>(zl::nativeFunctionTable()[*value].fn({handle})) != 12) return 3;
    try { zl::nativeFunctionTable()[*fail].fn({handle}); return 4; }
    catch (const std::exception& e) { if (std::string(e.what()).find("Counter native exception: counter failure") == std::string::npos) return 5; }
    zl::nativeFunctionTable()[*close].fn({handle});
    try { zl::nativeFunctionTable()[*value].fn({handle}); return 6; }
    catch (const std::exception&) {}

    // Struct schema bindings: every field reads and writes by name, the
    // fresh object is zero-initialized, the schema offsets lie inside the
    // struct's real size on this target, and a struct handle is type-checked
    // against class handles rather than reinterpreted.
    auto cfgNew = zl::findNativeFunction("Demo.DemoConfig_new");
    auto cfgClose = zl::findNativeFunction("Demo.DemoConfig_close");
    auto cfgSize = zl::findNativeFunction("Demo.DemoConfig_size");
    const auto getF = [](const char* f) { return *zl::findNativeFunction(std::string("Demo.DemoConfig_get_").append(f).c_str()); };
    const auto setF = [](const char* f) { return *zl::findNativeFunction(std::string("Demo.DemoConfig_set_").append(f).c_str()); };
    const auto offF = [](const char* f) { return *zl::findNativeFunction(std::string("Demo.DemoConfig_offset_").append(f).c_str()); };
    auto cfg = zl::nativeFunctionTable()[*cfgNew].fn({});
    if (std::get<bool>(zl::nativeFunctionTable()[getF("flag")].fn({cfg}))) return 20;
    if (std::get<std::int64_t>(zl::nativeFunctionTable()[getF("count")].fn({cfg})) != 0) return 21;
    if (std::get<double>(zl::nativeFunctionTable()[getF("ratio")].fn({cfg})) != 0.0) return 22;
    zl::nativeFunctionTable()[setF("flag")].fn({cfg, true});
    zl::nativeFunctionTable()[setF("count")].fn({cfg, std::int64_t(-42)});
    zl::nativeFunctionTable()[setF("ratio")].fn({cfg, 3.25});
    zl::nativeFunctionTable()[setF("mode")].fn({cfg, std::int64_t(65535)});
    if (!std::get<bool>(zl::nativeFunctionTable()[getF("flag")].fn({cfg}))) return 23;
    if (std::get<std::int64_t>(zl::nativeFunctionTable()[getF("count")].fn({cfg})) != -42) return 24;
    if (std::get<double>(zl::nativeFunctionTable()[getF("ratio")].fn({cfg})) != 3.25) return 25;
    // A uint16_t field truncates to its own width, not the ZL int's.
    if (std::get<std::int64_t>(zl::nativeFunctionTable()[getF("mode")].fn({cfg})) != 65535) return 26;
    zl::nativeFunctionTable()[setF("mode")].fn({cfg, std::int64_t(65536)});
    if (std::get<std::int64_t>(zl::nativeFunctionTable()[getF("mode")].fn({cfg})) != 0) return 27;
    const auto size = std::get<std::int64_t>(zl::nativeFunctionTable()[*cfgSize].fn({}));
    if (size < 4) return 28;
    const char* fields[] = {"flag", "count", "ratio", "mode"};
    for (int i = 0; i < 4; ++i) {
        const auto off = std::get<std::int64_t>(zl::nativeFunctionTable()[offF(fields[i])].fn({}));
        if (off < 0 || off >= size) return 29;
    }
    try { zl::nativeFunctionTable()[getF("flag")].fn({handle}); return 30; }
    catch (const std::exception&) {}
    zl::nativeFunctionTable()[*cfgClose].fn({cfg});
    try { zl::nativeFunctionTable()[getF("flag")].fn({cfg}); return 31; }
    catch (const std::exception&) {}
    std::cout << "zl-bind class/registry tests passed\n";
}
CPP
g++ -std=c++17 -I"$ROOT/include" "$TMP/Demo_bindings.o" "$TMP/example_native.o" $ZL_RUNTIME_OBJECTS "$TMP/registry_test.cpp" -o "$TMP/registry_test"
"$TMP/registry_test"
cat > "$TMP/M2e2e.zl" <<'ZL'
class M2e2e {
    func main(): void {
        var h = Demo.Counter_new(7)
        log(Demo.Counter_value(h))
        Demo.Counter_increment(h, 5)
        log(Demo.Counter_value(h))
        Demo.Counter_close(h)
        log(Demo.zl_add(40, 2))
        var cfg = Demo.DemoConfig_new()
        log(Demo.DemoConfig_get_flag(cfg))
        Demo.DemoConfig_set_flag(cfg, true)
        Demo.DemoConfig_set_ratio(cfg, 0.5)
        log(Demo.DemoConfig_get_flag(cfg))
        log(Demo.DemoConfig_get_ratio(cfg))
        Demo.DemoConfig_close(cfg)
    }
}
ZL
cat > "$TMP/extension_e2e.cpp" <<'CPP'
#include "zl/compiler/compiler.hpp"
#include "zl/compiler/pipeline.hpp"
#include "zl/vm/vm.hpp"
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) { std::cerr << "E2E FAIL: " << what << "\n"; ++failures; }
}
int main(int argc, char** argv) {
    const std::vector<std::filesystem::path> roots{argv[2]};
    zl::pipeline::Options options;
    // Reference path: shared front end, then AST -> bytecode, then the VM.
    zl::pipeline::Pipeline front(options);
    check(front.load(argv[1], roots), "reference load");
    check(front.analyze(), "reference analyze");
    if (failures) return 1;
    {
        zl::Compiler astCompiler;
        auto chunk = std::make_shared<zl::Chunk>(astCompiler.compile(*front.result().program));
        zl::VM vm;
        check(vm.run(std::move(chunk), {}) == 0, "reference run");
    }
    // MIR path: full pipeline to bytecode, then the VM.
    zl::pipeline::Pipeline compiler(options);
    check(compiler.load(argv[1], roots), "mir load");
    check(compiler.analyze(), "mir analyze");
    check(compiler.lowerToMir(), "mir lower");
    check(compiler.verifyMir(), "mir verify");
    check(compiler.optimizeMir(), "mir optimize");
    check(compiler.generate(), "mir generate");
    if (failures) return 1;
    {
        zl::VM vm;
        auto chunk = std::make_shared<zl::Chunk>(std::move(*compiler.result().chunk));
        check(vm.run(std::move(chunk), {}) == 0, "mir run");
    }
    if (!failures) std::cout << "zl-bind extension e2e passed\n";
    return failures ? 1 : 0;
}
CPP
g++ -std=c++17 -I"$ROOT/include" "$TMP/Demo_bindings.o" "$TMP/example_native.o" $ZL_RUNTIME_OBJECTS "$TMP/extension_e2e.cpp" -o "$TMP/extension_e2e"
# Both pipelines run the program, so each expected line appears exactly twice.
"$TMP/extension_e2e" "$TMP/M2e2e.zl" "$ROOT/stdlib" > "$TMP/e2e_out.txt"
[[ "$(grep -c '^7$' "$TMP/e2e_out.txt")" == "2" ]] || { echo "e2e output mismatch" >&2; cat "$TMP/e2e_out.txt" >&2; exit 1; }
[[ "$(grep -c '^12$' "$TMP/e2e_out.txt")" == "2" ]] || { echo "e2e output mismatch" >&2; cat "$TMP/e2e_out.txt" >&2; exit 1; }
[[ "$(grep -c '^42$' "$TMP/e2e_out.txt")" == "2" ]] || { echo "e2e output mismatch" >&2; cat "$TMP/e2e_out.txt" >&2; exit 1; }
[[ "$(grep -c '^false$' "$TMP/e2e_out.txt")" == "2" ]] || { echo "e2e struct default mismatch" >&2; cat "$TMP/e2e_out.txt" >&2; exit 1; }
[[ "$(grep -c '^true$' "$TMP/e2e_out.txt")" == "2" ]] || { echo "e2e struct bool round-trip mismatch" >&2; cat "$TMP/e2e_out.txt" >&2; exit 1; }
[[ "$(grep -c '^0.5$' "$TMP/e2e_out.txt")" == "2" ]] || { echo "e2e struct double round-trip mismatch" >&2; cat "$TMP/e2e_out.txt" >&2; exit 1; }

# Structs are refused rather than guessed at when they cannot be described:
# no fields at all, or a field whose type has no layout-safe binding.
cat > "$TMP/empty_struct.h" <<'H'
struct Nothing {
};
H
if "$BIN" "$TMP/empty_struct.h" Empty "$TMP/empty_out" >/dev/null 2>&1; then
  echo "expected empty struct rejection" >&2
  exit 1
fi
cat > "$TMP/bad_field.h" <<'H'
struct BadField {
    const char* name;
};
H
if "$BIN" "$TMP/bad_field.h" Bad "$TMP/bad_out" >/dev/null 2>&1; then
  echo "expected unsupported struct field type rejection" >&2
  exit 1
fi


cat > "$TMP/shared.h" <<'H'
#include <stdexcept>
// zl: ownership=shared
// zl: errors=exception
class SharedCounter {
public:
    SharedCounter(int start);
    ~SharedCounter();
    int value() const;
};
H
"$BIN" "$TMP/shared.h" Shared "$TMP/shared_out"
grep -q '^class|SharedCounter|constructor=1|ownership=shared|errors=exception|handles=integer|retain=true$' "$TMP/shared_out/Shared.zlbind"
grep -q 'SharedCounter_retain_binding' "$TMP/shared_out/Shared_bindings.cpp"
g++ -std=c++17 -I"$ROOT/include" -I"$TMP" -c "$TMP/shared_out/Shared_bindings.cpp" -o "$TMP/Shared_bindings.o"

cat > "$TMP/collide.h" <<'H'
// zl: ownership=borrowed
// zl: errors=exception
extern "C" int Counter_value(int x);
class Counter {
public:
    Counter(int start);
    ~Counter();
    int value() const;
private:
    int value_;
};
H
if "$BIN" "$TMP/collide.h" Demo "$TMP/collide_out" >/dev/null 2>&1; then
  echo "expected binding name collision rejection" >&2
  exit 1
fi

cat > "$TMP/bad.h" <<'H'
extern "C" long long unsupported(long long value);
H
if "$BIN" "$TMP/bad.h" Bad "$TMP/out" >/dev/null 2>&1; then
  echo "expected unsupported type rejection" >&2
  exit 1
fi

echo "zl-bind tests passed"
