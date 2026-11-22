# claude-mem

A portable persistent-memory workflow for coding agents.

## Upstream

The repository named in the original request, `thedannychoi/claude-mem`, does
not exist. The project with that name is
[**thedotmack/claude-mem**](https://github.com/thedotmack/claude-mem)
(Apache-2.0), a Claude Code plugin that captures tool usage, compresses
observations with the Claude Agent SDK, and injects relevant context into later
sessions via a local vector store (Chroma) and SQLite database.

That project is an installable application (hooks + background worker), not a
folder of skills, so its source is not vendored here. [`SKILL.md`](SKILL.md)
captures the same workflow in a harness-neutral form that any agent can follow
with plain files and search.

## Install the real thing

```sh
# Claude Code plugin marketplace
/plugin marketplace add thedotmack/claude-mem
```

See <https://docs.claude-mem.ai> for setup and supported integrations
(Claude Code, Codex, Gemini, Copilot, OpenCode, and others).

## Using the vendored skill

[`SKILL.md`](SKILL.md) keeps memory in a `memory/` directory at the project root:

- `memory/MEMORY.md` — index of what is remembered and where.
- `memory/domains/<domain>.md` — one file per area of the system.
- `memory/decisions/` — dated architectural decision records.

Nothing here runs automatically; the agent reads the index at the start of a
session and writes entries as decisions are made. Keep entries short, dated,
and evidence-backed, and prune stale ones.
