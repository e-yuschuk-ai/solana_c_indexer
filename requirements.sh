#!/usr/bin/env bash
#
# Installs the system packages the indexer needs to build and run, on Debian
# and Debian derivatives.
#
#   ./requirements.sh           install the build requirements
#   ./requirements.sh docker    also install the container toolchain, for the
#                               docker-compose.yml dev environment
#   ./requirements.sh check     verify what is installed, install nothing
#
# Everything else the project depends on is vendored: the JSON parser lives in
# vendor/yyjson and needs no package. See docs/decisions.md for why each
# dependency was chosen.
#
# Not installed here, deliberately: libpq-dev and a ClickHouse client. Storage
# is milestone M7 and no code needs them yet (ROADMAP.md).

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

readonly COMMAND="${1:-install}"

# ANSI styling, suppressed when stderr is not a terminal.
if [[ -t 2 ]]; then
    readonly BOLD=$'\033[1m' DIM=$'\033[2m' RED=$'\033[31m' \
        GREEN=$'\033[32m' YELLOW=$'\033[33m' RESET=$'\033[0m'
else
    readonly BOLD="" DIM="" RED="" GREEN="" YELLOW="" RESET=""
fi

info() { echo "${BOLD}==>${RESET} $*" >&2; }
warn() { echo "${YELLOW}warning:${RESET} $*" >&2; }
ok() { echo "  ${GREEN}ok${RESET} $*" >&2; }
bad() { echo "  ${RED}missing${RESET} $*" >&2; }
fail() {
    echo "${RED}error:${RESET} $*" >&2
    exit 1
}

command -v apt-get >/dev/null 2>&1 ||
    fail "apt-get not found. This script is for Debian and its derivatives."

# Root already, or sudo. Nothing here works without one of the two.
SUDO=""
if [[ "$(id -u)" -ne 0 ]]; then
    command -v sudo >/dev/null 2>&1 ||
        fail "not root and sudo is not installed. Re-run this as root."
    SUDO="sudo"
fi
readonly SUDO

# Keep apt from prompting for configuration during the install.
export DEBIAN_FRONTEND=noninteractive

install_build_requirements() {
    info "refreshing the package lists"
    $SUDO apt-get update

    info "installing the build requirements"

    # GCC, GNU Make and the libc headers. Also covers POSIX threads: the
    # Makefile links -lpthread, which glibc provides.
    $SUDO apt-get install -y build-essential

    # AddressSanitizer runtime. Debug builds are compiled with
    # -fsanitize=address and the test suite runs under it. build.sh probes for
    # this and silently drops the sanitizers when it is absent, so a missing
    # package costs coverage rather than breaking the build.
    $SUDO apt-get install -y libasan8

    # UndefinedBehaviorSanitizer runtime, for the -fsanitize=undefined half of
    # the same debug build.
    $SUDO apt-get install -y libubsan1

    # curl/curl.h, curl-config and the link target. libcurl carries both
    # transports: HTTP for JSON-RPC and WebSocket for the PubSub subscriptions
    # (decision D1). This is the one package the build genuinely cannot do
    # without.
    $SUDO apt-get install -y libcurl4-openssl-dev

    # The trust store libcurl verifies wss:// and https:// endpoints against.
    # Without it every connection fails certificate verification.
    $SUDO apt-get install -y ca-certificates

    # The curl command line tool. Not needed to build, but it is how you check
    # that the installed libcurl has WebSocket support, and how you probe an
    # endpoint by hand.
    $SUDO apt-get install -y curl
}

install_docker() {
    info "installing the container toolchain"

    # Docker Engine, for the dev image in Dockerfile.
    $SUDO apt-get install -y docker.io

    # The compose v2 plugin, so `docker compose up` works against
    # docker-compose.yml. Packaged as docker-compose-v2 on trixie and later;
    # on older releases install compose from Docker's own repository instead.
    $SUDO apt-get install -y docker-compose-v2

    warn "add yourself to the docker group to run it without sudo:"
    warn "  sudo usermod -aG docker $USER   ${DIM}(log out and back in)${RESET}"
}

# The build fails in confusing ways when one of these is half-present, so check
# for the capability rather than for the package name.
check() {
    local failures=0

    info "checking the toolchain"

    if command -v make >/dev/null 2>&1; then
        ok "make $(make --version | head -1 | grep -o '[0-9.]*$')"
    else
        bad "make (apt-get install build-essential)"
        failures=$((failures + 1))
    fi

    local compiler="${CC:-cc}"
    if command -v "$compiler" >/dev/null 2>&1; then
        ok "$compiler $("$compiler" -dumpversion)"
    else
        bad "$compiler (apt-get install build-essential)"
        failures=$((failures + 1))
    fi

    if command -v curl-config >/dev/null 2>&1; then
        ok "libcurl $(curl-config --version | grep -o '[0-9.]*$')"

        # The one requirement a package name does not guarantee. WebSocket
        # support has to be compiled in, and the block subscription is useless
        # without it (decision D1a). Debian 13 ships curl 8.14 with wss;
        # older releases may not.
        if curl-config --protocols | grep -qw WSS; then
            ok "libcurl has wss"
        else
            bad "libcurl has no wss support: blockSubscribe cannot connect"
            failures=$((failures + 1))
        fi
    else
        bad "curl-config (apt-get install libcurl4-openssl-dev)"
        failures=$((failures + 1))
    fi

    # Mirrors what build.sh does before it decides to drop the sanitizers.
    local probe
    probe="$(mktemp -d)"
    echo 'int main(void) { return 0; }' >"$probe/probe.c"
    if "$compiler" -fsanitize=address,undefined -o "$probe/probe" \
        "$probe/probe.c" >/dev/null 2>&1; then
        ok "sanitizer runtimes"
    else
        # Not fatal: build.sh falls back to a build without them.
        warn "sanitizer runtimes unavailable; debug builds lose ASan and UBSan"
        warn "  (apt-get install libasan8 libubsan1)"
    fi
    rm -rf "$probe"

    if [[ "$failures" -ne 0 ]]; then
        fail "$failures requirement(s) missing; run ./requirements.sh"
    fi

    info "${GREEN}everything the build needs is present${RESET}"
    echo "    build it with: ./build.sh" >&2
}

case "$COMMAND" in
    install)
        install_build_requirements
        check
        ;;
    docker)
        install_build_requirements
        install_docker
        check
        ;;
    check)
        check
        ;;
    -h | --help | help)
        sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
        ;;
    *)
        fail "unknown command '$COMMAND'. Try: install, docker, check, help"
        ;;
esac
