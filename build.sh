#!/usr/bin/env bash
#
# One command to build the indexer. Wraps the Makefile so day-to-day work does
# not need to know about profiles, flags or job counts.
#
#   ./build.sh              debug build with sanitizers (default)
#   ./build.sh release      optimized build
#   ./build.sh test         build everything and run the unit tests
#   ./build.sh clean        remove all build output
#   ./build.sh rebuild      clean, then build debug
#
# Overrides (rarely needed):
#   CC=clang ./build.sh     use a different compiler
#   JOBS=1 ./build.sh       serial build
#   SANITIZE=0 ./build.sh   force the sanitizers off
#   VERBOSE=1 ./build.sh    print the full compiler command lines

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

readonly COMMAND="${1:-debug}"

# ANSI styling, suppressed when stderr is not a terminal.
if [[ -t 2 ]]; then
    readonly BOLD=$'\033[1m' DIM=$'\033[2m' RED=$'\033[31m' \
        GREEN=$'\033[32m' YELLOW=$'\033[33m' RESET=$'\033[0m'
else
    readonly BOLD="" DIM="" RED="" GREEN="" YELLOW="" RESET=""
fi

info() { echo "${BOLD}==>${RESET} $*" >&2; }
warn() { echo "${YELLOW}warning:${RESET} $*" >&2; }
fail() {
    echo "${RED}error:${RESET} $*" >&2
    exit 1
}

check_toolchain() {
    command -v make >/dev/null 2>&1 ||
        fail "make not found. Install it (Debian/Ubuntu: apt install make)."

    local compiler="${CC:-cc}"
    command -v "$compiler" >/dev/null 2>&1 ||
        fail "compiler '$compiler' not found (Debian/Ubuntu: apt install gcc)."
}

# Debug builds want ASan and UBSan, but a toolchain may lack the runtimes.
# Probe once and fall back rather than failing with a link error.
detect_sanitizers() {
    if [[ -n "${SANITIZE:-}" ]]; then
        return
    fi

    local probe
    probe="$(mktemp -d)"
    trap 'rm -rf "$probe"' RETURN

    echo 'int main(void) { return 0; }' >"$probe/probe.c"
    if "${CC:-cc}" -fsanitize=address,undefined -o "$probe/probe" \
        "$probe/probe.c" >/dev/null 2>&1; then
        SANITIZE=1
    else
        SANITIZE=0
        warn "sanitizer runtimes unavailable; building without them"
        warn "(Debian/Ubuntu: apt install libasan8 libubsan1)"
    fi
    export SANITIZE
}

job_count() {
    if [[ -n "${JOBS:-}" ]]; then
        echo "$JOBS"
    elif command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v getconf >/dev/null 2>&1; then
        getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1
    else
        echo 1
    fi
}

run_make() {
    local profile="$1"
    shift
    make --no-print-directory -j"$(job_count)" "BUILD=$profile" \
        "SANITIZE=${SANITIZE:-1}" "V=${VERBOSE:-0}" "$@" >&2
}

report() {
    local profile="$1"
    local binary="build/$profile/indexer"

    [[ -x "$binary" ]] || fail "expected $binary to exist after the build"

    local size
    size="$(du -h "$binary" | cut -f1)"
    local sanitizers="off"
    [[ "$profile" == "debug" && "${SANITIZE:-1}" == "1" ]] && sanitizers="on"

    info "${GREEN}built${RESET} $binary ${DIM}($profile, ${size}," \
        "sanitizers ${sanitizers})${RESET}"
    # run.sh sets this: it is about to run the binary itself.
    [[ "${NO_RUN_HINT:-0}" == "1" ]] || echo "    run it with: ./run.sh" >&2
}

check_toolchain

case "$COMMAND" in
    debug)
        detect_sanitizers
        info "building debug"
        run_make debug
        report debug
        ;;
    release)
        info "building release"
        run_make release
        report release
        ;;
    test)
        detect_sanitizers
        info "building and running tests"
        run_make debug test
        info "${GREEN}tests passed${RESET}"
        ;;
    clean)
        info "removing build output"
        run_make debug clean
        ;;
    rebuild)
        detect_sanitizers
        info "removing build output"
        run_make debug clean
        info "building debug"
        run_make debug
        report debug
        ;;
    -h | --help | help)
        sed -n '3,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
        ;;
    *)
        fail "unknown command '$COMMAND'. Try: debug, release, test, clean, rebuild"
        ;;
esac
