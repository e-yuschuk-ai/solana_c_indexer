# Coding conventions

These are the conventions established by milestone M1. Every later milestone
follows them; changing one means changing the code that already depends on it,
so revisit them deliberately rather than by accident.

## Naming

- Everything public is prefixed `idx_` (`idx_arena`, `idx_config_load`).
- Types are `snake_case` and used without `struct`/`enum` via a typedef.
- Enum members are `IDX_UPPER_SNAKE` (`IDX_ERR_PARSE`, `IDX_LOG_DEBUG`).
- Macros are `IDX_UPPER_SNAKE` (`IDX_FAIL`, `IDX_TRY`).
- File-scope statics use a `g_` prefix; nothing else is global.
- One module per header/source pair, named after the module.

## Errors

Fallible functions return `idx_status` and take `idx_error *err` last:

```c
idx_status idx_thing_load(idx_thing *thing, const char *path, idx_error *err);
```

Rules:

- `err` may always be NULL. Callers that only need success or failure pass NULL.
- Output parameters are only valid when the function returns `IDX_OK`.
- Report failures with `IDX_FAIL(err, status, fmt, ...)`, which records the
  message, file and line and returns `status`.
- Propagate with `IDX_TRY(expr)`; add context only where it helps the reader.
- A partially applied change is not left behind on failure — validate first,
  then mutate (see `set_string` in `src/config.c`).

Messages are lowercase, name the offending value, and do not end with a period.

## Memory

- Long-lived, singular objects use plain `malloc`/`free`.
- Short-lived object graphs — anything scoped to one block or one transaction —
  are allocated from an `idx_arena` and released with a single
  `idx_arena_reset()`. Do not free individual arena allocations.
- An arena belongs to exactly one thread. Concurrent pipeline stages each get
  their own.
- Fixed-size buffers are preferred over heap strings for configuration-like
  data, so structs can be copied without ownership rules.

## Logging

- Use the `IDX_ERROR`/`IDX_WARN`/`IDX_INFO`/`IDX_DEBUG`/`IDX_TRACE` macros; they
  skip argument evaluation when the level is disabled, so expensive formatting
  in a `IDX_TRACE` call costs nothing in production.
- Levels mean: `error` needs attention, `warn` is degraded but working, `info`
  is lifecycle, `debug` is per-unit-of-work, `trace` is per-item.
- Never log inside a tight per-item loop above `trace`.
- Logging is not an error-reporting channel. Return an `idx_status` and let the
  caller decide whether it is worth a line.

## Style

- Four-space indent, 80-column limit, braces on the same line.
- The build runs `-Wall -Wextra -Werror -pedantic -Wshadow -Wconversion`;
  silence a warning by fixing the cause, not by casting it away.
- Comments explain why, not what. A comment that restates the code is noise.
- Code and documentation are written in English (see `AGENTS.md`).

## Tests

- One `tests/test_<module>.c` per module, each a standalone binary.
- Use the macros in `tests/test.h`; a test file ends with `TEST_MAIN({...})`
  listing its `TEST_RUN(...)` calls.
- Test the error paths, not only the happy path. Every `idx_status` a function
  can return should appear in a test.
- Tests run under ASan and UBSan by default; keep them free of leaks.
