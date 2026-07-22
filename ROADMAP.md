# Roadmap

Development plan for the Solana indexer. Work proceeds top to bottom: each
milestone assumes the previous ones are done. Mark items `[x]` when completed,
in the same change that implements them.

Status legend: `[ ]` pending · `[~]` in progress · `[x]` done

Design decisions that shape more than one milestone are recorded in
[docs/decisions.md](docs/decisions.md).

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

- [x] Fixed-width integer and byte-buffer helpers (`slice`, `cursor`, `buffer`)
- [x] Base58 encode/decode
- [x] Base64 encode/decode
- [x] Pubkey, signature and hash types (32/64-byte wrappers)
- [x] Dynamic array and hash map primitives
- [x] Unit tests for every encoder/decoder against known vectors

## M3 — Transport

Both transports are built on libcurl (decision D1). `blockSubscribe` over the
WebSocket is the hot path (decision D1a); HTTP is the recovery path for
everything the socket cannot replay.

- [x] Vendor the JSON parser (decision D2) and wrap it behind `idx_json`
- [x] libcurl initialization, TLS verification and shared connection setup
- [x] WebSocket connection over `wss://` with TLS verification
- [x] Fragment reassembly into a growable buffer with a high-water mark,
      sized for the measured 9–11 MiB notifications
- [x] JSON-RPC subscription envelope: subscribe, confirm id, notification demux
- [~] `blockSubscribe` works; `blockUnsubscribe` and wiring the filter and
      `transactionDetails` level to the configuration are still pending
- [x] Keepalive (ping/pong) and idle detection
- [x] Reconnect with exponential backoff and automatic resubscribe
- [ ] Record the last slot seen before a disconnect so the gap can be replayed
- [ ] HTTP JSON-RPC client: request/response envelope, batching, request ids
- [ ] Methods: `getSlot`, `getBlock`, `getBlocks`, `getBlockHeight`,
      `getTransaction`, `getHealth`, `getVersion`
- [ ] Retry with exponential backoff, timeouts and rate-limit (429) handling
- [ ] Multiple endpoint support with failover
- [ ] Fall back to `slotSubscribe` + `getBlock` when `blockSubscribe` is
      unavailable on the configured endpoint

## M4 — Ingestion pipeline

Real-time by default: blocks arrive on the socket, and the RPC client recovers
whatever the socket missed. The socket delivers ~12 MiB/s, so backpressure
handling is a requirement rather than a refinement.

- [ ] Slot cursor: track last indexed slot, resume after restart
- [ ] Follow mode driven by `blockSubscribe` notifications
- [ ] Bounded queue between the receive loop and the decoders, so a slow
      consumer never stalls the socket read
- [ ] Overflow policy: abandon the socket backlog and record the affected slot
      range as a gap rather than letting the provider drop the connection
- [ ] Gap detection: any distance between the cursor and a notified slot is a
      hole, whatever caused it
- [ ] Gap and backfill fetching over HTTP with configurable concurrency
- [ ] Backfill mode for historical ranges, sharing the gap fetch path
- [ ] Handle skipped slots and blocks the endpoint no longer retains
- [ ] Out-of-order arrival: commit in slot order, buffer what arrives early
- [ ] Graceful shutdown on `SIGINT`/`SIGTERM`, draining in-flight work

## M5 — Decoding

- [ ] Block header decoding (slot, blockhash, parent, block time)
- [ ] Transaction decoding: signatures, message header, account keys
- [ ] Legacy and versioned (v0) message support
- [ ] Address lookup table resolution
- [ ] Instruction and inner-instruction decoding
- [ ] Transaction metadata: status, fee, pre/post balances, token balances, logs
- [ ] Built-in program instruction decoders (System, SPL Token, SPL Token-2022)

## M6 — Domain decoding

Turns decoded instructions into the entities the storage tiers hold. Scope
depends on decision D5, still open.

- [ ] Transfer extraction from SPL Token and System instructions
- [ ] Mint and burn extraction
- [ ] Per-DEX swap decoders, one module per venue
- [ ] Normalized trade record: venue, market, side, amounts, price, payer
- [ ] Bar derivation from trades: OHLCV per market and interval
- [ ] Bar recomputation for a slot range, used by the reorg path (D4)
- [ ] Golden-file tests: real blocks in, expected entities out

## M7 — Storage

Two tiers per decision D4: PostgreSQL for `confirmed`, ClickHouse for
`finalized`. Sized for the ~2600 transactions/s that D1a commits to.

- [ ] Storage abstraction layer, one interface per tier
- [ ] PostgreSQL client over libpq: connection handling, prepared statements
- [ ] Confirmed schema: trades, mints, transfers, bars, indexed by slot
- [ ] Reorg path in one transaction: delete at or above the reorged slot,
      rewrite, and recompute the affected bars
- [ ] Retention: drop confirmed rows once promoted and past a safety margin
- [ ] ClickHouse HTTP client: query, insert, error and exception-code handling
- [ ] `RowBinary` serialization for the insert path (`JSONEachRow` for
      development and debugging)
- [ ] Finalized schema: denormalized, ordered by `(slot, transaction_index)`,
      partitioned by slot range, with column codecs where they pay off
- [ ] Batching writer: accumulate rows, flush on row count or time bound,
      never one insert per block (`TOO_MANY_PARTS`)
- [ ] Re-indexing a slot is safe: `ReplacingMergeTree` keyed on the sort key
      with a version column
- [ ] Promotion path: on finalization, bulk read from PostgreSQL and batch
      insert into ClickHouse without refetching the block
- [ ] Schema migrations for both tiers
- [ ] Backpressure from the writers to the ingestion queue

## M8 — Observability and operations

- [ ] Metrics: slots/sec, transactions/sec, lag behind chain tip, error counts
- [ ] WebSocket health: reconnect count, time since last notification
- [ ] Promotion lag: slots sitting in the confirmed tier awaiting finalization
- [ ] Reorg counter and depth histogram
- [ ] Health and readiness endpoints
- [ ] Structured log output
- [ ] Dockerfile and example deployment configuration
- [ ] Runbook in `docs/` for common failure modes

## M9 — Query interface

- [ ] Read API over indexed data (HTTP + JSON)
- [ ] Queries: transaction by signature, transactions by account, block by slot
- [ ] Pagination and result limits
- [ ] API documentation in `docs/`

---

## Backlog (not scheduled)

- [ ] Geyser plugin ingestion instead of RPC
- [ ] Account state indexing (not just transactions)
- [ ] Pluggable custom program decoders
- [ ] Alternative storage backends behind the M7 interface
- [ ] Parallel backfill sharded across processes
- [ ] Fuzzing harness for the decoders
