# Agent Instructions

## Project

This project is a **Solana indexer written in C**.

## Language

**All code and documentation must be written in English.** This includes:

- Source code: identifiers, function names, struct/type names, macros
- Comments (inline, block, and doc comments)
- Commit messages, PR titles and descriptions
- Markdown documents, README files, design notes
- Log messages, error strings, and CLI help text

Conversation with the user may happen in any language, but nothing written into
the repository should be in a language other than English.

## Commit trailers

Commits authored by an AI agent must include a `Reasoning-Effort: <value>`
trailer, alongside any `Co-Authored-By`/session trailers the tool already
adds, using the value the session actually ran with (e.g. `Reasoning-Effort:
40`), not a qualitative label. Do not retroactively amend older commits to
add this — only apply it going forward.

## Roadmap-driven work

`ROADMAP.md` is the source of truth for what gets built and in what order.

- Before starting any task, read `ROADMAP.md` and pick the next unfinished item.
- Do not invent scope. If a request is not covered by the roadmap, either map it
  to an existing item or propose adding it to `ROADMAP.md` first.
- When a task is completed, update its status in `ROADMAP.md` in the same change.
- If `ROADMAP.md` does not exist yet, creating it is the first task.
