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
- [x] `blockSubscribe` / `blockUnsubscribe`, with the filter and
      `transactionDetails` level taken from the configuration
- [x] Keepalive (ping/pong) and idle detection
- [x] Reconnect with exponential backoff and automatic resubscribe
- [x] HTTP JSON-RPC client: request/response envelope, batching, request ids
- [x] Methods: `getSlot`, `getBlock`, `getBlocks`, `getBlockHeight`,
      `getTransaction`, `getHealth`, `getVersion`
- [x] Request gzip: a block is 12.2 MiB of JSON but 0.93 MiB compressed
- [x] Treat a skipped slot as not-found rather than an error, and split
      `getBlocks` to whatever range the provider's plan allows
- [x] Retry with exponential backoff, timeouts and rate-limit (429) handling
- [x] Multiple endpoint support with failover
- [x] Fall back to `slotSubscribe` + `getBlock` when `blockSubscribe` is
      unavailable on the configured endpoint

## M4 — Ingestion pipeline

Real-time by default: blocks arrive on the socket, and the RPC client recovers
whatever the socket missed. The socket delivers ~12 MiB/s, so backpressure
handling is a requirement rather than a refinement.

- [x] Slot cursor: track last indexed slot, resume after restart, and record
      the last slot seen before a disconnect so the gap can be replayed
- [x] Follow mode driven by `blockSubscribe` notifications, falling back to
      `slotSubscribe` + `getBlock` where the endpoint does not offer it
- [x] Bounded queue between the receive loop and the decoders, so a slow
      consumer never stalls the socket read (decision D6)
- [x] Overflow policy: abandon the socket backlog and record the affected slot
      range as a gap rather than letting the provider drop the connection
      (decision D6) — the ring drops its oldest entry and the sequence gap
      makes the loss visible; handing the range to the fetcher is the item
      below
- [x] Gap detection: any distance between the cursor and a notified slot is a
      hole, whatever caused it — a reconnect, a provider dropping a slow
      consumer, or the ring shedding under pressure all read the same
- [x] Gap and backfill fetching over HTTP with configurable concurrency —
      `idx_fetcher` claims ranges from `idx_gaps`, one connection per worker.
      Claims narrow themselves when a provider refuses the getBlocks width
- [x] Backfill mode for historical ranges, sharing the gap fetch path — not a
      mode: a resumed cursor puts its distance from the tip into the same gap
      set, and the same fetchers work it
- [x] Handle skipped slots and blocks the endpoint no longer retains — the
      getBlocks enumeration resolves what the chain never produced without a
      fetch each, and a not-found block resolves rather than retries
- [~] Out-of-order arrival: commit in slot order, buffer what arrives early —
      what is ordered is the durable cursor, not the handler. `idx_gaps`
      tracks a contiguous watermark, so a restart never resumes past a slot
      that was never indexed, while blocks commit in whatever order they
      arrive. Buffering blocks until a hole ahead of them fills would mean
      holding tens of megabytes each for as long as the gap lasts; the
      watermark buys the same guarantee for a few slot numbers. If M7 turns
      out to need arrival order too, this is where it goes
- [x] Graceful shutdown on `SIGINT`/`SIGTERM`, draining in-flight work — the
      receive loop stops at a block boundary and unsubscribes, the processing
      thread drains what is queued, and the cursor is persisted last

## M5 — Decoding

- [x] Block header decoding (slot, blockhash, parent, block time)
- [x] Transaction decoding: signatures, message header, account keys
- [x] Legacy and versioned (v0) message support
- [x] Address lookup table resolution
- [x] Instruction and inner-instruction decoding
- [ ] Transaction metadata: status, fee, pre/post balances, token balances, logs
      (status and fee done; balances, token balances and logs remain)
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
