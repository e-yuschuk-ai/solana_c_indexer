#!/usr/bin/env bash
#
# Builds (if needed) and runs the indexer.
#
#   ./run.sh                       debug build, settings from .env
#   ./run.sh --log-level trace     extra flags are passed through
#   BUILD=release ./run.sh         optimized build
#   SKIP_BUILD=1 ./run.sh          run the existing binary as-is
#
# Configuration is read from ./.env when present (gitignored), then from the
# environment, then from the flags given to this script. See README.md.

set -euo pipefail

BUILD="${BUILD:-debug}"
SKIP_BUILD="${SKIP_BUILD:-0}"

cd "$(dirname "${BASH_SOURCE[0]}")"

ENV_FILE="${ENV_FILE:-.env}"
if [[ -f "$ENV_FILE" ]]; then
    echo "run.sh: loading $ENV_FILE" >&2
    # Exporting every assignment so the indexer inherits them.
    set -a
    # shellcheck disable=SC1090
    source "$ENV_FILE"
    set +a
fi

if [[ -z "${SOLANA_RPC_URL:-}" ]] && [[ ! " $* " =~ " --rpc-url" ]] &&
    [[ ! -f indexer.conf ]]; then
    echo "run.sh: no RPC endpoint configured." >&2
    echo "  Set SOLANA_RPC_URL, create $ENV_FILE or indexer.conf," >&2
    echo "  or pass --rpc-url. See indexer.conf.example." >&2
    exit 1
fi

BIN="build/$BUILD/indexer"

if [[ "$SKIP_BUILD" != "1" ]]; then
    NO_RUN_HINT=1 ./build.sh "$BUILD"
fi

if [[ ! -x "$BIN" ]]; then
    echo "run.sh: $BIN not found; run make BUILD=$BUILD" >&2
    exit 1
fi

# exec so signals reach the indexer directly, which matters once M4 installs
# the SIGINT/SIGTERM handlers.
exec "./$BIN" "$@"
