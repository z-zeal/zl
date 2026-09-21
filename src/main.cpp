#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <set>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>


#include "manifest.hpp"
#include "zl/common/executable_path.hpp"
#include "zl/common/json.hpp"
#include "zl/common/stdlib_version.hpp"
#include "zl/compiler/module_loader.hpp"
#include "zl/compiler/type_checker.hpp"
#include "zl/compiler/compiler.hpp"
#include "zl/parser/parser.hpp"
#include "zl/lexer/lexer.hpp"
#include "zl/vm/vm.hpp"
#include "zl/vm/runtime_thread.hpp"
#include "zl/vm/native.hpp"
#include "zl/compiler/native_compiler.hpp"
#include "zl/compiler/pipeline.hpp"
#include "zl/mir/printer.hpp"
#include "zl/mir/reachability.hpp"
#include "zl/native/exec.hpp"
#include "zl/native/pipeline.hpp"

namespace pipeline = zl::pipeline;



namespace {

std::vector<std::filesystem::path> splitPathList(const std::string& value) {
    std::vector<std::filesystem::path> result;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::stringstream ss(value);
    std::string segment;
    while (std::getline(ss, segment, sep)) {
        if (!segment.empty()) result.emplace_back(segment);
    }
    return result;
}

std::filesystem::path findNearestManifest(const std::filesystem::path& entryFile) {
    std::filesystem::path dir = std::filesystem::absolute(entryFile).parent_path();
    for (std::filesystem::path p = dir;;) {
        const auto candidate = p / "zlpkg.toml";
        if (std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate)) return candidate;
        const auto parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return {};
}

std::vector<std::filesystem::path> discoverLocalPackageRoots(const std::filesystem::path& manifestPath) {
    std::vector<std::filesystem::path> roots;
    std::set<std::filesystem::path> visited;
    std::vector<std::filesystem::path> queue{manifestPath};

    while (!queue.empty()) {
        const auto currentManifest = std::filesystem::weakly_canonical(queue.back());
        queue.pop_back();
        if (!visited.insert(currentManifest).second) continue;

        zlpkg::Manifest manifest;
        try {
            manifest = zlpkg::loadManifest(currentManifest);
        } catch (const zlpkg::ManifestError&) {
            // A broken nested manifest is skipped rather than failing --check
            // of an otherwise valid entry file, matching the previous scanner.
            continue;
        }
        for (const auto& dep : manifest.dependencies) {
            if (dep.kind != zlpkg::Dependency::Kind::Path) continue;
            const auto depDir = std::filesystem::weakly_canonical(currentManifest.parent_path() / dep.source);
            if (!std::filesystem::exists(depDir) || !std::filesystem::is_directory(depDir)) continue;

            const auto src = depDir / "src";
            roots.push_back(std::filesystem::exists(src) && std::filesystem::is_directory(src) ? src : depDir);
            const auto nestedManifest = depDir / "zlpkg.toml";
            if (std::filesystem::exists(nestedManifest)) queue.push_back(nestedManifest);
        }
    }
    return roots;
}

std::filesystem::path resolveStdlibRoot(const char* argv0) {
    if (const char* env = std::getenv("ZL_STDLIB_ROOT")) {
        if (*env != '\0') return std::filesystem::path(env);
    }
    if (const char* env = std::getenv("ZL_HOME")) {
        if (*env != '\0') return std::filesystem::path(env) / "lib" / "zl" / "stdlib";
    }

    const auto exeDir = zl::common::executableDir(argv0);
    const auto adjacent = exeDir / "stdlib";
    if (std::filesystem::exists(adjacent)) return adjacent;

    // Installed layout: <prefix>/bin/zl -> <prefix>/lib/zl/stdlib.
    const auto installed = exeDir.parent_path() / "lib" / "zl" / "stdlib";
    if (std::filesystem::exists(installed)) return installed;

    return adjacent;
}

// ---------------------------------------------------------------------------
// Pipeline plumbing shared by every command
// ---------------------------------------------------------------------------
//
// Since MIR became the compiler boundary, every command is the same six stages
// with different stage options; the difference between `zl file.zl` and
// `zl --emit-mir - file.zl` is which stages stop the process, not which code
// runs. These helpers are the shared part, and they are the only place the CLI
// reads its environment switches.

// Appends the standard roots: ZL_EXTRA_ROOTS, then the stdlib root last, so a
// project or dependency can shadow a stdlib package with its own file of the
// same dotted path. Returns false - setting `exitCode` to 3 - when the stdlib is
// present but was built for a different runtime version.
bool appendStandardRoots(const char* argv0, std::vector<std::filesystem::path>& roots, int& exitCode,
                        std::string* error = nullptr) {
    if (const char* env = std::getenv("ZL_EXTRA_ROOTS")) {
        if (*env != '\0') for (auto& root : splitPathList(env)) roots.push_back(std::move(root));
    }
    const auto stdlibRoot = resolveStdlibRoot(argv0);
    const auto stdlibVersion = zl::common::checkStdlibVersion(stdlibRoot, ZL_VERSION_STRING);
    if (!stdlibVersion.compatible) {
        if (error != nullptr) *error = stdlibVersion.error;
        else std::cerr << "error: " << stdlibVersion.error << "\n";
        exitCode = 3;
        return false;
    }
    roots.push_back(stdlibRoot);
    return true;
}

// A boolean environment switch. Unset means "use the caller's default", so a
// stage that is on by default can still be turned off with `=0`.
bool environmentFlag(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    const std::string text(value);
    if (text == "0" || text == "false" || text == "off" || text == "no") return false;
    return true;
}

// The stage switches, read once so every command agrees about them:
//
//   ZL_MIR_OPT=0|1          the MIR optimisation stage
//   ZL_MIR_OPT_CHECK=1      differential check after optimising
//   ZL_MIR_OPT_PASSES=...   "default", "none", or a comma-separated pass list
//   ZL_MIR_OPT_SNAPSHOT_DIR write a before/after snapshot per changed pass
//   ZL_MIR_OPT_VERBOSE=1    per-pass trace
//   ZL_MIR_PROMOTE=1        promote slots to block parameters before codegen
//   ZL_MIR_SSA_VERBOSE=1    one line per declined promotion
//   ZL_PIPELINE_VERBOSE=1   the stage ledger
struct StageSwitches {
    bool optimize{true};
    bool checkOptimization{false};
    bool promote{false};
    bool verbose{false};        // the stage ledger
    bool traceOptimization{false};  // one line per pass run
    bool promotionSkips{false};     // one line per declined slot promotion
    std::string passes;
};

StageSwitches stageSwitchesFromEnvironment(bool optimizeByDefault) {
    StageSwitches switches;
    switches.optimize = environmentFlag("ZL_MIR_OPT", optimizeByDefault);
    switches.checkOptimization = environmentFlag("ZL_MIR_OPT_CHECK", false);
    switches.promote = environmentFlag("ZL_MIR_PROMOTE", false);
    switches.verbose = environmentFlag("ZL_PIPELINE_VERBOSE", false);
    switches.traceOptimization = environmentFlag("ZL_MIR_OPT_VERBOSE", false);
    switches.promotionSkips = environmentFlag("ZL_MIR_SSA_VERBOSE", false);
    if (const char* spec = std::getenv("ZL_MIR_OPT_PASSES")) {
        if (*spec != '\0' && environmentFlag("ZL_MIR_OPT", true)) switches.passes = spec;
    }
    return switches;
}

// The backend choice: an explicit `--backend <name>` beats ZL_BACKEND, which
// beats the shipped default (the bytecode backend). An unrecognised name is a
// usage error rather than a fallback, because silently compiling with a
// different backend than the one asked for is exactly the class of surprise
// this phase exists to remove.
bool selectBackend(const std::string& explicitChoice, pipeline::Backend& backend, std::string& error) {
    if (!explicitChoice.empty()) {
        if (!pipeline::parseBackend(explicitChoice, backend)) {
            error = "unknown backend '" + explicitChoice + "' (expected: bytecode, native)";
            return false;
        }
        return true;
    }
    if (const char* env = std::getenv("ZL_BACKEND")) {
        if (*env != '\0' && !pipeline::parseBackend(env, backend)) {
            error = std::string("unknown ZL_BACKEND '") + env + "' (expected: bytecode, native)";
            return false;
        }
    }
    return true;
}

// Options for a command that will *run* the program: the full pipeline with the
// optimiser on (it is a stage of the pipeline, not an aside), the requested
// backend, and the VM as the execution driver.
pipeline::Options optionsForRun(pipeline::Backend backend) {
    const StageSwitches switches = stageSwitchesFromEnvironment(/*optimizeByDefault=*/true);
    pipeline::Options options;
    options.backend = backend;
    options.execution = pipeline::Execution::Vm;
    options.optimize = switches.optimize;
    options.checkOptimization = switches.checkOptimization;
    options.promoteSlots = switches.promote;
    options.optimizerPipeline = switches.passes;
    options.verbose = switches.verbose;
    options.traceOptimization = switches.traceOptimization;
    options.reportPromotionSkips = switches.promotionSkips;
    // All-or-nothing native compilation: refuse to finish when the native
    // backend left a function to the VM. Off by default because the tier is
    // deliberately partial; on when a program is written to the subset and its
    // author wants that to be enforced rather than reported.
    options.strictNative = environmentFlag("ZL_NATIVE_STRICT", false);
    if (const char* snapshots = std::getenv("ZL_MIR_OPT_SNAPSHOT_DIR")) {
        if (*snapshots != '\0') options.optimization.snapshotDirectory = snapshots;
    }
    return options;
}

// Options for a command that only inspects or emits: generation stops after the
// requested artifact, and optimisation is off unless the caller asked for it by
// name (`--emit-mir-opt`, `--mir-opt-check`, or ZL_MIR_OPT=1).
pipeline::Options optionsForEmit(pipeline::Backend backend, bool optimize, bool codeGeneration = true) {
    const StageSwitches switches = stageSwitchesFromEnvironment(optimize);
    pipeline::Options options;
    options.backend = backend;
    options.execution = pipeline::Execution::None;
    options.requireMain = false;
    options.codeGeneration = codeGeneration;
    options.optimize = switches.optimize;
    options.checkOptimization = switches.checkOptimization;
    options.promoteSlots = switches.promote;
    options.optimizerPipeline = switches.passes;
    options.verbose = switches.verbose;
    options.traceOptimization = switches.traceOptimization;
    options.reportPromotionSkips = switches.promotionSkips;
    options.strictNative = environmentFlag("ZL_NATIVE_STRICT", false);
    if (const char* snapshots = std::getenv("ZL_MIR_OPT_SNAPSHOT_DIR")) {
        if (*snapshots != '\0') options.optimization.snapshotDirectory = snapshots;
    }
    return options;
}

// One line for the optimiser, so a person can see it ran and what it did.
// `always` is for the inspection commands; a plain run stays quiet unless the
// stage ledger was asked for.
void reportOptimization(const pipeline::Result& result, bool always = false) {
    const auto* stage = result.stage(pipeline::Stage::MirOptimization);
    if (stage == nullptr || !stage->ran) return;
    if (always || result.options.verbose) std::cerr << "mir-opt: " << stage->detail << "\n";
}

// Refusals from the bytecode backend: a stubbed function is a deliberate
// fail-closed refusal, and a refusal is only defensible if it names what it
// refused.
void reportStubs(const pipeline::Result& result) {
    if (result.stubbedFunctions.empty()) return;
    std::cerr << "MIR bytecode: " << result.stubbedFunctions.size()
              << " function(s) not translatable (stubbed; reachable ones raise at runtime)\n";
    for (std::size_t i = 0; i < result.stubbedFunctions.size() && i < 400; ++i) {
        std::cerr << "  stub: " << result.stubbedFunctions[i];
        if (i < result.stubbedReasons.size() && !result.stubbedReasons[i].empty())
            std::cerr << " - " << result.stubbedReasons[i];
        std::cerr << "\n";
    }
}

// The native backend's per-function ledger, including the fallback note: the
// program still runs, on MIR-derived bytecode, because the VM is the only
// execution driver in this phase.
void reportNativeLedger(const pipeline::Result& result, std::size_t refusalLines = 8) {
    if (!result.native) return;
    const auto& native = *result.native;
    std::cerr << "native: " << native.nativeFunctions.size() << " function(s) compiled, "
              << native.vmFunctions.size() << " left to the VM\n";
    // The totals are about the whole module, and most of a module is stdlib the
    // program never calls. Saying how much of it can run turns a number that
    // reads as "my program is 95% interpreted" into the number that matters.
    const auto reachable = zl::mir::reachableFunctions(result.module);
    if (reachable.hasEntryPoint) {
        std::cerr << "  reachable: " << reachable.describe(result.module) << "\n";
    }
    for (const auto& name : native.nativeFunctions) std::cerr << "  native  " << name << "\n";
    for (std::size_t i = 0; i < native.vmFunctions.size() && i < refusalLines; ++i) {
        std::cerr << "  vm      " << native.vmFunctions[i].function << " - "
                  << native.vmFunctions[i].reason << " (line " << native.vmFunctions[i].line
                  << ")\n";
    }
    if (native.vmFunctions.size() > refusalLines) {
        std::cerr << "  vm      (+" << (native.vmFunctions.size() - refusalLines)
                  << " more; see --emit-native-ir for the full ledger)\n";
    }
    if (result.options.execution == pipeline::Execution::Vm && !native.vmFunctions.empty()) {
        std::cerr << "  execution: VM (the native tier is a code generator; mixed-mode native "
                     "execution is not implemented yet)\n";
    }
}

// Reports a pipeline failure the way this CLI always has: a labelled one-line
// message, the detail behind it, and the failure's documented exit code.
int reportPipelineFailure(const pipeline::Result& result) {
    const auto& failure = result.failure;
    const char* label = pipeline::errorKindLabel(failure.kind);
    std::cerr << ((label != nullptr && *label != '\0') ? label : "error") << ": " << failure.message
              << "\n";
    if (!failure.detail.empty()) std::cerr << failure.detail << "\n";
    return pipeline::exitCodeFor(failure.kind);
}

[[nodiscard]] int writeTextOutput(const char* path, const std::string& text) {
    if (std::string(path) == "-") {
        std::cout << text;
        return std::cout.good() ? 0 : 5;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) { std::cerr << "error: cannot open output '" << path << "'\n"; return 5; }
    out << text;
    return out.good() ? 0 : 5;
}

} // namespace


// Observation-only pipeline: no runtime and no optimisations. Each result names
// the actual boundary reached; unsupported lowering is never a verification pass.
int safetyCheck(const char* file, const char* executable, const std::string& stopAfter = "mir") {
    using zl::common::jsonString;
    std::string stage = "setup";
    std::string parsing = "not-reached", semantic = "not-reached", mir = "not-reached";
    std::string notes = "[]", incomplete = "[]", verification = "null";
    const auto array = [&](const std::vector<std::string>& strings) {
        std::string out = "[";
        for (std::size_t i = 0; i < strings.size(); ++i) {
            if (i) out += ',';
            out += jsonString(strings[i]);
        }
        return out + ']';
    };
    const auto finish = [&](int code, const std::string& outcome, const std::string& message) {
        std::cout << "{\"schema_version\":1,\"file\":" << jsonString(file)
                  << ",\"stage\":" << jsonString(stage) << ",\"outcome\":" << jsonString(outcome)
                  << ",\"message\":" << jsonString(message)
                  << ",\"layers\":{\"parsing\":" << jsonString(parsing)
                  << ",\"semantic\":" << jsonString(semantic) << ",\"mir\":" << jsonString(mir)
                  << ",\"runtime\":\"not-run\"},\"lowering_notes\":" << notes
                  << ",\"incomplete_functions\":" << incomplete
                  << ",\"verification\":" << verification << "}\n";
        return code;
    };

    // The same six stages as every other command, stopped and reported
    // differently. `execution = None` is what makes this observation-only: the
    // pipeline never generates code, so a rejected program cannot execute
    // anything by being checked.
    std::vector<std::filesystem::path> roots;
    int exitCode = 1;
    std::string rootError;
    if (!appendStandardRoots(executable, roots, exitCode, &rootError)) {
        return finish(3, "environment-error", rootError);
    }

    pipeline::Options options;
    options.requireMain = false;
    options.execution = pipeline::Execution::None;
    options.optimize = false;
    options.verification.auditBoundaries = true;
    pipeline::Pipeline compiler(options);

    // An exit code is only ever charged to the layer that actually rejected the
    // program: a parse failure never reports a MIR outcome, and a MIR failure
    // never credits the front end with having accepted more than it did.
    const auto failure = [&](const pipeline::Result& result) -> int {
        switch (result.failure.kind) {
            case pipeline::ErrorKind::Syntax:
                parsing = "rejected";
                return finish(1, "rejected", result.failure.message);
            case pipeline::ErrorKind::Module:
                return finish(7, "module-error", result.failure.message);
            case pipeline::ErrorKind::TypeCheck:
                semantic = "rejected";
                return finish(1, "rejected", result.failure.message);
            case pipeline::ErrorKind::Unsupported:
                return finish(6, "unsupported", result.failure.message);
            case pipeline::ErrorKind::Verification:
                verification = result.verification.toJson();
                mir = "rejected";
                return finish(4, "rejected",
                              "MIR contract violation (may be a compiler defect)");
            default:
                return finish(7, "unclassified-error", result.failure.message);
        }
    };

    stage = "parsing";
    if (!compiler.load(file, roots)) return failure(compiler.result());
    parsing = "accepted";
    if (stopAfter == "parsing") return finish(0, "accepted", "parsing completed");

    stage = "semantic";
    if (!compiler.analyze()) return failure(compiler.result());
    semantic = "accepted";
    if (stopAfter == "semantic") return finish(0, "accepted", "semantic checking completed");

    stage = "lowering";
    if (!compiler.lowerToMir()) return failure(compiler.result());
    notes = array(compiler.result().loweringDiagnostics);
    incomplete = array(compiler.result().incompleteFunctions);

    stage = "mir";
    if (!compiler.verifyMir()) return failure(compiler.result());
    verification = compiler.result().verification.toJson();
    mir = compiler.result().complete() ? "verified" : "partial";
    if (!compiler.result().complete()) {
        return finish(6, "unsupported", "partial MIR is not a safety certificate");
    }
    return finish(0, "verified", "static verification does not discharge runtime obligations");
}

// Shared exception-to-exit-code mapping for every command that executes a
// program. Both the default run path and --mir-vm route their vm.run through
// here so an uncaught ZL exception always becomes a `runtime error:` report
// with exit 1, never an uncaught C++ exception (SIGABRT).
int reportRunException(std::exception_ptr ptr) {
    try {
        std::rethrow_exception(std::move(ptr));
    } catch (const zl::SystemExitException& ex) {
        // System.exit(code) - deliberately NOT caught by zl's own try/catch
        // (it isn't a std::runtime_error), so it always terminates the program.
        return ex.code;
    } catch (const zl::ModuleError& e) {
        std::cerr << "module error: " << e.what() << "\n";
        return 1;
    } catch (const zl::ParseError& e) {
        std::cerr << "syntax error: " << e.what() << "\n";
        return 1;
    } catch (const zl::TypeCheckError& e) {
        std::cerr << "compile error: " << e.what() << "\n";
        return 1;
    } catch (const zl::ZlThrownException& e) {
        std::string message = e.what();
        if (e.value()) {
            const auto* it = zl::objectFieldLookup(*e.value(), "message");
            if (it != nullptr && std::holds_alternative<std::string>(*it)) {
                message = std::get<std::string>(*it);
            }
            const auto* traceIt = zl::objectFieldLookup(*e.value(), "stackTrace");
            if (traceIt != nullptr && std::holds_alternative<std::string>(*traceIt)) {
                const auto& trace = std::get<std::string>(*traceIt);
                if (!trace.empty()) {
                    std::cerr << "runtime error: " << message << "\n" << trace << "\n";
                    return 1;
                }
            }
        }
        std::cerr << "runtime error: " << message << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "runtime error: " << e.what() << "\n";
        return 1;
    }
}

int zlMain(int argc, char** argv) {
    if (argc >= 2) {
        const std::string command = argv[1];
        if (command == "--safety-check") {
            std::string stop = "mir";
            if (argc == 4 && std::string(argv[3]) == "--stop-after=parsing") stop = "parsing";
            else if (argc == 4 && std::string(argv[3]) == "--stop-after=semantic") stop = "semantic";
            else if (argc != 3) {
                std::cerr << "usage: zl --safety-check <file.zl> [--stop-after=parsing|semantic]\n";
                return 2;
            }
            return safetyCheck(argv[2], argv[0], stop);
        }
        if (command == "--emit-machine-code") {
            // The x86-64 artifact, written from verified MIR: the native backend
            // selects and emits, and this command only packages the bytes. The
            // container is unchanged (magic, version, function table, blobs), so
            // anything that read a .zlm before reads this one; what changed is
            // that the code no longer comes from the legacy AST -> zl::ir tier.
            if (argc != 4) {
                std::cerr << "usage: zl --emit-machine-code <output.zlm> <file.zl>\n";
                return 2;
            }
            std::vector<std::filesystem::path> roots;
            int exitCode = 1;
            if (!appendStandardRoots(argv[0], roots, exitCode)) return exitCode;
            pipeline::Options options = optionsForEmit(pipeline::Backend::Native, /*optimize=*/true);
            options.native.selectOnly = false;
            pipeline::Pipeline compiler(std::move(options));
            if (!compiler.run(argv[3], roots)) return reportPipelineFailure(compiler.result());
            if (!compiler.result().native || compiler.result().native->code.empty()) {
                std::cerr << "machine compile error: the native backend emitted no code\n";
                return 4;
            }
            const auto& functions = compiler.result().native->code;
            std::ofstream out(argv[2], std::ios::binary);
            if (!out) { std::cerr << "error: cannot open machine output '" << argv[2] << "'\n"; return 5; }
            auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
            auto u64 = [&](std::uint64_t v) { out.write(reinterpret_cast<const char*>(&v), 8); };
            out.write("ZLM1", 4); u32(1); u32(static_cast<std::uint32_t>(functions.size()));
            std::uint64_t offset = 0;
            for (const auto& fn : functions) {
                u32(static_cast<std::uint32_t>(fn.name.size())); out.write(fn.name.data(), static_cast<std::streamsize>(fn.name.size()));
                u64(offset); u64(static_cast<std::uint64_t>(fn.code.size())); u64(static_cast<std::uint64_t>(fn.parameterCount));
                offset += fn.code.size();
            }
            for (const auto& fn : functions) out.write(reinterpret_cast<const char*>(fn.code.data()), static_cast<std::streamsize>(fn.code.size()));
            if (!out.good()) return 5;
            std::cerr << "machine code: " << functions.size() << " function(s) from backend native\n";
            return 0;
        }
        if (command == "--emit-mir") {
            // The MIR the lowerer produced, before any optimisation: this is the
            // command for "what did my program become", so it deliberately does
            // not run the optimisation stage.
            if (argc != 4) {
                std::cerr << "usage: zl --emit-mir <output|-> <file.zl>\n";
                return 2;
            }
            std::vector<std::filesystem::path> roots;
            int exitCode = 1;
            if (!appendStandardRoots(argv[0], roots, exitCode)) return exitCode;
            pipeline::Pipeline compiler(optionsForEmit(pipeline::Backend::Bytecode, /*optimize=*/false,
                                                       /*codeGeneration=*/false));
            if (!compiler.run(argv[3], roots)) return reportPipelineFailure(compiler.result());
            const auto& result = compiler.result();
            for (const auto& diagnostic : result.loweringDiagnostics)
                std::cerr << "note: " << diagnostic << "\n";
            return writeTextOutput(argv[2], zl::mir::printModule(result.module));
        }
        if (command == "--emit-ssa") {
            // Like --emit-mir, but asking the lowering stage for the MIR's SSA
            // form (mutable locals promoted to block parameters) where that is
            // provably safe. The promotion happens inside the pipeline, so the
            // verification stage checks the promoted module - a promotion that
            // produced invalid MIR is reported, not emitted.
            if (argc != 4) {
                std::cerr << "usage: zl --emit-ssa <output|-> <file.zl>\n";
                return 2;
            }
            std::vector<std::filesystem::path> roots;
            int exitCode = 1;
            if (!appendStandardRoots(argv[0], roots, exitCode)) return exitCode;
            pipeline::Options options = optionsForEmit(pipeline::Backend::Bytecode, /*optimize=*/false,
                                                       /*codeGeneration=*/false);
            options.promoteSlots = true;
            pipeline::Pipeline compiler(std::move(options));
            if (!compiler.run(argv[3], roots)) return reportPipelineFailure(compiler.result());
            const auto& result = compiler.result();
            for (const auto& diagnostic : result.loweringDiagnostics)
                std::cerr << "note: " << diagnostic << "\n";
            for (const auto& note : result.promotion.notes) std::cerr << "  ssa: " << note << "\n";
            std::cerr << "ssa: promoted " << result.promotion.promotedSlots << " slot(s) into "
                      << result.promotion.blockParameters << " block parameter(s); removed "
                      << result.promotion.loadsRemoved << " load(s), "
                      << result.promotion.storesRemoved << " store(s)\n";
            return writeTextOutput(argv[2], zl::mir::printModule(result.module));
        }
        if (command == "--emit-mir-opt") {
            // Like --emit-mir, with the optimisation stage run first. This is how
            // a person answers "what did the optimiser actually do to my
            // program": the textual MIR after it, plus the stage's own report.
            if (argc != 4) {
                std::cerr << "usage: zl --emit-mir-opt <output|-> <file.zl>\n";
                return 2;
            }
            std::vector<std::filesystem::path> roots;
            int exitCode = 1;
            if (!appendStandardRoots(argv[0], roots, exitCode)) return exitCode;
            pipeline::Options options = optionsForEmit(pipeline::Backend::Bytecode, /*optimize=*/true);
            options.verbose = true;
            pipeline::Pipeline compiler(std::move(options));
            if (!compiler.run(argv[3], roots)) return reportPipelineFailure(compiler.result());
            const auto& result = compiler.result();
            for (const auto& diagnostic : result.loweringDiagnostics)
                std::cerr << "note: " << diagnostic << "\n";
            reportOptimization(result, /*always=*/true);
            return writeTextOutput(argv[2], zl::mir::printModule(result.module));
        }
        if (command == "--mir-opt-check") {
            // The differential check on its own: optimise, compare the result
            // against the unoptimised module, and say whether the two observe the
            // same thing. The comparison is a stage option, so this command is
            // just "run the pipeline with the check on". Exits 4 when the modules
            // disagree, so a CI run can use it as a gate without parsing output.
            if (argc != 3) {
                std::cerr << "usage: zl --mir-opt-check <file.zl>\n";
                return 2;
            }
            std::vector<std::filesystem::path> roots;
            int exitCode = 1;
            if (!appendStandardRoots(argv[0], roots, exitCode)) return exitCode;
            pipeline::Options options = optionsForEmit(pipeline::Backend::Bytecode, /*optimize=*/true,
                                                       /*codeGeneration=*/false);
            options.checkOptimization = true;
            pipeline::Pipeline compiler(std::move(options));
            if (!compiler.run(argv[2], roots)) return reportPipelineFailure(compiler.result());
            std::cout << "mir-opt-check: equivalent\n";
            return 0;
        }
        if (command == "--artifact-stats") {
            // Measurement hook for the research harness. Emits one JSON object
            // describing the stage timings and the generated artifacts (MIR
            // shape, bytecode chunk, native machine code). It is a measurement
            // command, not a run path: it never executes the program, and it
            // reports a failed compile as data (exit 0, "ok": false) so a
            // harness can record it instead of treating it as a crash.
            std::string backendChoice;
            bool referenceCompiler = false;
            int statArg = 2;
            while (statArg < argc) {
                const std::string arg = argv[statArg];
                if (arg == "--reference-compiler") { referenceCompiler = true; ++statArg; continue; }
                if (arg.rfind("--backend=", 0) == 0) { backendChoice = arg.substr(10); ++statArg; continue; }
                if (arg == "--backend" && statArg + 1 < argc) { backendChoice = argv[statArg + 1]; statArg += 2; continue; }
                break;
            }
            if (statArg + 1 != argc) {
                std::cerr << "usage: zl --artifact-stats [--backend=<bytecode|native>] [--reference-compiler] <file.zl>\n";
                return 2;
            }
            const char* entry = argv[statArg];
            pipeline::Backend backend = pipeline::Backend::Bytecode;
            std::string backendError;
            if (!selectBackend(backendChoice, backend, backendError)) {
                std::cerr << "error: " << backendError << "\n";
                return 2;
            }
            std::vector<std::filesystem::path> roots;
            int rootExitCode = 1;
            if (!appendStandardRoots(argv[0], roots, rootExitCode)) return rootExitCode;

            std::ostringstream out;
            out << std::fixed << std::setprecision(4);
            out << "{";
            bool firstField = true;
            auto kv = [&](const char* key, const std::string& value) {
                if (!firstField) out << ",";
                out << zl::common::jsonString(key) << ":" << value;
                firstField = false;
            };
            const auto started = std::chrono::steady_clock::now();
            kv("command", zl::common::jsonString("artifact-stats"));
            kv("backend", zl::common::jsonString(pipeline::backendName(backend)));
            kv("reference", referenceCompiler ? "true" : "false");

            if (referenceCompiler) {
                // The original AST -> bytecode compiler, kept unchanged behind
                // ZL_COMPILER=ast. Front end (stages 1-2) is shared with the
                // pipeline; only code generation differs.
                pipeline::Pipeline compiler(optionsForEmit(pipeline::Backend::Bytecode, /*optimize=*/false,
                                                           /*codeGeneration=*/false));
                const bool loaded = compiler.load(entry, roots) && compiler.analyze();
                const auto astStart = std::chrono::steady_clock::now();
                zl::Chunk chunk;
                bool ok = false;
                std::string failure = "";
                if (loaded) {
                    try {
                        zl::Compiler astCompiler;
                        chunk = astCompiler.compile(*compiler.result().program);
                        ok = true;
                    } catch (const std::exception& e) {
                        failure = e.what();
                    }
                } else {
                    failure = compiler.result().failure.message;
                }
                const auto astEnd = std::chrono::steady_clock::now();
                const auto stageMs = [&](pipeline::Stage s) {
                    const pipeline::StageRecord* rec = compiler.result().stage(s);
                    return rec ? rec->milliseconds : 0.0;
                };
                kv("ok", ok ? "true" : "false");
                kv("failure_message", zl::common::jsonString(failure));
                kv("ms_load", std::to_string(stageMs(pipeline::Stage::Load)));
                kv("ms_semantic", std::to_string(stageMs(pipeline::Stage::SemanticAnalysis)));
                kv("ms_codegen", std::to_string(std::chrono::duration<double, std::milli>(astEnd - astStart).count()));
                kv("bytecode_instructions", std::to_string(chunk.code.size()));
                kv("bytecode_bytes", std::to_string(chunk.code.size() * sizeof(zl::Instruction)));
                kv("bytecode_constants", std::to_string(chunk.constants.size()));
                kv("bytecode_names", std::to_string(chunk.names.size()));
                kv("bytecode_functions", std::to_string(chunk.functions.size()));
            } else {
                pipeline::Pipeline compiler(optionsForEmit(backend, /*optimize=*/true));
                const bool ok = compiler.run(entry, roots);
                const auto& result = compiler.result();
                const auto stageMs = [&](pipeline::Stage s) {
                    const pipeline::StageRecord* rec = result.stage(s);
                    return rec ? rec->milliseconds : 0.0;
                };
                std::size_t mirInstructions = 0;
                std::size_t mirBlocks = 0;
                for (const auto& fn : result.module.functions) {
                    mirBlocks += fn.blocks.size();
                    for (const auto& block : fn.blocks) mirInstructions += block.instructions.size();
                }
                kv("ok", ok ? "true" : "false");
                kv("failure_kind", zl::common::jsonString(result.failure.ok()
                                                              ? "" : pipeline::errorKindName(result.failure.kind)));
                kv("failure_stage", zl::common::jsonString(result.failure.ok()
                                                               ? "" : pipeline::stageName(result.failure.stage)));
                kv("failure_message", zl::common::jsonString(result.failure.message));
                kv("ms_total", std::to_string(stageMs(pipeline::Stage::Load) +
                                              stageMs(pipeline::Stage::SemanticAnalysis) +
                                              stageMs(pipeline::Stage::TypedLowering) +
                                              stageMs(pipeline::Stage::MirVerification) +
                                              stageMs(pipeline::Stage::MirOptimization) +
                                              stageMs(pipeline::Stage::CodeGeneration)));
                kv("ms_load", std::to_string(stageMs(pipeline::Stage::Load)));
                kv("ms_semantic", std::to_string(stageMs(pipeline::Stage::SemanticAnalysis)));
                kv("ms_lower", std::to_string(stageMs(pipeline::Stage::TypedLowering)));
                kv("ms_verify", std::to_string(stageMs(pipeline::Stage::MirVerification)));
                kv("ms_opt", std::to_string(stageMs(pipeline::Stage::MirOptimization)));
                kv("ms_codegen", std::to_string(stageMs(pipeline::Stage::CodeGeneration)));
                kv("mir_functions", std::to_string(result.module.functions.size()));
                kv("mir_blocks", std::to_string(mirBlocks));
                kv("mir_instructions", std::to_string(mirInstructions));
                kv("incomplete_functions", std::to_string(result.incompleteFunctions.size()));
                kv("verification_errors", std::to_string(result.verification.errorCount()));
                kv("verification_warnings", std::to_string(result.verification.warningCount()));
                if (result.chunk) {
                    kv("bytecode_instructions", std::to_string(result.chunk->code.size()));
                    kv("bytecode_bytes", std::to_string(result.chunk->code.size() * sizeof(zl::Instruction)));
                    kv("bytecode_constants", std::to_string(result.chunk->constants.size()));
                    kv("bytecode_names", std::to_string(result.chunk->names.size()));
                    kv("bytecode_functions", std::to_string(result.chunk->functions.size()));
                }
                kv("stubbed_functions", std::to_string(result.stubbedFunctions.size()));
                if (result.native) {
                    std::size_t nativeBytes = 0;
                    for (const auto& fn : result.native->code) nativeBytes += fn.code.size();
                    kv("native_functions_compiled", std::to_string(result.native->nativeFunctions.size()));
                    kv("native_functions_vm", std::to_string(result.native->vmFunctions.size()));
                    kv("native_code_functions", std::to_string(result.native->code.size()));
                    kv("native_code_bytes", std::to_string(nativeBytes));
                }
            }
            out << ",\"ms_wall\":" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            out << "}\n";
            std::cout << out.str();
            return 0;
        }
        if (command == "--pipeline-report") {
            // The pipeline made visible: one line per stage, the invariant each
            // stage guarantees, and the MIR digest every backend is handed. It
            // compiles the program twice - once per implemented backend - and
            // fails if the two runs did not reach the *same* MIR, which is the
            // mechanical form of "choosing a backend cannot change what a program
            // means".
            std::string backendChoice;
            int reportArg = 2;
            if (reportArg < argc && std::string(argv[reportArg]).rfind("--backend=", 0) == 0) {
                backendChoice = std::string(argv[reportArg]).substr(10);
                ++reportArg;
            } else if (reportArg + 1 < argc && std::string(argv[reportArg]) == "--backend") {
                backendChoice = argv[reportArg + 1];
                reportArg += 2;
            }
            if (reportArg + 1 != argc) {
                std::cerr << "usage: zl --pipeline-report [--backend=<bytecode|native>] <file.zl>\n";
                return 2;
            }
            const char* entry = argv[reportArg];
            pipeline::Backend backend = pipeline::Backend::Bytecode;
            std::string backendError;
            if (!selectBackend(backendChoice, backend, backendError)) {
                std::cerr << "error: " << backendError << "\n";
                return 2;
            }
            std::vector<std::filesystem::path> roots;
            int exitCode = 1;
            if (!appendStandardRoots(argv[0], roots, exitCode)) return exitCode;

            // The report covers the whole pipeline, code generation included,
            // so it describes a *runnable program's* pipeline - and that is a
            // program contract: the entry file must define a valid entry
            // point. A library is rejected at the semantic stage
            // (requireMain below, the same stage that validates main's
            // signature), never with a report that pretends a code-generation
            // stage exists for a module nothing can enter. Inspect a library
            // with --emit-mir instead: that command's contract stops at MIR.
            pipeline::Options reportOptions = optionsForEmit(backend, /*optimize=*/true);
            reportOptions.requireMain = true;
            pipeline::Pipeline compiler(reportOptions);
            if (!compiler.run(entry, roots)) return reportPipelineFailure(compiler.result());
            if (compiler.result().module.entryPoint == zl::mir::kNoFunction) {
                // A backstop for the command-level contract: the semantic stage
                // already refuses a missing main, so this only fires if that
                // ever relaxes. The report still must not describe a module
                // with no entry point.
                std::cerr << "error: --pipeline-report describes a runnable program's pipeline, but '"
                          << entry << "' has no entry point (no main()): it is a library module, "
                          "and a library has no code-generation stage to report\n"
                          << "  inspect the library with --emit-mir, or add a main() function to "
                          "run it as a program\n";
                return 2;
            }
            std::cout << compiler.result().describe() << "\n";
            std::cout << "stage invariants\n";
            for (std::size_t i = 0; i < pipeline::kStageCount; ++i) {
                const auto stage = static_cast<pipeline::Stage>(i);
                std::cout << "  " << pipeline::stageName(stage) << ": "
                          << pipeline::stageInvariant(stage) << "\n";
            }

            // The other backend, from the same source: same MIR, or the
            // backend-independence claim in the header is false. Same program
            // contract: the sibling run reports the same entry-point rules.
            const pipeline::Backend other = backend == pipeline::Backend::Bytecode
                ? pipeline::Backend::Native : pipeline::Backend::Bytecode;
            pipeline::Options siblingOptions = optionsForEmit(other, /*optimize=*/true);
            siblingOptions.requireMain = true;
            pipeline::Pipeline sibling(siblingOptions);
            if (!sibling.run(entry, roots)) return reportPipelineFailure(sibling.result());
            const std::string mine = compiler.result().mirDigest();
            const std::string theirs = sibling.result().mirDigest();
            std::cout << "backend parity\n"
                      << "  " << pipeline::backendName(backend) << ": " << mine << "\n"
                      << "  " << pipeline::backendName(other) << ": " << theirs << "\n";
            if (mine != theirs) {
                std::cerr << "error: the two backends were handed different MIR\n";
                return 4;
            }
            std::cout << "  identical: the backend choice is a code-generation decision only\n"
                      << "reachability\n"
                      << "  " << zl::mir::reachableFunctions(compiler.result().module)
                                    .describe(compiler.result().module) << "\n";
            return 0;
        }
        if (command == "--mir-vm") {
            // Run a program through the pipeline explicitly: source -> type
            // analysis -> MIR -> verify -> optimise -> bytecode -> VM. Since the
            // default run path is the same pipeline, this command is now the
            // "show me the MIR ledger while you run it" spelling of it, and the
            // differential sibling of the reference AST -> bytecode compiler
            // (`ZL_COMPILER=ast`), which both must match observable behaviour for.
            if (argc < 3) {
                std::cerr << "usage: zl --mir-vm <file.zl> [program args...]\n";
                return 2;
            }
            std::vector<std::filesystem::path> roots;
            int exitCode = 1;
            if (!appendStandardRoots(argv[0], roots, exitCode)) return exitCode;
            pipeline::Options options = optionsForRun(pipeline::Backend::Bytecode);
            options.verbose = true;           // the point of the command
            options.traceOptimization = environmentFlag("ZL_MIR_OPT_VERBOSE", false);
            pipeline::Pipeline compiler(std::move(options));
            if (!compiler.run(argv[2], roots)) return reportPipelineFailure(compiler.result());
            const auto& result = compiler.result();
            for (const auto& diagnostic : result.loweringDiagnostics)
                std::cerr << "note: " << diagnostic << "\n";
            reportStubs(result);
            std::vector<std::string> programArgs;
            for (int i = 3; i < argc; ++i) programArgs.emplace_back(argv[i]);
            zl::VM vm;
            auto chunk = std::make_shared<zl::Chunk>(std::move(*compiler.result().chunk));
            // This branch runs before the function-wide try/catch below, so an
            // uncaught ZL exception here used to escape to std::terminate
            // (SIGABRT) instead of the normal `runtime error:` report. Route
            // it through the same shared mapping the default run path uses.
            try {
                return vm.run(std::move(chunk), programArgs);
            } catch (...) {
                return reportRunException(std::current_exception());
            }
        }
        if (command == "--emit-native") {
            // Legacy tier, deliberately kept and deliberately not grown: this is
            // the only remaining consumer of the untyped `zl::ir`, and it lowers
            // the AST directly instead of going through MIR. The MIR-based
            // native tiers (`--emit-native-ir`, `--emit-native-code`) supersede
            // it for machine code; this one still exists because it emits
            // portable C++ for the restricted `@native` subset, and that emitter
            // has not been ported to consume MIR yet.
            //
            // Its *front end* is not legacy: stages 1-2 are the pipeline's, like
            // every other command, so this path inherits the shared module roots,
            // the stdlib-version check and the failure/exit-code mapping instead
            // of growing its own copy of them. What is legacy is what happens
            // after stage 2 - one emitter that reads the checked AST.
            if (argc != 4) {
                std::cerr << "usage: zl --emit-native <output.cpp> <file.zl>\n";
                return 2;
            }
            std::cerr << "note: --emit-native uses the legacy AST -> zl::ir tier; "
                         "use --emit-native-ir/--emit-native-code for the MIR-based native backend\n";
            const char* nativeOut = argv[2];
            std::vector<std::filesystem::path> roots;
            int rootExitCode = 1;
            if (!appendStandardRoots(argv[0], roots, rootExitCode)) return rootExitCode;
            try {
                // Stages 1-2 only: the emitter below consumes the checked AST, so
                // lowering to MIR would be work thrown away. `codeGeneration` is
                // off because nothing here reaches stage 6 either.
                pipeline::Options options = optionsForEmit(pipeline::Backend::Bytecode,
                                                           /*optimize=*/false,
                                                           /*codeGeneration=*/false);
                pipeline::Pipeline compiler(std::move(options));
                if (!compiler.load(argv[3], roots)) return reportPipelineFailure(compiler.result());
                if (!compiler.analyze()) return reportPipelineFailure(compiler.result());
                const auto nativeResult = zl::native::emitMirCpp(*compiler.result().program);
                if (!nativeResult.success) { std::cerr << "native compile error: " << nativeResult.error << "\n"; return 4; }
                std::ofstream out(nativeOut, std::ios::binary);
                if (!out) { std::cerr << "error: cannot open native output '" << nativeOut << "'\n"; return 5; }
                out << nativeResult.source;
                return out.good() ? 0 : 5;
            } catch (const std::exception& e) {
                std::cerr << "native compile error: " << e.what() << "\n";
                return 1;
            }
        }
        if (command == "--emit-native-ir" || command == "--emit-native-code") {
            // The native backend, driven from verified MIR:
            //   source -> type analysis -> MIR -> verify -> native IR -> machine code
            // Never from the AST: the whole point of the tier is that it
            // consumes the same verified MIR the bytecode backend does.
            const bool emitCode = command == "--emit-native-code";
            if (argc != 4) {
                std::cerr << "usage: zl " << command << " <output|-> <file.zl>\n";
                return 2;
            }
            std::vector<std::filesystem::path> roots;
            int exitCode = 1;
            if (!appendStandardRoots(argv[0], roots, exitCode)) return exitCode;
            pipeline::Options options = optionsForEmit(pipeline::Backend::Native, /*optimize=*/false);
            options.native.selectOnly = !emitCode;
            pipeline::Pipeline compiler(std::move(options));
            if (!compiler.run(argv[3], roots)) return reportPipelineFailure(compiler.result());
            const auto& result = compiler.result();
            const auto& native = *result.native;

            std::ostringstream text;
            text << "; target " << result.nativeTargetTriple << " (" << zl::native::hostTarget().cc.name << ")\n";
            text << zl::native::printLirModule(native.lir);
            if (emitCode) {
                text << "\n; machine code\n";
                for (const auto& fn : native.code) {
                    text << "; " << fn.name << " frame=" << fn.frameSize
                         << " bytes=" << fn.code.size() << "\n";
                    for (std::size_t i = 0; i < fn.code.size(); ++i) {
                        static const char* kHex = "0123456789abcdef";
                        text << kHex[fn.code[i] >> 4] << kHex[fn.code[i] & 0xf]
                             << ((i + 1) % 16 == 0 ? '\n' : ' ');
                    }
                    if (!fn.code.empty() && fn.code.size() % 16 != 0) text << "\n";
                }
            }
            std::cerr << native.describe();
            return writeTextOutput(argv[2], text.str());
        }
        if (command == "--run-native") {
            // The native tier's execution driver: verified MIR -> selected and
            // emitted machine code -> mapped executable -> real call. Without
            // `--call` it is a listing (what the subset compiled of this
            // program, and why the rest is absent); with it, the named
            // function is invoked `--iters` times and its result - int64 or
            // double, decided by the function's own signature - plus the
            // loop total and the wall time are printed. The driver's refusals
            // (runtime-bound calls, mixed or reference signatures,
            // unsupported hosts) are the subset boundary stated in
            // zl/native/exec.hpp - reported by name, before anything
            // executes.
            std::vector<std::string> rest(argv + 2, argv + argc);
            std::string file;
            std::string callSpec;
            std::vector<std::int64_t> intArgs;
            std::vector<double> dblArgs;
            std::int64_t iters = 1;
            for (std::size_t i = 0; i < rest.size(); ++i) {
                const std::string& a = rest[i];
                auto need = [&](const char* opt) -> std::string {
                    if (i + 1 >= rest.size()) {
                        std::cerr << "usage error: " << opt << " needs a value\n";
                        std::exit(2);
                    }
                    return rest[++i];
                };
                if (a == "--call") callSpec = need("--call");
                else if (a == "--int64") {
                    try { intArgs.push_back(std::stoll(need("--int64"))); }
                    catch (const std::exception&) {
                        std::cerr << "usage error: --int64 expects an integer\n";
                        return 2;
                    }
                }
                else if (a == "--double") {
                    try { dblArgs.push_back(std::stod(need("--double"))); }
                    catch (const std::exception&) {
                        std::cerr << "usage error: --double expects a number\n";
                        return 2;
                    }
                }
                else if (a == "--iters") {
                    try { iters = std::stoll(need("--iters")); }
                    catch (const std::exception&) {
                        std::cerr << "usage error: --iters expects an integer\n";
                        return 2;
                    }
                    if (iters < 1) {
                        std::cerr << "usage error: --iters must be positive\n";
                        return 2;
                    }
                }
                else if (!a.empty() && a[0] == '-') {
                    std::cerr << "usage error: unknown option " << a << "\n";
                    return 2;
                }
                else if (file.empty()) file = a;
                else { std::cerr << "usage error: more than one input file\n"; return 2; }
            }
            if (file.empty()) {
                std::cerr << "usage: zl --run-native <file.zl> [--call <name>] [--int64 v | --double v]... [--iters n]\n";
                return 2;
            }
            std::vector<std::filesystem::path> roots;
            int exitCode = 1;
            if (!appendStandardRoots(argv[0], roots, exitCode)) return exitCode;
            pipeline::Options options = optionsForEmit(pipeline::Backend::Native, /*optimize=*/true);
            options.native.selectOnly = false;
            pipeline::Pipeline compiler(std::move(options));
            if (!compiler.run(file, roots)) return reportPipelineFailure(compiler.result());
            const auto& native = *compiler.result().native;
            zl::native::NativeExecutable exec(native.code, native.lir);
            if (!exec.ok()) {
                std::cerr << "run-native: " << exec.error() << "\n";
                return 4;
            }
            if (callSpec.empty()) {
                std::cout << "run-native: " << native.code.size() << " function(s) compiled natively\n";
                for (const auto& fn : native.code) std::cout << "  native  " << fn.name << "\n";
                std::cerr << native.describe();
                return 0;
            }
            if (!intArgs.empty() && !dblArgs.empty()) {
                std::cerr << "usage error: --int64 and --double cannot be mixed; the driver dispatches on the function's signature\n";
                return 2;
            }
            std::string error;
            const std::size_t entry = exec.resolve(callSpec, error);
            if (entry == zl::native::NativeExecutable::kNoEntry) {
                std::cerr << error << "\n";
                return 4;
            }
            const std::string name = native.code[entry].name;
            std::string reason;
            const auto shape = exec.shape(entry, reason);
            using Shape = zl::native::NativeExecutable::Shape;
            if (shape == Shape::kUnsupported) { std::cerr << reason << "\n"; return 4; }
            // The first call warms the mapping; the measured loop follows, and
            // the loop total is accumulated exactly like the fixture's VM loop
            // does - same order, same per-operation rounding - so
            // `native-exec-parity` can compare totals as data, not as bounds.
            const auto start = std::chrono::steady_clock::now();
            if (shape == Shape::kInt64) {
                if (!dblArgs.empty()) {
                    std::cerr << "usage error: '" << name << "' has an all-integer signature; pass --int64 arguments\n";
                    return 2;
                }
                std::int64_t value = 0;
                if (!exec.callInt64(entry, intArgs, value, error)) { std::cerr << error << "\n"; return 4; }
                std::int64_t total = 0;
                for (std::int64_t i = 0; i < iters; ++i) {
                    std::int64_t r = 0;
                    if (!exec.callInt64(entry, intArgs, r, error)) { std::cerr << error << "\n"; return 4; }
                    if (r != value) { std::cerr << "run-native: iteration results disagree (driver bug)\n"; return 4; }
                    total += r;
                }
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                std::cout << "native-exec: " << name << " result=" << value << " iters=" << iters
                          << " total=" << total << " native_ms=" << ms << "\n";
                return 0;
            }
            if (!intArgs.empty()) {
                std::cerr << "usage error: '" << name << "' has an all-double signature; pass --double arguments\n";
                return 2;
            }
            double value = 0.0;
            if (!exec.callDouble(entry, dblArgs, value, error)) { std::cerr << error << "\n"; return 4; }
            double total = 0.0;
            for (std::int64_t i = 0; i < iters; ++i) {
                double r = 0.0;
                if (!exec.callDouble(entry, dblArgs, r, error)) { std::cerr << error << "\n"; return 4; }
                if (r != value) { std::cerr << "run-native: iteration results disagree (driver bug)\n"; return 4; }
                total += r;
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::cout << "native-exec: " << name << " result=" << zl::doubleToShortestString(value)
                      << " iters=" << iters << " total=" << zl::doubleToShortestString(total)
                      << " native_ms=" << ms << "\n";
            return 0;
        }
        if (command == "--parse-only") {
            if (argc != 3) {
                std::cerr << "usage: zl --parse-only <file.zl>\n";
                return 2;
            }
            try {
                std::ifstream in(argv[2], std::ios::binary);
                if (!in) {
                    std::cerr << "error: cannot open source file '" << argv[2] << "\'\n";
                    return 2;
                }
                std::ostringstream source;
                source << in.rdbuf();
                zl::Lexer lexer(source.str());
                auto tokens = lexer.tokenize();
                zl::Parser parser(std::move(tokens));
                (void)parser.parse();
                return 0;
            } catch (const zl::ParseError& e) {
                std::cerr << "syntax error: " << e.what() << "\n";
                return 1;
            } catch (const std::exception& e) {
                std::cerr << "error: " << e.what() << "\n";
                return 1;
            }
        }
        if (command == "--version" || command == "-v") {
            std::cout << "ZL " << ZL_VERSION_STRING << "\n";
            return 0;
        }
        if (command == "--help" || command == "-h") {
            std::cout << "usage:\n"
                         "  zl [--backend <bytecode|native>] [--reference-compiler] [--strict-native] [--root <path>]... <file.zl> [program args...]\n"
                         "  zl --check [--root <path>]... <file.zl>\n"
                         "  zl --pipeline-report [--backend=<bytecode|native>] <file.zl>\n"
                         "  zl --parse-only <file.zl>\n"
                         "  zl --emit-native <output.cpp> <file.zl>\n"
                         "  zl --emit-machine-code <output.zlm> <file.zl>\n"
                         "  zl --emit-native-ir <output|-> <file.zl>\n"
                         "  zl --emit-native-code <output|-> <file.zl>\n"
                         "  zl --run-native <file.zl> [--call <name>] [--int64 v | --double v]... [--iters n]\n"
                         "  zl --mir-vm <file.zl> [program args...]\n"
                         "  zl --emit-mir <output|-> <file.zl>\n"
                         "  zl --emit-ssa <output|-> <file.zl>\n"
                         "  zl --emit-mir-opt <output|-> <file.zl>\n"
                         "  zl --mir-opt-check <file.zl>\n"
                         "  zl --artifact-stats <file.zl>  JSON stage timings + artifact sizes; never executes\n"
                         "  zl --safety-check <file.zl>   JSON safety report; never executes\n"
                         "  zl --version\n"
                         "  zl --help\n"
                         "\n"
                         "every command is the same pipeline (source -> semantic analysis -> MIR ->\n"
                         "verify -> optimise -> backend) with different stages; see docs/pipeline.md\n"
                         "\n"
                         "`--backend native` generates machine code for the supported subset, but the\n"
                         "program still executes on the VM: in-program mixed-mode native execution is\n"
                         "not implemented. The subset boundary is concrete at the execution driver:\n"
                         "zl --run-native maps the emitted bytes and calls a compiled function\n"
                         "(all-integer or all-double signatures; everything else is refused by\n"
                         "name and reason).\n"
                         "Use --strict-native to refuse programs the native tier cannot compile fully.\n"
                         "\n"
                         "environment:\n"
                         "  ZL_BACKEND=<bytecode|native>  backend selection (same as --backend; native still runs on the VM)\n"
                         "  ZL_MIR_OPT=0|1                the MIR optimisation stage on the run path\n"
                         "  ZL_MIR_OPT_PASSES=<spec>      optimiser pipeline: default, none, or a pass list\n"
                         "  ZL_MIR_OPT_CHECK=1            differential check after optimising\n"
                         "  ZL_MIR_PROMOTE=1              run the SSA form of MIR (block parameters)\n"
                         "  ZL_COMPILER=ast|mir           reference AST compiler vs the MIR pipeline\n"
                         "  ZL_NATIVE_STRICT=1            refuse unless every function compiled natively\n"
                         "  ZL_PIPELINE_VERBOSE=1         print the stage ledger\n"
                         "  ZL_EXTRA_ROOTS=<paths>        extra module search roots (before the stdlib)\n"
                         "  ZL_STDLIB_ROOT=<path>         the standard library root\n";
            return 0;
        }
    }

    // Extra module search roots, in the precedence order zl_language will
    // try them (after the project's own sourceRoot_, which ModuleLoader
    // always tries first). This is the "dependency-unaware interpreter"
    // half of Phase 6's package manager: zl_language itself has no notion
    // of zlpkg.toml/.zlpkg caches/git - it just takes an ordered list of
    // roots, the same way it always has for the stdlib root, and `zlpkg
    // run` is responsible for resolving dependencies into roots and
    // passing them here (via repeated --root flags).
    //
    //   1. --root <path> flags, one per dependency, in the order given
    //      (this is how `zlpkg run` wires resolved dependencies in)
    //   2. ZL_EXTRA_ROOTS, an OS-path-list env var, for ad-hoc/manual use
    //      without going through zlpkg at all
    //   3. the stdlib root (ZL_STDLIB_ROOT, or <exe dir>/stdlib), always
    //      last, so a project or dependency can shadow a stdlib package
    //      with its own file of the same dotted path
    std::vector<std::filesystem::path> extraRoots;

    bool checkOnly = false;
    if (argc >= 2 && std::string(argv[1]) == "--check") {
        checkOnly = true;
        for (int i = 2; i < argc; ++i) argv[i - 1] = argv[i];
        --argc;
    }

    // Backend and pipeline switches, so they can be written before the entry
    // file the same way --root can.
    std::string backendChoice;
    bool referenceCompiler = false;
    bool strictNative = false;
    int argi = 1;
    for (; argi < argc; ++argi) {
        std::string arg = argv[argi];
        if (arg == "--root") {
            if (argi + 1 >= argc) {
                std::cerr << "error: --root requires a path argument\n";
                return 2;
            }
            extraRoots.emplace_back(argv[++argi]);
        } else if (arg.rfind("--root=", 0) == 0) {
            extraRoots.emplace_back(arg.substr(7));
        } else if (arg == "--backend") {
            if (argi + 1 >= argc) {
                std::cerr << "error: --backend requires a name (bytecode or native)\n";
                return 2;
            }
            backendChoice = argv[++argi];
        } else if (arg.rfind("--backend=", 0) == 0) {
            backendChoice = arg.substr(10);
        } else if (arg == "--reference-compiler") {
            referenceCompiler = true;
        } else if (arg == "--strict-native") {
            strictNative = true;
        } else if (arg.size() > 1 && arg[0] == '-') {
            // A misspelled option used to be taken for the entry file, so
            // `zl --emit-mirr out prog.zl` reported "could not open file:
            // --emit-mirr" and exited 1 - a file-not-found for a file the user
            // never named. Options are checked before paths, and an unknown one
            // is a usage error.
            std::cerr << "error: unknown option '" << arg << "' (try --help)\n";
            return 2;
        } else {
            break; // first non-flag argument is the entry file
        }
    }

    if (argi >= argc) {
        std::cerr << "usage: zl [--backend <bytecode|native>] [--root <path>]... <file.zl> [program args...]\n";
        return 1;
    }
    const char* entryFile = argv[argi];

    // The reference path is the escape hatch that keeps the differential check
    // possible now that the pipeline is the shipped path: it is the old
    // AST -> bytecode compiler, kept exactly as it was, so
    // tools/mir_backend_diff.sh can compare it against the pipeline.
    if (const char* env = std::getenv("ZL_COMPILER")) {
        const std::string choice(env);
        if (choice == "ast" || choice == "reference") referenceCompiler = true;
        if (choice == "mir" || choice == "pipeline") referenceCompiler = false;
    }

    pipeline::Backend backend = pipeline::Backend::Bytecode;
    std::string backendError;
    if (!selectBackend(backendChoice, backend, backendError)) {
        std::cerr << "error: " << backendError << "\n";
        return 2;
    }

    // Direct editor/--check invocations do not pass through zlpkg, so discover
    // local path dependencies from the nearest zlpkg.toml. Git dependencies
    // remain managed by zlpkg and are not fetched by the compiler.
    if (checkOnly) {
        const auto manifest = findNearestManifest(entryFile);
        if (!manifest.empty()) {
            auto discovered = discoverLocalPackageRoots(manifest);
            extraRoots.insert(extraRoots.end(), discovered.begin(), discovered.end());
        }
    }

    int rootExitCode = 1;
    if (!appendStandardRoots(argv[0], extraRoots, rootExitCode)) return rootExitCode;

    // Anything after the entry file is passed through to main(args: list<string>).
    std::vector<std::string> programArgs;
    for (int i = argi + 1; i < argc; ++i) programArgs.emplace_back(argv[i]);

    try {
        pipeline::Options options = optionsForRun(backend);
        options.requireMain = !checkOnly;
        if (strictNative) options.strictNative = true;
        pipeline::Pipeline compiler(std::move(options));

        // Stages 1-2, shared by every command: parse the entry file plus
        // everything it (transitively) imports and merge them into one Program,
        // then run semantic analysis over it.
        if (!compiler.load(entryFile, extraRoots)) return reportPipelineFailure(compiler.result());
        if (!compiler.analyze()) return reportPipelineFailure(compiler.result());

        if (checkOnly) {
            // Check mode is parse + type-check only. The pipeline is stopped
            // before lowering: nothing is generated and nothing executes.
            if (compiler.result().options.verbose) std::cerr << compiler.result().describe() << "\n";
            return 0;
        }

        if (referenceCompiler) {
            // The reference path reuses the front end and then hands the checked
            // program to the original AST -> bytecode compiler. It exists to be
            // compared against, so it says so.
            std::cerr << "reference compiler: AST -> bytecode (not the MIR pipeline)\n";
            zl::Compiler astCompiler;
            auto chunk = std::make_shared<zl::Chunk>(astCompiler.compile(*compiler.result().program));
            zl::VM vm;
            return vm.run(std::move(chunk), programArgs);
        }

        // Stages 3-6: MIR, verification, optimisation, the selected backend.
        if (!compiler.lowerToMir()) return reportPipelineFailure(compiler.result());
        if (!compiler.verifyMir()) return reportPipelineFailure(compiler.result());
        if (!compiler.optimizeMir()) return reportPipelineFailure(compiler.result());
        if (!compiler.generate()) return reportPipelineFailure(compiler.result());

        const auto& result = compiler.result();
        for (const auto& diagnostic : result.loweringDiagnostics)
            std::cerr << "note: " << diagnostic << "\n";
        reportOptimization(result);
        reportStubs(result);
        reportNativeLedger(result);

        zl::VM vm;
        // The executed artifact is bytecode translated from the same verified
        // MIR whichever backend generated code, because the VM is the execution
        // driver in this phase. See docs/pipeline.md.
        // Shared ownership lets every closure and async invocation reference
        // this one chunk instead of deep-copying it per closure.
        auto chunk = std::make_shared<zl::Chunk>(std::move(*compiler.result().chunk));
        return vm.run(std::move(chunk), programArgs);
    } catch (...) {
        return reportRunException(std::current_exception());
    }
}

// The real main(): if the interpreter abandoned a deadlocked worker thread at
// teardown, exit without running static destructors - the abandoned thread may
// still touch global runtime state (GC heap, scheduler) during its teardown,
// and a use-after-free at process exit is worse than a clean _Exit.
int main(int argc, char** argv) {
    const int code = zlMain(argc, argv);
    std::cerr.flush();
    std::cout.flush();
    if (zl::gAbandonedWorkerThreads.load(std::memory_order_acquire)) std::_Exit(code);
    return code;
}
