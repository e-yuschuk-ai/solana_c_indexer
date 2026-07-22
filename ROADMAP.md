# Roadmap

Development plan for the Solana indexer. Work proceeds top to bottom: each
milestone assumes the previous ones are done. Mark items `[x]` when completed,
in the same change that implements them.

Status legend: `[ ]` pending · `[~]` in progress · `[x]` done

---

## M1 — Project foundation

- [x] `Makefile` with `all`, `clean`, `test`, `debug` and `release` targets
- [x] Directory layout: `src/`, `include/`, `tests/`, `docs/`
- [x] Compiler flags: `-std=c11 -Wall -Wextra -Werror`, sanitizers in debug builds
- [x] Logging module with levels (error, warn, info, debug, trace)
- [x] Error handling convention (return codes + error context struct)
- [x] Arena/pool allocator for per-block and per-transaction scratch memory
- [x] Configuration loading (CLI flags + config file + environment variables)
- [x] `build.sh` and `run.sh` entry points wrapping the Makefile
- [x] `README.md` with build and run instructions

Conventions established here are documented in `docs/conventions.md`.

## M2 — Core data structures

- [ ] Fixed-width integer and byte-buffer helpers (`slice`, `buffer`)
- [ ] Base58 encode/decode
- [ ] Base64 encode/decode
- [ ] Pubkey, signature and hash types (32/64-byte wrappers)
- [ ] Dynamic array and hash map primitives
- [ ] Unit tests for every encoder/decoder against known vectors

## M3 — RPC client

- [ ] HTTP client (libcurl or a minimal socket-based client — decide and document)
- [ ] JSON parser/serializer (vendored or hand-written — decide and document)
- [ ] JSON-RPC 2.0 request/response envelope
- [ ] Methods: `getSlot`, `getBlock`, `getBlockHeight`, `getBlocks`,
      `getTransaction`, `getHealth`
- [ ] Retry with exponential backoff, timeouts and rate-limit handling
- [ ] Multiple endpoint support with failover

## M4 — Ingestion pipeline

- [ ] Slot cursor: track last indexed slot, resume after restart
- [ ] Block fetch loop with configurable concurrency
- [ ] Handle skipped slots and missing blocks
- [ ] Backfill mode (historical range) and follow mode (chain tip)
- [ ] Bounded queue between fetchers and processors
- [ ] Graceful shutdown on `SIGINT`/`SIGTERM`

## M5 — Decoding

- [ ] Block header decoding (slot, blockhash, parent, block time)
- [ ] Transaction decoding: signatures, message header, account keys
- [ ] Legacy and versioned (v0) message support
- [ ] Address lookup table resolution
- [ ] Instruction and inner-instruction decoding
- [ ] Transaction metadata: status, fee, pre/post balances, token balances, logs
- [ ] Built-in program instruction decoders (System, SPL Token, SPL Token-2022)

## M6 — Storage

- [ ] Storage abstraction layer (backend behind an interface)
- [ ] First backend implementation (PostgreSQL via libpq — decide and document)
- [ ] Schema: slots, blocks, transactions, instructions, accounts, token balances
- [ ] Batched writes inside transactions
- [ ] Idempotent upserts so re-indexing a slot is safe
- [ ] Schema migrations
- [ ] Reorg handling: detect and roll back orphaned slots

## M7 — Observability and operations

- [ ] Metrics: slots/sec, transactions/sec, lag behind chain tip, error counts
- [ ] Health and readiness endpoints
- [ ] Structured log output
- [ ] Dockerfile and example deployment configuration
- [ ] Runbook in `docs/` for common failure modes

## M8 — Query interface

- [ ] Read API over indexed data (HTTP + JSON)
- [ ] Queries: transaction by signature, transactions by account, block by slot
- [ ] Pagination and result limits
- [ ] API documentation in `docs/`

---

## Backlog (not scheduled)

- [ ] Geyser plugin ingestion instead of polling RPC
- [ ] WebSocket subscriptions for lower latency at the tip
- [ ] Account state indexing (not just transactions)
- [ ] Pluggable custom program decoders
- [ ] Alternative storage backends (SQLite, ClickHouse)
- [ ] Parallel backfill sharded across processes
- [ ] Fuzzing harness for the decoders
