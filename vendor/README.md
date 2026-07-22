# Vendored dependencies

Third-party sources committed into the tree. They are compiled with warnings
disabled (see `CFLAGS_VENDOR` in the Makefile): upstream code is not ours to
fix, and `-Werror` on it would break the build on every upgrade.

Do not edit these files. To update one, replace it wholesale from upstream at a
tagged release and record the new version here.

| Library | Version | License | Upstream |
| --- | --- | --- | --- |
| yyjson | 0.12.0 | MIT | https://github.com/ibireme/yyjson |

## yyjson

Chosen in decision D2. The socket delivers roughly 12 MiB/s of JSON, so the
parser sits on the critical path; yyjson is among the fastest available and is
a single translation unit with no dependencies of its own.

It is reached only through `include/json.h`. Nothing outside `src/json.c`
includes `yyjson.h`, so replacing it later does not touch call sites.

Files taken from `src/` upstream:

- `yyjson.h`
- `yyjson.c`
- `LICENSE`
