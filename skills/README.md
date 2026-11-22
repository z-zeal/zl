# Skills

Agent skill packs vendored into this repository. Each subfolder is a
self-contained pack of [Agent Skills](https://agentskills.io): directories with
a `SKILL.md` (YAML frontmatter + instructions) plus any scripts, templates, and
resources the skill needs.

Retrieved 2026-11-22.

## Packs

| Pack | Upstream | License | Contents |
| --- | --- | --- | --- |
| [`superpowers/`](superpowers) | [obra/superpowers](https://github.com/obra/superpowers) | MIT | 14 skills: brainstorming, writing/executing plans, TDD, systematic debugging, code review, subagent-driven development, git worktrees, verification before completion |
| [`anthropics/`](anthropics) | [anthropics/skills](https://github.com/anthropics/skills) | Apache-2.0 | 19 official skills: document creation (docx/pdf/pptx/xlsx), skill-creator, mcp-builder, webapp-testing, frontend/canvas/brand design, internal comms |
| [`real-engineer/`](real-engineer) | [mattpocock/skills](https://github.com/mattpocock/skills) | MIT | 31 skills ("Skills for Real Engineers"): engineering (TDD, code review, diagnosing bugs, domain modeling, architecture, specs, tickets), productivity, misc |
| [`claude-mem/`](claude-mem) | [thedotmack/claude-mem](https://github.com/thedotmack/claude-mem) | Apache-2.0 | Persistent-memory workflow skill (see note below) |
| [`agentic-awesome-skills/`](agentic-awesome-skills) | [sickn33/agentic-awesome-skills](https://github.com/sickn33/agentic-awesome-skills) | CC BY 4.0 / see `LICENSE-CONTENT` | `CATALOG.md` index of 3,130 community skills (the full set is not vendored) |

## Notes on the source list

Two of the repositories named in the original request do not exist:

- `thedannychoi/claude-mem` — no such user or repository. The widely used
  project with that name is [`thedotmack/claude-mem`](https://github.com/thedotmack/claude-mem).
  It is an application (hooks + worker + vector store), not a skill pack, so it
  is represented here by a portable memory-workflow skill rather than vendored
  source. See [`claude-mem/README.md`](claude-mem/README.md).
- `real-engineer/skills` — no such repository. The project described
  ("Skills for Real Engineers") is [`mattpocock/skills`](https://github.com/mattpocock/skills),
  vendored under [`real-engineer/`](real-engineer).

`sickn33/agentic-awesome-skills` is a 132 MB collection of 3,130 skills. Only its
generated catalog is vendored; the full tree is available upstream.

## Using these skills

Skill discovery is relative to the agent's configuration directory, not to this
repository, so point your agent at the pack you want:

- **Claude Code** — copy or symlink skills into `.claude/skills/`:

  ```sh
  ln -s "$PWD/skills/superpowers/test-driven-development" .claude/skills/
  ```

  or register an upstream repo as a plugin marketplace instead of vendoring:

  ```
  /plugin marketplace add obra/superpowers
  ```

- **Other agents** — consult the agent's skill-loading documentation; the
  on-disk format (`<skill-name>/SKILL.md` with `name`/`description` frontmatter)
  is the shared Agent Skills standard.

## Adding or updating a pack

1. Clone the upstream repository.
2. Copy the skill folders (and the upstream `LICENSE`) into the matching
   subfolder here.
3. Update the table above and the pack's `README.md`.
