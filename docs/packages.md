# Packages, Imports, and Dependencies

- [Imports](#imports)
- [One primary type per file](#one-primary-type-per-file)
- [The stdlib root](#the-stdlib-root)
- [External dependencies (`zlpkg`)](#external-dependencies-zlpkg)

## Imports

Java-style dotted imports: `import io.github.test.Helpers` resolves to
`<sourceRoot>/io/github/test/Helpers.zl`. `sourceRoot` is the nearest ancestor
directory literally named `src` that is reached **before** a project boundary
(`zlpkg.toml` or a `.git` file/directory). Hitting a project boundary first
stops the walk, so cloning into a path that happens to contain a directory named
`src` (for example `/home/src/app`) cannot steal imports from an unrelated tree.
If neither `src` nor a project boundary is found, the entry file's own directory
is used.

Transitive imports, diamond imports, and circular-import detection are all handled.

## One primary type per file

Every `.zl` file must declare one **primary type** whose name matches the file stem
exactly: `Helpers.zl` declares `class Helpers`, `Point.zl` declares `data Point`. The
loader enforces this for the entry file and for every imported file, and names the
file in the diagnostic when it does not hold:

```text
module error: file /path/to/split_test.zl must define primary type 'split_test'
```

The primary type does not have to be a `class`: `class`, `data`, `interface`,
`enum`, and `memory` declarations all satisfy the rule, so `Point.zl` may
declare `data Point` and `Shape.zl` may declare `interface Shape`. Helper
declarations may sit alongside it - a file whose primary type is `Helpers` may
also declare a second class, a `data`, an `enum`, and so on, in any order.

### Why the rule exists

**An import names a type, and resolving it is a path computation.** `import
io.github.test.Helpers` becomes `<sourceRoot>/io/github/test/Helpers.zl` by replacing
dots with separators - no file is opened, no declaration is read, and there is no
index of "which type lives where" to build or to keep fresh. The stem match is what
makes that one name do both jobs: the last segment is simultaneously the file to load
and the type it delivers. If `Helpers.zl` were allowed to declare `class Utils`
instead, the import would resolve to a file that does not contain the name it asked
for, and answering "where does `Utils` live?" would mean scanning every file in every
root.

**The dotted name is the module's identity, not just its address.** Diamond imports
are merged once, circular imports are refused, and a type declared twice is reported
as `class 'X' is defined in both <module> and <module>` - all keyed on the dotted
module name (`src/compiler/module_graph.cpp`, driven from
`src/compiler/module_loader.cpp`). One file, one name, one node in that graph. A file
that exported two importable types would be two nodes backed by one file, and then a
rename, a conflict report, or a cycle would have to say which of the two it meant.

**A diagnostic can name the file or the type, interchangeably.** `Point.zl:12` and
`Point` point at the same thing, which is worth more than it sounds when the message
is all the user gets.

### What that costs, and the escape hatch

Only the primary type is importable *by name*: `import app.LibHelper` fails with
`cannot resolve import` when `LibHelper` is a helper class inside `app/Lib.zl`, because
there is no `LibHelper.zl` to resolve to. The helper is not lost, though - importing
the file brings everything it declares with it, so `import app.Lib` makes both `Lib`
and `LibHelper` visible to the importer.

So the choice is about reach, not permission: a type that only matters next to its
primary type can live in the same file and still travel with it; a type meant to be
imported on its own gets its own file. The examples follow that convention:
`examples/intermediate/_lib/` holds one type per file precisely because the examples
import them individually (`Inheritance.zl` imports `Animal` and `Dog`;
`Interfaces.zl` imports `Named`, `Shape` and `Circle`).

## The stdlib root

On top of a project's own imports, the interpreter searches a **stdlib root**:
`$ZL_STDLIB_ROOT` if set, otherwise `<the zl_language executable's directory>/stdlib`.

- **`zl.lang`** is auto-imported everywhere, no `import` needed.
- **Everything else** requires an explicit `import`, and the compiler rejects a call to
  a package that was not imported in that file — `import zl.util.Queue` must precede any
  use of `Queue.newQueue()`.

Stdlib packages are not special-cased at the resolver level. A stdlib package is just
another `.zl` file the loader happens to find via the second root, resolved through the
exact same code path as a project's own `import pkg.Helpers`.

Full lookup precedence is documented in [development.md](development.md#stdlib-lookup).

## External dependencies (`zlpkg`)

Beyond the project root and the stdlib root, `zl_language` searches any number of
additional roots passed as repeatable `--root <path>` flags, or listed in the
`ZL_EXTRA_ROOTS` environment variable (OS path-list separated: `;` on Windows, `:` on
POSIX). They are tried in the order given, before the stdlib root.

`zlpkg` — a separate CLI built alongside `zl_language` from the same `CMakeLists.txt` —
turns a dependency graph into that list of roots automatically:

```bash
# in a project directory containing zlpkg.toml:
zlpkg install            # resolve dependencies, write zlpkg.lock
zlpkg install --update   # force a fresh resolve, ignoring any existing lock
zlpkg run src/Main.zl    # install if needed, then run with dependencies wired in
```

`zlpkg run` also verifies that the runtime next to `zlpkg` reports a matching version.

### `zlpkg.toml`

```toml
[package]
name = "myproject"
version = "0.1.0"

[dependencies]
geom = { path = "../geom" }                                       # local, for dev/fixtures
ecs  = { git = "https://example.com/zl-ecs.git", ref = "v1.0.0" } # ref is a tag/branch/commit
```

Omit `ref` to track the default branch.

### Versioning convention

- Every installable package declares `[package] name` and an exact `version`; the
  project convention is `MAJOR.MINOR.PATCH`.
- A dependency may request an exact version with `version = "..."` in its inline table.
  `zlpkg` verifies the resolved manifest declares exactly that version. There is
  intentionally no version-range or registry resolution yet.
- Package names must agree between the dependency key and the dependency's own manifest.
- `zlpkg.lock` records the resolved version alongside source and ref, so the dependency
  API identity is visible in the lockfile.

### Resolution

Each dependency's own `zlpkg.toml`, if present, is resolved transitively. If the same
dependency name is reached through two different paths in the graph and they resolve to
different content — a different commit, or a different local path — `zlpkg` reports a
version-conflict error naming both requesters rather than silently picking one.

`zlpkg.lock` records the exact resolved commit or path for each dependency and is meant
to be committed, like `Cargo.lock`. Only the `.zlpkg/` cache directory it populates
should be gitignored.

The lock is a cache, not an authority: every `install`/`run` validates it against the
current manifests (a fingerprint of the project's own manifest plus each dependency's
dependency list) and reuses it only when everything still matches. If any manifest
changed - or an entry no longer checks out (missing directory, wrong package, cache
that fails verification) - the lock is treated as stale: it is re-resolved from
scratch and rewritten, with a notice saying why. `zlpkg install --update` forces that
re-resolve even when the lock is fresh.

Dependencies are fetched over git by shelling out to the system `git` CLI. No registry
or index exists yet, so every dependency must state exactly where it comes from —
`path` or `git`, never a bare version number.

Git remotes must be `https://`, `ssh://`, or scp-like `user@host:path`.
`http://`, `git://`, `file://`, and helper transports such as `ext::` are rejected.
