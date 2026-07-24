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
- [x] Transaction metadata: status, fee, pre/post balances, token balances, logs
- [x] Built-in program instruction decoders: System and SPL Token — two
      formats, not one: the System program is bincode with a `u32`
      discriminant, SPL Token a packed byte layout with a `u8` one. Both
      decoders resolve their named accounts against the transaction's
      resolved list, and an unknown discriminant reports not-found rather
      than a parse failure, so a program upgrade is skipped and not fatal
- [x] SPL Token-2022: base instruction set and extension discriminants —
      per-extension payloads are decoded when a consumer needs them, not
      up front. The base set is the one above, which the same decoder already
      reads for either program, so what was left is discriminant 25 and up:
      six instructions decoded in full and fourteen extension groups
      identified by group and sub-discriminant, payload untouched. The
      metadata and group interfaces are dispatched by an eight-byte
      discriminator rather than by these, so they belong to the M6 item that
      builds the token dimension from them

## M6 — Domain decoding

Turns decoded instructions into the entities the storage tiers hold. Decision
D5 fixes the scope: the indexer feeds a trading terminal, so it derives
balances, transfers, swaps and bars, and it derives them only from what the
block stream carried — nothing is fetched from a node to complete a record.

- [x] Vote transaction filter: recognised and dropped before any extraction —
      a transaction counts as a vote only when every top-level instruction
      invokes the Vote program. Missing one costs storage, mistaking anything
      else for one loses an event the block no longer holds, so the rule errs
      in the direction that is recoverable
- [x] SOL balance state per account, from `meta.pre/postBalances` — only the
      accounts that moved are emitted. The ones that never move are the ones
      in every transaction, so writing them would rewrite the hottest keys in
      the system to say nothing happened, and an account with no movement has
      nothing for the terminal to show
- [x] Token balance state per token account, from `meta.pre/postTokenBalances`,
      carrying mint, owner and decimals — the two lists are sparse and
      independent, so an observation is their join on the token account and its
      mint: an account on one side only is a creation or a close, not a
      mismatch. Only what moved is emitted, as for SOL, and the movement is a
      pair of amounts rather than a signed delta because a raw token amount is
      bounded by uint64 and not by any supply
- [x] Transfer extraction from System and SPL Token instructions — walked over
      the inner instructions too, which is where most token movement is: a
      venue's program transfers on the trader's behalf. Mints and burns are
      transfers to and from the mint itself rather than a shape of their own,
      so a balance never grows with no event to explain it. Token-2022's
      `TransferCheckedWithFee` is the first extension payload M5 left as bytes
      to be decoded, since a mint that charges a fee moves its tokens through
      it and not through `TransferChecked`. What the
      instruction does not name — the mint of an unchecked `Transfer`, its
      scale, the wallets behind the token accounts — is resolved against
      `meta`'s token balances, and the owners are resolved now rather than
      joined later, because a token account's owner can change and the state
      tier only holds the latest. Failed transactions yield nothing: their
      instructions rolled back. `CloseAccount` is the one movement left out —
      its amount is in `meta` rather than in the instruction, and D5 assigns
      balance-delta reasoning to the swap path
- [~] Per-DEX swap decoders, one module per venue — pump.fun (the bonding
      curve and PumpSwap), Raydium (AMM v4 and CLMM) and Jupiter v6. Where a
      program emits an Anchor CPI event the event is what is decoded, not the
      instruction: it states what was traded rather than what was requested,
      and it survives the account-order changes an upgrade brings — pump's
      curve already ships two layouts at once. Where there is no event
      (Raydium AMM v4) the decoder names the pool and its vaults and leaves
      the amounts to the balance deltas, which is the item below. Jupiter is
      decoded but is not a pool: decision D8. Still open: Raydium CPMM, whose
      account layout no block observed so far contains, and which is not going
      in on a layout that was not verified against real data
- [ ] Swap normalization: mints and amounts resolved against the balance deltas
      of the pool's accounts, attributed per invocation so a multi-hop route
      yields one row per pool
- [ ] Price on a swap whose counter-side is a quote mint (SOL/WSOL, USDT, USDC,
      USD1), with the quote set configurable
- [ ] Pool registry: structure learned from the first observed swap, enriched
      by a creation instruction when one is seen
- [ ] Token registry: address and decimals from balances, name, symbol and
      metadata URI from metadata instructions when observed
- [ ] Bar derivation per pool at 1s and 1m, keyed `(pool, bucket)`
- [ ] Bar recomputation for a slot range, used by the reorg path (D4)
- [ ] Golden-file tests: real blocks in, expected entities out

## M7 — Storage

Two tiers per decision D4: PostgreSQL for `confirmed`, ClickHouse for
`finalized`. Sized for the ~2600 transactions/s that D1a commits to, less
whatever the vote filter removes. The entities are the ones D5 names.

- [ ] Storage abstraction layer, one interface per tier
- [ ] PostgreSQL client over libpq: connection handling, prepared statements
- [ ] Confirmed schema, indexed by slot: balances, transfers, swaps, bars,
      plus the pool and token dimensions
- [ ] Balance state written as an upsert keyed on the account, so the tier
      holds a current value per account rather than an observation log
- [ ] Reorg path in one transaction: delete at or above the reorged slot,
      rewrite, and recompute the affected bars
- [ ] Retention: drop confirmed rows once promoted and past a safety margin
- [ ] ClickHouse HTTP client: query, insert, error and exception-code handling
- [ ] `RowBinary` serialization for the insert path (`JSONEachRow` for
      development and debugging)
- [ ] Finalized schema: denormalized, event tables ordered by
      `(slot, transaction_index)`, partitioned by slot range, with column
      codecs where they pay off
- [ ] Balance state as `ReplacingMergeTree` keyed on the account with the slot
      as version, so the latest observation wins without a delete
- [ ] Batching writer: accumulate rows, flush on row count or time bound,
      never one insert per block (`TOO_MANY_PARTS`)
- [ ] Re-indexing a slot is safe: `ReplacingMergeTree` keyed on the sort key
      with a version column
- [ ] Bar rollup: a pool with no swaps for a configured window is abandoned;
      its `1s` and `1m` bars collapse into `1d` and the fine-grained rows are
      dropped. `bars_1s` is the largest table in the design and this is what
      bounds it
- [ ] Promotion path: on finalization, bulk read from PostgreSQL and batch
      insert into ClickHouse without refetching the block
- [ ] Schema migrations for both tiers
- [ ] Backpressure from the writers to the ingestion queue

## M8 — Observability and operations

- [~] Metrics: slots/sec, transactions/sec, lag behind chain tip, error counts
      — a progress line every 5 s carries the rates and the lag while the run is
      live; error counts are still only in the summary at exit, and nothing is
      exported in a form a machine can scrape
- [ ] WebSocket health: reconnect count, time since last notification
- [ ] Promotion lag: slots sitting in the confirmed tier awaiting finalization
- [ ] Reorg counter and depth histogram
- [ ] Health and readiness endpoints
- [ ] Structured log output
- [ ] Dockerfile and example deployment configuration
- [ ] Runbook in `docs/` for common failure modes

## M9 — Query interface

The API serves the terminal D5 describes, not a general ledger. Nothing stores
transactions as such, so every read is anchored on a wallet, a token or a
pool — there is no query by signature.

- [ ] Read API over indexed data (HTTP + JSON)
- [ ] Wallet: SOL and token balances, transfers in and out
- [ ] Token: the pools that trade it, and the latest price in each
- [ ] Pool: swap history, and bars for an interval and slot range — `1s` and
      `1m` are stored, coarser intervals are built from them on read
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
