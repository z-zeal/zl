# ZL Language

ZL is a small, statically typed, interpreted language implemented in C++17.

This repository contains the full toolchain: lexer, parser, type checker, a typed
mid-level IR (MIR) that is the compiler's boundary, two backends that translate
it (bytecode for the shipped VM, and a restricted native/AOT backend), the
virtual machine itself, native runtime helpers, a standard library written mostly
in ZL itself, and `zlpkg`, a git/path dependency manager.

The compiler is one pipeline, and every command is that pipeline with different
stage options:

```text
ZL source → lexer/parser → semantic analysis → typed lowering → MIR
          → MIR verification → MIR optimisation → selected backend
                                                ├── bytecode → VM
                                                └── native   → machine code
```

Backend selection (`zl --backend bytecode|native`, or `ZL_BACKEND`) never changes
what a program means: both backends are handed the same verified MIR, and
`zl --pipeline-report` prints the digest that proves it. See
[docs/pipeline.md](docs/pipeline.md).

```zl
class Hello {
    func main(): void {
        var names = new List<string>()
        names.push("Ada")
        names.push("Grace")
        names.push("Alan")
        names.forEach(func(n) => log("Hello, " + n))
    }
}
```

## Contents

- [Requirements](#requirements)
- [Build](#build)
- [Run a program](#run-a-program)
- [Install](#install)
- [Project layout](#project-layout)
- [Language at a glance](#language-at-a-glance)
- [Documentation](#documentation)
- [Current limitations](#current-limitations)
- [License](#license)

## Requirements

- CMake 3.10 or newer
- A C++17-capable compiler (MSVC, GCC, or Clang)
- `git` on `PATH` if you use `zlpkg` git dependencies

## Build

The portable path is plain CMake, on every platform:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target zl-core
```

`zl-core` is the default developer build: the toolchain only. Tests, benchmarks
and examples are behind the `zl-tests` / `zl-full` aggregate targets.

That produces both binaries: `zl_language` (the runtime) and `zlpkg` (the dependency manager).

Convenience wrappers are also provided:

| Task | Linux / macOS | Windows |
| --- | --- | --- |
| Build (core toolchain) | `scripts/build.sh` | `scripts\build.bat` |
| Build tests | `scripts/build.sh test` | `scripts\build.bat test` |
| Build everything | `scripts/build.sh full` | `scripts\build.bat full` |
| Clean | `scripts/build.sh clean` | `scripts\build.bat clean` |
| Run a file | `scripts/zl <file.zl>` | `scripts\run.bat <file.zl>` |
| Run examples | `scripts/run_examples.sh` | `scripts\run_examples.bat` |
| Run + verify examples | `examples/run_all.sh` | `examples\run_all.bat` |
| Run regressions | `scripts/run_regressions.sh` | `scripts\run_regressions.bat` |
| Native compiler gate | `scripts/native_gate.sh` | `scripts\native_gate.ps1` |

`scripts/build.sh` and `scripts\build.bat` are the same driver for their platform.
Both take a mode - `core` (default), `test`, `full`, `clean`, `help` - plus
`--debug`, `--target <name>`, `--jobs <n>`, `--ctest` and `--run [file]`.
The default mode builds only the toolchain (3 binaries); it never pulls in
tests, benchmarks or examples.

## Run a program

```bash
build/zl_language examples/basics/HelloWorld.zl arg1 arg2
```

```text
zl <file.zl> [program args...]
```

The runtime also supports `zl --version`, `zl --help`, and repeatable `--root <path>`
flags for extra import roots.

> **File rule:** each `.zl` file must declare one *primary type* whose name matches the
> file stem exactly — `Hello.zl` must declare `class Hello`. A `data`, `interface` or
> `enum` satisfies the rule just as well, and helper declarations may share the file;
> only the primary type is importable by name. Enforced by the compiler, and explained
> (with the reason) in [docs/packages.md](docs/packages.md#one-primary-type-per-file).

## Install

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cmake --install build --prefix <install-prefix>
```

The install tree contains `bin/zl` (launcher), `bin/zl_language`, `bin/zlpkg`,
`lib/zl/stdlib/`, and the public headers. An installed runtime finds its bundled
stdlib without any environment variables.

Environment variables: `ZL_STDLIB_ROOT`, `ZL_HOME`, `ZL_EXTRA_ROOTS`, and the
pipeline switches (`ZL_BACKEND`, `ZL_MIR_OPT`, `ZL_MIR_OPT_PASSES`, `ZL_MIR_PROMOTE`,
`ZL_NATIVE_STRICT`, `ZL_PIPELINE_VERBOSE`, `ZL_COMPILER`); `zl --help` lists them
with what each one does.
See [docs/development.md](docs/development.md#stdlib-lookup) for the lookup precedence.

## Project layout

```text
include/zl/    Public headers for the runtime and compiler
src/           C++ implementation of the compiler, VM, and natives
               src/mir/ is the mid-level IR and compiler boundary (docs/mir.md)
               src/compiler/pipeline.cpp is the stage pipeline (docs/pipeline.md)
               tools/boundary_lint.sh enforces the boundary rules in CI
zlpkg/         C++ implementation of the zlpkg CLI (separate target)
stdlib/        Standard-library packages beyond auto-imported zl.lang
tools/         Developer tooling, including zl-lsp, zl-test, and the zl-bind binding generator
benchmarks/    Native-boundary performance gates
packaging/     CPack release packaging configuration
scripts/       Build, run, regression, native-gate, and binding helpers
docs/          Reference documentation (see below)
build/         Local CMake build directory (not committed)
```

## Language at a glance

- `var` / `let` declarations with type inference or explicit types
- Primitives `int`, `double`, `decimal`, `bool`, `string`, plus union types (`int|string`)
- `class`, `data` records, `enum`, interfaces, inheritance, and `public`/`private`/`protected`
- Generic classes, including `List<T>`, `Map<K,V>`, and `Set<T>` as real compiled generics
- Lambdas — `func(x) => x * 2` and `func(x) { ... }` — with by-value capture
- Function-type signatures — `func(int, string): bool` as a parameter, return, or
  declaration type, checked at the call site; bare `func` stays dynamically checked
- Operator overloading via `operator +(...)` declarations
- `if` / `elif` / `else`, `for`, `while`, `repeat`, `break`, `continue`
- `async func`, `await`, `Task<T>`, and `Shared<T>` for concurrency
- Java-style dotted imports with an auto-imported `zl.lang`
- Minimal name-based reflection through `Type.*`
- `@native` opt-in AOT compilation and `@ffi` native bindings

Full syntax and semantics: **[docs/language-guide.md](docs/language-guide.md)**.

## Documentation

| Document | Covers |
| --- | --- |
| [docs/language-guide.md](docs/language-guide.md) | Types, lambdas, generics, records, operators, reflection |
| [docs/stdlib.md](docs/stdlib.md) | `zl.lang`, `Math`, `zl.test`, `zl.logging`, `zl.text`, `zl.serialize`, `zl.time`, `zl.crypto` |
| [docs/packages.md](docs/packages.md) | Imports, the stdlib root, and `zlpkg` dependency management |
| [docs/native.md](docs/native.md) | Native boundary, `@native` compilation, `zl-bind`, FFI, async |
| [docs/pipeline.md](docs/pipeline.md) | The compiler pipeline: stages and their invariants, backends, backend selection, exit codes |
| [docs/mir.md](docs/mir.md) | MIR — the typed mid-level IR: structures, invariants, verifier, lowering |
| [docs/native-backend.md](docs/native-backend.md) | The native backend: target abstraction, calling conventions, value classes, native IR, MIR → machine code |
| [docs/mir-optimizer.md](docs/mir-optimizer.md) | The MIR optimiser: pass/analysis managers, effect classification, the safety contract, differential validation |
| [docs/tooling.md](docs/tooling.md) | Editor/test tooling, `zl-lsp`, `zl-test`, fast diagnostics, document symbols |
| [docs/development.md](docs/development.md) | Build details, stdlib lookup, testing, packaging |
| [docs/memory-domains.md](docs/memory-domains.md) | Design direction: user-defined automatic memory domains (arena/pool/`memory` blocks) beside the tracing GC |
| [examples/](examples/README.md) | 46 runnable examples, basics through async, each with verified output |
| [examples/REVIEW.md](examples/REVIEW.md) | Bugs found while writing them, fixed and open |
| [TASKS.md](TASKS.md) | The open work list: what is left, what it would take, and what is already stale |
| [docs/changelog.md](docs/changelog.md) | Dated development checkpoints by phase |

The memory-model direction — ownership combined with tracing GC, GC-managed and
thread-confined by default, with `shared` explicit rather than implicit — is
summarized in [docs/language-guide.md](docs/language-guide.md#memory-model-direction).
The plan for extending it so programmers can write their own automatic memory
strategies (memory domains: arena, pool, user-defined) is
[docs/memory-domains.md](docs/memory-domains.md).

## Current limitations

- The standard library is deliberately hybrid: high-level APIs live in ZL, while the
  VM, OS access, parsing engines, and storage primitives stay native.
- Async is cooperative: `async func`, `Task<T>`, `await`, `block()`/`ignore()`/`cancel()`,
  and async lambdas (`async func(x) => ...`) work. `cancel()` requests cancellation, which a
  task observes at its next `await` and passes on to the tasks spawned from its body; a
  synchronous `Task.spawn` closure cannot notice mid-run, so it settles as cancelled at
  completion. A task dropped without `block()`/`ignore()` reports its failure to stderr at
  teardown instead of vanishing (exit status unchanged by design).
- `zlpkg` has no registry — dependencies resolve by `git` URL or local `path` only,
  with no version ranges, workspaces, or dev-dependencies.
- One primary type per file, matching the file stem (helper declarations may share the
  file, but only the primary type is importable by name) —
  [the rule and its rationale](docs/packages.md#one-primary-type-per-file).
- `Shared<T>` makes a capture legal, not safe: what is checked at compile time is the
  *capture*, not the arithmetic. Each cell operation is synchronised, but a
  `get()`/`setValue()` read-modify-write can still interleave and lose the update — use
  `withLock`, `Atomic`, or `Mutex` for that
  ([the measurement and its regression](tests/zl/valid/concurrency_regressions/SharedLostUpdateMeasurement.zl)).

## License

Licensed under the **PolyForm Noncommercial License 1.0.0 with Runtime Exception**.
See [LICENSE](LICENSE) for the full text.
