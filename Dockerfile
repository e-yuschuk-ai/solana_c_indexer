# Dev/build image for solana_c_indexer.
#
# Carries the toolchain build.sh and run.sh expect: GNU Make, a C11
# compiler, the ASan/UBSan runtimes, and libcurl with WebSocket support
# (Ubuntu 24.04's libcurl4-openssl-dev has curl_ws_send/curl_ws_recv; older
# releases may not). The source is bind-mounted at runtime by
# docker-compose.yml, so this image holds only the toolchain, not a copy of
# the code.
FROM ubuntu:24.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        libcurl4-openssl-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Matches a typical Linux host UID/GID so files written into the bind-mounted
# repo (build/, .env edits) are not owned by root. Override at build time
# with --build-arg UID=$(id -u) --build-arg GID=$(id -g) if yours differ.
ARG UID=1000
ARG GID=1000
RUN (getent group "$GID" >/dev/null || groupadd -g "$GID" indexer) && \
    useradd -m -u "$UID" -g "$GID" -o indexer

USER indexer
WORKDIR /app

CMD ["./run.sh"]
