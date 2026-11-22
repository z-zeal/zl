---
name: claude-mem
description: Use when starting or ending a coding session, or when a decision, workaround, or architectural constraint needs to survive into later sessions. Maintains a persistent, searchable project memory in plain Markdown.
---

# Persistent Project Memory

Agents forget everything between sessions. This skill keeps the important parts
in files, so the next session starts from knowledge instead of from scratch.

## Layout

Keep memory at the project root, under `memory/`:

```text
memory/
  MEMORY.md            # index: what is remembered, and where
  domains/
    <domain>.md        # one file per area of the system
  decisions/
    YYYY-MM-DD-<slug>.md
```

Never store secrets, credentials, or large artifacts in memory. Reference them
by path instead.

## At the start of a session

1. Read `memory/MEMORY.md`.
2. Read the `domains/` file for any area you are about to touch.
3. If the index is missing or empty, create it and note that memory starts here.

## While working

Write an entry when any of these happen:

- A non-obvious **decision** is made, or a request exposes a constraint.
- A **workaround** or surprising behavior is discovered.
- A **fact** about the codebase that is not derivable from reading it (build
  quirks, verification commands, who owns what, why something is disabled).
- A **correction** — the user redirects your approach.

Do not write entries for things the code already says plainly.

## Entry format

Decisions go in `decisions/YYYY-MM-DD-<slug>.md`:

```markdown
# <short title>

- Date: YYYY-MM-DD
- Status: accepted | superseded by <link>
- Context: why this came up
- Decision: what was chosen
- Consequences: what it costs or enables
- Evidence: command output, file, or conversation that supports it
```

Domain notes go in `domains/<domain>.md` as short bullet entries with a date:

```markdown
- 2026-11-22 — MIR is the compiler boundary; both backends consume verified MIR. (docs/pipeline.md)
```

## Index format

`MEMORY.md` is a table, newest area first:

```markdown
| Topic | File | Updated |
| --- | --- | --- |
| Compiler pipeline | domains/pipeline.md | 2026-11-22 |
| ADR: native backend scope | decisions/2026-11-20-native-scope.md | 2026-11-20 |
```

## At the end of a session

1. Re-read what you wrote during the session.
2. Update `MEMORY.md` so every entry is findable.
3. Mark superseded decisions instead of deleting them.
4. Report to the user what was remembered and what was pruned.

## Maintenance

Periodically, and whenever a domain file grows past roughly 200 lines:

- Merge overlapping entries.
- Drop entries that no longer match the code — verify before dropping.
- Prefer linking to the code or docs over restating them.
