# Agentic Awesome Skills (index)

[Vendored index](CATALOG.md) of
[**sickn33/agentic-awesome-skills**](https://github.com/sickn33/agentic-awesome-skills)
— a community library of 3,055+ installable `SKILL.md` playbooks for Claude
Code, Codex, Cursor, Gemini CLI, and other agents.

- Catalog generated 2026-11-20 (upstream release V21.1.0).
- Original code and tooling: MIT ([`LICENSE`](LICENSE)).
- Original written content: CC BY 4.0 ([`LICENSE-CONTENT`](LICENSE-CONTENT)).
- Imported skills keep their own upstream licenses and attribution requirements.

## What is vendored here

Only the generated catalog, [`CATALOG.md`](CATALOG.md) — a searchable table of
every skill with its description, risk level, source repository, tags, and
triggers, grouped by category (agent-behavior, agent-orchestration, coding,
design, data, and so on).

The full skill tree is **not** vendored: it is 3,130 skills and roughly 132 MB,
which is too large to carry in this repository. Clone upstream when you need
the files:

```sh
git clone --depth 1 https://github.com/sickn33/agentic-awesome-skills.git
```

A machine-readable index is also available upstream as `skills_index.json`, and
the hosted catalog is at <https://aaskills.tech/core/>.

## Finding a skill

Search the catalog by category, tag, or trigger:

```sh
grep -n 'code review' skills/agentic-awesome-skills/CATALOG.md
grep -n '^## ' skills/agentic-awesome-skills/CATALOG.md   # list categories
```

The `Source` column names the upstream repository for each skill. Clone that
repository and copy the skill folder into your agent's skills directory.

## Adding a selected skill to this repo

Once you have picked a skill from the catalog, vendor it under the relevant
pack folder (or a new one) and record it in [`../README.md`](../README.md), so
the rest of the team gets it too. Prefer a handful of reviewed skills over
importing the whole catalog.
