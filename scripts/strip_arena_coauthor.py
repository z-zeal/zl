#!/usr/bin/env python3
"""Strip Arena AI co-author attribution from a commit message.

Usage:
    strip_arena_coauthor.py                 # filter: stdin message -> stdout
    strip_arena_coauthor.py --check-range A..B
    strip_arena_coauthor.py --check-message-file FILE

Why this exists: Arena adds a trailer like

    Co-authored-by: arena-agent <297053741+arena-agent@users.noreply.github.com>

to the commits it helps write. GitHub turns any ``Co-authored-by:`` trailer
into a real co-author: the account gets an avatar on the commit, shows up
in the PR's author list, and is counted as a repository contributor.
Deleting those lines before the commits are shared is the only way to stop
that attribution - removing a repository *collaborator* does nothing,
because the bot was never a collaborator, it is a co-author.

The filter is deliberately narrow. It only deletes attribution lines that
mention Arena, so ordinary co-authors (people, or other tools the author
actually wants credited) are left alone:

  * ``Co-authored-by: ...arena...``   (any casing, [bot] suffixes included)
  * ``Co-Authored-By: arena-ai-coding-agent[bot] <...@users.noreply.github.com>``
  * a bare ``Generated-by:`` / ``Authored-by:`` Arena line, if one appears

Everything else - subject, body, sign-offs, other trailers - is passed
through unchanged apart from tidying blank lines the removal leaves behind
at the end of the message.

``--check-range`` exits 1 (and prints the offending commits) when any
commit in the range still carries Arena attribution, 0 otherwise. The
GitHub workflow uses it both to decide whether a rewrite is needed at all
and to verify the rewrite worked.
"""

from __future__ import annotations

import re
import subprocess
import sys

# An attribution line: a trailer-ish key whose value mentions Arena.
# The value side is matched loosely (`arena-agent`, `Arena AI`,
# `arena-ai-coding-agent[bot]`, the noreply addresses all contain "arena"),
# the key side is strict so body text merely mentioning Arena survives.
ATTRIBUTION_RE = re.compile(
    r"^\s*(co-authored-by|co-author|generated-by|authored-by)\s*:\s*.*arena",
    re.IGNORECASE,
)

# Arena identities as they appear in author/committer headers, used by the
# workflow's env-filter (kept here as the single place to update them).
ARENA_IDENTITY_MARKERS = ("arena-agent", "arena-ai", "arena.ai")


def clean_message(message: str) -> str:
    """Return `message` with Arena attribution lines removed."""
    kept = [line for line in message.splitlines() if not ATTRIBUTION_RE.match(line)]
    # Removing a final trailer leaves the blank separator line that preceded
    # it dangling at the end; drop trailing blank lines, then end the
    # message with exactly one newline (git's own `--cleanup` would do the
    # same, filter-branch does not).
    while kept and not kept[-1].strip():
        kept.pop()
    # A removal in the middle (attribution followed by other trailers) can
    # leave a doubled blank line; collapse 2+ blank lines to one.
    out: list[str] = []
    for line in kept:
        if not line.strip() and out and not out[-1].strip():
            continue
        out.append(line)
    if not out:
        return ""
    return "\n".join(out) + "\n"


def commits_with_attribution(range_spec: str) -> list[str]:
    """Short descriptions of commits in `range_spec` carrying attribution."""
    log = subprocess.run(
        ["git", "log", "--format=%H%x00%B%x1e", range_spec],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    offenders: list[str] = []
    for record in log.split("\x1e"):
        record = record.strip("\n")
        if not record.strip():
            continue
        sha, _, body = record.partition("\x00")
        if any(ATTRIBUTION_RE.match(line) for line in body.splitlines()):
            subject = next((ln for ln in body.splitlines() if ln.strip()), "")
            offenders.append(f"{sha[:12]} {subject.strip()}")
    return offenders


def main(argv: list[str]) -> int:
    if not argv:  # msg-filter mode
        sys.stdout.write(clean_message(sys.stdin.read()))
        return 0

    if argv[0] == "--check-range" and len(argv) == 2:
        offenders = commits_with_attribution(argv[1])
        for line in offenders:
            print(line)
        return 1 if offenders else 0

    if argv[0] == "--check-message-file" and len(argv) == 2:
        with open(argv[1], encoding="utf-8") as handle:
            text = handle.read()
        return 1 if any(ATTRIBUTION_RE.match(ln) for ln in text.splitlines()) else 0

    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
