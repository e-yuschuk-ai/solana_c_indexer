# solana_c_indexer

A Solana blockchain indexer written in C11.

The project is built incrementally; see [ROADMAP.md](ROADMAP.md) for the plan
and the current state, and [docs/decisions.md](docs/decisions.md) for the
choices that shape it.

Complete so far: the project foundation (M1), the core data structures (M2),
and most of the transport (M3) — a WebSocket client that follows the chain tip
through `blockSubscribe`, and an HTTP JSON-RPC client for everything the socket
cannot replay. The ingestion pipeline that joins them is M4 and does not exist
yet, so the `indexer` binary still only loads its configuration and exits; the
transports are exercised through the tools described below.

## Requirements

- A C11 compiler (GCC 9+ or Clang 10+)
- GNU Make
- POSIX threads
- For debug builds: AddressSanitizer and UndefinedBehaviorSanitizer support

libcurl is required for both the JSON-RPC client and the WebSocket
subscriptions, and it must have `ws`/`wss` compiled in (check with
`curl --version`):

```sh
sudo apt install libcurl4-openssl-dev     # Debian/Ubuntu
```

The JSON parser (yyjson) is vendored in `vendor/`, so it needs no installation.
See [docs/decisions.md](docs/decisions.md) for why each was chosen.

## Building

Use `build.sh`. It checks the toolchain, picks a job count, falls back when the
sanitizer runtimes are missing, and keeps the output to one line per file:

```sh
./build.sh            # debug build with sanitizers -> build/debug/indexer
./build.sh release    # optimized build             -> build/release/indexer
./build.sh test       # build everything and run the unit tests
./build.sh clean      # remove build/
./build.sh rebuild    # clean, then build debug
```

Overrides, rarely needed:

| Variable | Values | Purpose |
| --- | --- | --- |
| `CC` | e.g. `clang` | Override the compiler |
| `JOBS` | e.g. `1` | Override the parallel job count |
| `SANITIZE` | `1`, `0` | Force the sanitizers on or off |
| `VERBOSE` | `1` | Print the full compiler command lines |

The Makefile underneath can still be driven directly (`make BUILD=release`,
`make test`, `make V=1`) — `build.sh` only wraps it.

Everything is compiled with `-Wall -Wextra -Werror -pedantic -Wshadow
-Wconversion`, so warnings break the build.

## Running

Use `run.sh`. It loads `.env`, rebuilds if needed, and execs the binary:

```sh
cp .env.example .env     # then fill in your endpoint
./run.sh
./run.sh --log-level trace --start-slot 250000000
```

| Variable | Values | Purpose |
| --- | --- | --- |
| `BUILD` | `debug` (default), `release` | Which binary to build and run |
| `SKIP_BUILD` | `0` (default), `1` | Run the existing binary without rebuilding |
| `ENV_FILE` | path | Load a file other than `.env` |

Any flags given to `run.sh` are passed straight to the indexer. The binary can
also be invoked directly:

```sh
export SOLANA_RPC_URL="https://your-endpoint.example/"
./build/debug/indexer --log-level debug
```

Run `./run.sh --help` for the full list of options.

### Configuration

Settings are resolved from four sources, each overriding the previous one:

1. built-in defaults
2. a configuration file — `--config PATH`, or `./indexer.conf` when it exists
3. environment variables
4. command line flags

| Setting | Environment | Flag | Default |
| --- | --- | --- | --- |
| `rpc_url` | `SOLANA_RPC_URL` | `--rpc-url` | *(required)* |
| `wss_url` | `SOLANA_WSS_URL` | `--wss-url` | *(unset)* |
| `log_level` | `INDEXER_LOG_LEVEL` | `--log-level` | `info` |
| `start_slot` | `INDEXER_START_SLOT` | `--start-slot` | `0` (chain tip) |
| `end_slot` | `INDEXER_END_SLOT` | `--end-slot` | `0` (follow) |
| `concurrency` | `INDEXER_CONCURRENCY` | `--concurrency` | `4` |
| `commitment` | `INDEXER_COMMITMENT` | `--commitment` | `confirmed` |
| `tx_details` | `INDEXER_TX_DETAILS` | `--tx-details` | `full` |
| `block_filter` | `INDEXER_BLOCK_FILTER` | `--block-filter` | `all` |
| `blocks_range_limit` | `INDEXER_BLOCKS_RANGE_LIMIT` | `--blocks-range-limit` | plan-dependent |
| `state_file` | `INDEXER_STATE_FILE` | `--state-file` | *(disabled)* |

Two starting points are provided: [`.env.example`](.env.example) for the
environment variables `run.sh` loads, and
[`indexer.conf.example`](indexer.conf.example) for the file form. Both `.env`
and `indexer.conf` are gitignored because endpoint URLs often embed an API
token in the path.

### Docker

`Dockerfile` and `docker-compose.yml` provide the toolchain (GNU Make, a C11
compiler, the ASan/UBSan runtimes, and a libcurl with WebSocket support) for
running `build.sh` and `run.sh` without installing anything locally. The
image carries only the toolchain; the repository is bind-mounted at
`/app`, so edits and build output are shared with the host:

```sh
docker compose build                              # once, or after Dockerfile changes
docker compose run --rm indexer ./build.sh test    # any build.sh command
docker compose run --rm indexer ./run.sh            # or just: docker compose up
```

`run.sh` loads `.env` exactly as it does outside Docker, so `cp .env.example
.env` first. On a Linux host where your UID/GID are not 1000, rebuild with
`docker compose build --build-arg UID=$(id -u) --build-arg GID=$(id -g)` so
files written into the bind-mounted repo are not owned by root.

This image is a development convenience, distinct from the production
deployment Dockerfile tracked as a M8 roadmap item.

## Layout

```
include/    public headers, one per module
src/        implementation; main.c is the entry point
tests/      unit tests, one binary per tests/test_*.c
docs/       design notes and operational documentation
```

| Module | Purpose |
| --- | --- |
| `error` | `idx_status` codes plus an `idx_error` context carrying message, file and line |
| `log` | Leveled, mutex-serialized logging to a single sink |
| `arena` | Chunked bump allocator for per-block and per-transaction scratch memory |
| `config` | Layered configuration loading and validation |
| `bytes` | `idx_slice` views, a bounds-checked read cursor, a growable buffer and hex |
| `base58` | Bitcoin-alphabet base58, as used for pubkeys and signatures |
| `base64` | RFC 4648 base64, as returned by the RPC for account and transaction data |
| `types` | `idx_pubkey`, `idx_signature`, `idx_hash` and well-known program ids |
| `vec` | Growable array of fixed-size elements |
| `map` | Open-addressing hash map with byte-string keys |
| `json` | Reading and minimal writing, wrapping the vendored yyjson |
| `ws` | WebSocket transport over libcurl, reassembling fragmented messages |
| `pubsub` | Subscription registry, notification demux, reconnect and resubscribe |
| `rpc` | HTTP JSON-RPC client with gzip, retries, failover and batching |

Programs under `tools/` talk to a live endpoint and are built with
`./build.sh tools`, never by `make test`:

| Tool | Purpose |
| --- | --- |
| `wsdump` | Subscribe and report what arrives; saves payloads as fixtures |
| `subscribe` | Follow the stream through the PubSub layer, reporting slot continuity |
| `rpcprobe` | Exercise every RPC method, including a skipped slot and a batch |

See [docs/conventions.md](docs/conventions.md) for the coding conventions these
modules establish.

## Testing

`./build.sh test` builds one binary per `tests/test_*.c` and runs them all under
the sanitizers. A non-zero exit from any binary fails the run.

## License

See [LICENSE](LICENSE).
