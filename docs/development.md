# Development

- [Build](#build)
- [Stdlib lookup](#stdlib-lookup)
- [Source organization](#source-organization)
- [Testing](#testing)
- [Distribution builds](#distribution-builds)

## Build

Portable, on every platform:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target zl-core
```

### Aggregate targets

The build is organized around four project-level aggregates, so a default
developer build is not a complete repository build:

| Target | Contents |
| --- | --- |
| `zl-core` (default) | `zl_language` (runtime/compiler/VM), `zlpkg`, `zl-bind` and the full source closure they need |
| `zl-tests` | `zl-core` plus every C++ regression executable and the native-library test fixture |
| `zl-benchmarks` / `zl-examples` | `zl-core` plus the prerequisites of the benchmark gate and the example runners |
| `zl-full` | everything above - the intentional complete build |

`zl` is an alias for `zl-core`. Test executables are marked `EXCLUDE_FROM_ALL`,
so they are only ever built through `zl-tests` / `zl-full`; they remain fully
registered with CTest and are never removed from the configuration.

The Python-based `zl-lsp` and `zl-test` tools do not need compilation.

### Build scripts

`scripts/build.sh` (Linux/macOS) and `scripts\build.bat` (Windows) wrap the same
configuration with identical mode semantics:

```bash
scripts/build.sh              # core toolchain only (target zl-core)
scripts/build.sh test         # core + all test executables (target zl-tests)
scripts/build.sh test --ctest # ...and then run the suite
scripts/build.sh full         # complete repository (target zl-full)
scripts/build.sh clean        # delete the build directory
scripts/build.sh help         # explain the modes
scripts/build.sh --debug --target zl_language --jobs 8
scripts/build.sh --run examples/basics/HelloWorld.zl
```

```bat
scripts\build.bat            :: core toolchain only (target zl-core)
scripts\build.bat test       :: core + all test executables (target zl-tests)
scripts\build.bat test --ctest
scripts\build.bat full       :: complete repository (target zl-full)
scripts\build.bat clean      :: delete the build directory
scripts\build.bat help       :: explain the modes
scripts\build.bat --debug
scripts\build.bat --target zl_language
scripts\build.bat --jobs 8
```

### Install

```bash
cmake --install build --prefix <install-prefix>
```

Produces `bin/zl` (launcher), `bin/zl_language`, `bin/zlpkg`, `lib/zl/stdlib/`, and the
public headers. Development builds continue to produce `zl_language` directly.

## Stdlib lookup

The runtime resolves its standard library in this order:

1. An explicit `ZL_STDLIB_ROOT`
2. `ZL_HOME/lib/zl/stdlib`
3. The `stdlib` directory beside the executable
4. The installed `<prefix>/lib/zl/stdlib` layout

`ZL_EXTRA_ROOTS` is an ordered dependency/search list inserted *before* the stdlib root.
It uses `;` as the path-list separator on Windows and `:` on POSIX platforms.

The runtime validates `lib/zl/stdlib/VERSION` against its own version before loading a
program, and `zlpkg run` verifies that the adjacent runtime reports a matching version.

## Source organization

- A standard CMake executable target defined in `CMakeLists.txt`.
- The main executable runs one pipeline: load (`ModuleLoader`), semantic analysis
  (`TypeChecker`), typed lowering to MIR, MIR verification, MIR optimisation, and
  the selected backend. `zl file.zl` is that pipeline with the bytecode backend,
  whose chunk the VM executes. `zl --backend native file.zl` is the same pipeline
  with the native backend generating machine code for the supported subset, but
  the program still executes on the VM; mixed-mode native execution inside a
  program is not implemented (the compiled functions do run, stand-alone, via
  `zl --run-native` - see [native-backend.md](native-backend.md)). Every command in
  `main.cpp` is a set of stage options over
  `zl::pipeline::Pipeline` (`include/zl/compiler/pipeline.hpp`), not a pipeline
  of its own. See [`pipeline.md`](pipeline.md) for the stages, their invariants,
  the backend contract and the exit codes.
- The `zl_language` sources are listed explicitly in `CMakeLists.txt` (no
  `file(GLOB_RECURSE ...)`), so a stray `.cpp` under `src/` does not silently
  join the link.
- Errors are reported in three categories: `syntax error`, `compile error`, and
  `runtime error` (plus `module error` for import/package problems).
- `stdlib/` is copied next to the built binary so a development build can find it.
- `src/mir/` and `include/zl/mir/` hold MIR, the mid-level IR and the compiler
  boundary. Backends consume only MIR; the lowerer is the only place that knows
  the AST exists. See [`mir.md`](mir.md) for its invariants and design decisions,
  and [`mir-optimizer.md`](mir-optimizer.md) for stage 5.
- A new language construct that changes *when memory returns* - a memory domain -
  is specified in [`memory-domains.md`](memory-domains.md). It lists the nine files
  one new MIR opcode touches and the gates that must stay green, so a design change
  at that boundary can be costed before it is started.
- `src/compiler/compiler.cpp` is the reference AST → bytecode compiler, kept
  unchanged so the MIR pipeline can be differentially tested against it
  (`ZL_COMPILER=ast`, `tools/mir_backend_diff.sh`). It is no longer the default
  path.
- `src/compiler/ir.cpp` is the older, untyped `zl::ir`. It has one remaining
  consumer - the portable-C++ emitter behind `--emit-native` - and is frozen:
  new constructs go into `zl::mir`.`

## Testing

C++ unit tests cover the lexer, parser, compiler, VM, IR, MIR, regex engine,
scheduler, native compiler, and FFI layers; they are declared as separate
executables in `CMakeLists.txt`. Required tests fail configuration if their
source is missing rather than being skipped. If you extend the language, add
tests for the affected layer.

`.github/workflows/ci.yml` configures the tree, builds `zl-tests`, and runs
`ctest` on every push and pull request.

MIR has six targets, split by what they link:

```bash
cmake --build build --target zl-pipeline-tests       # the pipeline contract
cmake --build build --target zl-mir-tests            # verifier regressions
cmake --build build --target zl-mir-ssa-tests        # CFG, data flow, promotion
cmake --build build --target zl-mir-lowering-tests   # end-to-end lowering
cmake --build build --target zl-mir-opt-tests        # optimiser, hand-built MIR
cmake --build build --target zl-mir-opt-pipeline-tests   # optimiser, real programs
./build/zl-pipeline-tests && ./build/zl-mir-tests && ./build/zl-mir-ssa-tests
./build/zl-mir-lowering-tests && ./build/zl-mir-opt-tests && ./build/zl-mir-opt-pipeline-tests
```

`zl-pipeline-tests` owns the composition: stage order and the ledger, the
invariant that no backend is handed unverified MIR (including a module mutated
after the verification stage), backend selection leaving the MIR boundary
byte-identical and the program's behaviour unchanged, and the failure-kind to
exit-code mapping. It links the whole compiler minus `main()`, because a pipeline
assembled out of mocks would prove nothing about the one that ships.

`zl-mir-tests` builds MIR by hand and checks the verifier rejects each class of
malformed module; it links only the MIR sources. `zl-mir-ssa-tests` does the same
for the CFG queries, the data-flow analyses and slot→block-parameter promotion,
including the malformed-SSA cases the verifier must reject and the slots the
promotion must decline. `zl-mir-lowering-tests` drives `ModuleLoader` →
`TypeChecker` → `lowerProgram` → `verifyModule` on small programs, so it links the
whole compiler minus `main()`.

`zl-mir-opt-tests` is the optimiser's specification: pass manager, analysis
caching, verification between passes, the effect table, each pass and its
declination rules — hand-built MIR, no front end, so it runs in seconds.
`zl-mir-opt-pipeline-tests` is the other half, and links the whole compiler minus
`main()` like the lowering target: it lowers real ZL programs, optimises them,
checks the two modules statically with `compareModules`, then runs both through
the bytecode backend and compares their output and exit code. Both halves matter:
the static check sees structure and observable events, the run sees values and the
runtime checks the static check deliberately ignores.

The ZL-level corpus is split in two:

- **Regression fixtures** under `tests/zl/valid/` and `tests/zl/invalid/`, grouped by
  category (`core_tests/`, `import_tests/`, `package_tests/`,
  `package_manager_tests/`, `language_hardening_tests/`, `records/`, …). These are
  permanent and are the source of truth for language behavior.
- **Examples** under `examples/` — a small, curated, user-facing set kept deliberately
  separate from the regression corpus.

> Note: the `.zl` corpus and `examples/` are not part of every checkout. The runner
> scripts below discover cases from the filesystem, so they only report what is present.

### Running

```bash
# interactive: pick 1) Test All  2) Invalid  3) Valid, then which categories to run
./scripts/run_examples.sh build/zl_language

# non-interactive
./scripts/run_examples.sh build/zl_language all         # everything
./scripts/run_examples.sh build/zl_language invalid     # every invalid category
./scripts/run_examples.sh build/zl_language valid 1,3   # only categories 1 and 3
./scripts/run_examples.sh build/zl_language --list      # list categories/cases and exit
```

`scripts/run_examples.bat` takes the same arguments:
`scripts\run_examples.bat build\zl_language.exe [all|invalid|valid] [selection]`.

Use `scripts/run_regressions.sh` / `scripts/run_regressions.bat` for the full permanent
regression corpus, including package-manager cases, and `scripts/native_gate.sh` /
`scripts/native_gate.ps1` for the native compiler gate.

Five differential harnesses cover the pipeline on `examples/`:

```bash
tools/backend_diff.sh ./build/zl_language         # reference | bytecode | native
tools/mir_backend_diff.sh ./build/zl_language     # reference vs the MIR pipeline
tools/mir_opt_diff.sh ./build/zl_language         # optimised vs unoptimised
tools/mir_promotion_diff.sh ./build/zl_language   # block params vs store/load
tools/mir_opt_check_all.sh ./build/zl_language    # the same, statically, wider
```

The first is the backend-selection gate: every program is compiled and run three
times - the reference AST → bytecode compiler (`ZL_COMPILER=ast`), the bytecode
backend, and the native backend - and all three must print the same bytes and
exit the same way, with `--pipeline-report` confirming per program that both
backends were handed the same MIR. It is currently 50 of 50. The second compares
the reference path against the MIR pipeline (`--mir-vm`) and must stay at 50
matching with 0 documented fail-closed gaps. The third compares `--mir-vm` with
and without the optimiser (`ZL_MIR_OPT=0` / `=1`) and must report 0 differing; it
is currently 50 of 50, none skipped. The fourth compares the memory and value
forms of the same MIR (`ZL_MIR_PROMOTE=0` / `=1`, optimiser pinned off); it is
currently 50 of 50. The fifth does not need a backend at all: it runs
`zl --mir-opt-check` over every `.zl` in `examples/` *and* `stdlib/`, which is how
the optimiser gets checked against the generics, async, task, lock and FFI code
the bytecode backend cannot execute yet; it is currently 76 of 76 equivalent.
All five are the check that a change to the IR, the promotion pass, the
optimiser, or a backend did not alter behaviour.

The boundary itself is checked without running anything:

```bash
tools/boundary_lint.sh                            # rules 1-4, source-level
```

It fails when a backend includes the AST, the parser, the type checker, the
module loader, the pipeline or the legacy IR; when the typed lowerer touches the
checker except through `expressionType`; when anything outside the pipeline
constructs a `ModuleLoader` or a `TypeChecker`; or when the legacy `zl::ir` gains
a second consumer. It is registered with CTest as `boundary-lint`, so the rules
in [pipeline.md](pipeline.md) are enforced rather than remembered.

The first two set their legs up explicitly (`ZL_COMPILER=ast`, `--mir-vm`,
`--backend ...`) and verify each leg really is the path it claims to be before
comparing anything: now that the default path is the MIR pipeline, a harness that
compared "the default" against "the pipeline" would be a green line measuring
nothing.

Both discover the `_lib` module roots themselves and set `ZL_EXTRA_ROOTS`, so a
program that imports a sibling module is genuinely compared instead of failing to
load on both paths and being counted as a match. If a corpus program stops being
compared, that shows up as a change in the counts, not as a silent pass.

### On CI

`.github/workflows/ci.yml` runs exactly the commands above on every push to
`main` and every pull request. The Linux job is the full gate: the `zl-tests`
build, the whole CTest suite, the regression corpus
(`scripts/run_regressions.sh … all`), the five differential harnesses, and the
native compiler gate. macOS and Windows build and run CTest so the
"portable, on every platform" claim is verified rather than assumed; the
native-execution tests guard themselves and skip where executable memory is
unavailable. Nothing in the workflow is CI-only, and every gate exits
non-zero on a mismatch, so a green check is the claim above, not a hope.

## Distribution builds

Release packaging is driven by CMake/CPack — see [`packaging/README.md`](../packaging/README.md).

On Linux the release configuration produces native TGZ, ZIP, and DEB packages. Windows
and macOS releases should be built on native runners using the same CMake/CPack
configuration; the repository includes a GitHub Actions release matrix for those builds.

## MIR safety evaluation

The non-executing `--safety-check` command and the adversarial-corpus evaluation
harness distinguish parser, semantic, MIR, and runtime outcomes without bypassing
earlier checks. See [MIR safety analysis](mir-safety.md) for the property matrix,
abstract domains, source provenance, reproduction commands, and limitations.
