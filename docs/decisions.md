# Design decisions

Decisions that constrain more than one milestone. Each entry states what was
chosen, what it rules out, and what would justify revisiting it.

---

## D1 — libcurl for both HTTP and WebSocket

**Status:** accepted · **Affects:** M3, M4

Blocks are consumed in real time, so the indexer needs a `wss://` client, and
`wss://` means TLS. Hand-writing TLS is not reasonable, so the project takes a
third-party dependency here; the only question was which one.

**Decision.** libcurl provides both transports: HTTP for JSON-RPC and
WebSocket for the PubSub subscriptions. One dependency covers TLS, the HTTP
upgrade handshake, RFC 6455 framing, masking and ping/pong. The system curl is
8.14.1 with `ws` and `wss` in its protocol list, built against OpenSSL 3.5.6.

Rejected alternatives:

- *OpenSSL plus a hand-written RFC 6455 implementation.* Full control and no
  experimental API, but framing, masking, fragmentation and the close handshake
  are a large amount of subtle code for no functional gain.
- *libcurl for HTTP and a hand-written WebSocket.* Two dependencies and the
  same subtle code, to avoid one experimental API.

**Consequences.**

- `libcurl4-openssl-dev` becomes a build requirement.
- curl's WebSocket API is still marked experimental and is pull-based
  (`curl_ws_recv`), so the receive loop owns buffering and frame reassembly
  even though curl does the framing.
- Everything sits behind `idx_http` and `idx_ws`, so replacing libcurl later
  touches two modules rather than the pipeline.
- Ubuntu's packaged libcurl is not built with WebSocket support, unlike Debian
  trixie's. `requirements.sh` detects this and builds a fixed libcurl from
  the upstream tarball into `/usr/local` as a fallback.

**Revisit if** the WebSocket API proves unstable across curl releases, or if
latency measurements show the pull-based receive loop is a bottleneck.

---

## D1a — `blockSubscribe` as the primary tip source

**Status:** accepted · **Affects:** M3, M4, M5

Solana offers two ways to follow the tip over WebSocket:

- `slotSubscribe` notifies that a slot exists; the block is then fetched with
  `getBlock` over HTTP.
- `blockSubscribe` pushes the whole block over the socket.

`blockSubscribe` is an unstable RPC method — the validator must run with
`--rpc-pubsub-enable-block-subscription` — so support was verified against the
configured endpoint before committing to it.

**Decision.** Subscribe to `blockSubscribe` and consume blocks directly from
the socket, removing the `getBlock` round trip from the hot path.

### Measured behaviour

Five consecutive mainnet blocks, `commitment=confirmed`, `encoding=json`,
`transactionDetails=full`, `showRewards=false`:

| slot | payload | transactions | gap |
| --- | --- | --- | --- |
| 434543707 | 6.76 MiB | 1411 | — |
| 434543708 | 4.11 MiB | 1268 | 0.41 s |
| 434543709 | 4.69 MiB | 1313 | 0.37 s |
| 434543710 | 6.05 MiB | 1434 | 0.57 s |
| 434543711 | 7.39 MiB | 1490 | 0.65 s |

Average 5.80 MiB, peak 7.39 MiB, arriving every 0.4–0.65 s. That is roughly
**12 MiB/s of JSON to parse, sustained**, and about 2600 transactions/s.

### Consequences

- **The HTTP client is still required.** `blockSubscribe` does not replay what
  was missed: a reconnect leaves a hole that only `getBlock` can fill. M3 keeps
  both transports; the difference is that HTTP is now the recovery path rather
  than the hot path.
- **Frame reassembly is mandatory.** Notifications far exceed any sane single
  read, so the receive loop must reassemble fragments into a buffer sized in
  the tens of MiB, with a high-water mark rather than a fixed allocation.
- **The parser is on the critical path.** 12 MiB/s sustained rules out a
  parser that allocates a node per value; see D2.
- **Backpressure is a design constraint, not an afterthought.** If decoding
  falls behind, TCP backpressure builds until the provider drops the
  connection. The pipeline needs a bounded queue and must be able to abandon
  the socket's backlog and recover those slots over HTTP.
- **Bandwidth is significant.** ~12 MiB/s is close to 1 TiB/day. On a metered
  plan this is the dominant cost, and it is the main argument for narrowing the
  subscription (see below).

### Sizing, measured against a live endpoint

A later run with the real client saw larger blocks than the first probe:
9.2–11.0 MiB, arriving as ~1600 fragments of 16 KiB each. libcurl hands back
about 16 KiB per `curl_ws_recv` call regardless of `CURLOPT_BUFFERSIZE`, so
fragment count follows message size and reassembly is the normal path.

Block size tracks chain activity and has no fixed ceiling, so the message cap
is set at 64 MiB — far above anything observed. The cap exists to bound a
runaway peer; sizing it snugly would kill working connections.

Where the bytes are, in a 9.57 MiB block of 1719 transactions:

| part | share |
| --- | --- |
| `meta` | 76% |
| `transaction` | 23% |

Within `meta`: `logMessages` 20%, pre/post token balances 35%,
`loadedAddresses` 7%, `innerInstructions` 5%. Neither `encoding=base64` (8.5%
smaller) nor `transactionDetails=accounts` (9.4% smaller) touches any of it —
there is no RPC option that excludes logs or balances.

### Compression is unavailable on this path

gzip shrinks a 12.21 MiB block to 0.93 MiB, a factor of 13, and libcurl
applies it transparently over HTTP. It is **not** available over the
WebSocket: libcurl does not implement `permessage-deflate`.

That asymmetry is worth remembering. The HTTP recovery path must set
`CURLOPT_ACCEPT_ENCODING`, where it is nearly free. If WebSocket ingress ever
becomes the binding constraint, compression is the largest single lever
available and would require either a client that negotiates the extension or a
move to a binary transport (Geyser, already in the backlog).

Throughput against a free, temporary endpoint fell short of the ~2.5 blocks/s
the chain produces, by both transports. That endpoint is rate limited and is
not representative; the project targets a paid provider, where this shape is
known to work. The figures above are kept for sizing, not as a capacity claim.

### Scope: the whole stream, whatever the indexer keeps

The subscription is `"all"` with `transactionDetails=full`. D5 later narrowed
what is *persisted* to a trading terminal's entities, but it did not narrow
what is *received*, and the two are independent: D5 indexes only what it
observes and never looks anything up, so a filtered subscription would not be
an optimisation but a permanent blind spot — a pool that appears tomorrow is
discovered only by watching every block today. The figures above are therefore
the design target, not a worst case: every milestone is sized for ~12 MiB/s and
~2600 tx/s sustained, even though the vote filter means far fewer transactions
reach storage.

`transactionDetails` also accepts `accounts`, `signatures` and `none`, and the
filter accepts `{"mentionsAccountOrProgram": <pubkey>}`. Both stay exposed in
`idx_config` — they are the first lever for a deployment that only cares about
specific programs, and they make load testing cheap — but neither is the
default.

Two consequences follow from committing to full volume:

- **Storage, not parsing, is the likely bottleneck.** 2600 transactions/s
  sustained is the number M6 has to absorb; a parser at even 100 MiB/s has an
  order of magnitude of headroom, a row-at-a-time insert path does not.
- **~1 TiB/day of ingress is an operating cost.** If it becomes the binding
  constraint, the answer is not to tune the client but to change the source —
  a dedicated node or Geyser, already in the backlog.

**Revisit if** the endpoint stops supporting `blockSubscribe`, in which case
the fallback is `slotSubscribe` plus `getBlock` — which the recovery path
already implements.

---

## D2 — yyjson, vendored behind `idx_json`

**Status:** accepted · **Affects:** M3, M5

Measurements in D1a put this at roughly 12 MiB of JSON per second, sustained,
containing ~2600 transactions/s, so the parser sits on the critical path.

**Decision.** Vendor `yyjson` 0.12.0 (MIT, single translation unit, no
dependencies) in `vendor/yyjson/`, reached only through `include/json.h`.
Nothing outside `src/json.c` includes `yyjson.h`.

### Measured throughput

A real 5.25 MiB `blockSubscribe` notification containing 1316 transactions,
parsed 30 times, release build:

| mode | ms/block | MiB/s | blocks/s |
| --- | --- | --- | --- |
| copying parse | 3.0 | 1741 | 332 |
| in-situ parse (incl. the memcpy) | 2.8 | 1878 | 358 |

Against a requirement of ~12 MiB/s and ~2.5 blocks/s, that is roughly two
orders of magnitude of headroom.

### Consequences

- **Parsing is not the bottleneck and should not be optimized further.** The
  in-situ path is only ~8% faster here, so it is worth using where the buffer
  is already owned and disposable, but it is not worth contorting the receive
  loop for.
- **This confirms the expectation in D3** that storage, not decoding, is what
  M7 has to be designed around.
- Rejected: `cJSON` (allocates a node per value), and a hand-written streaming
  parser (with this much headroom, it would buy nothing for considerable
  effort and risk).

---

## D3 — ClickHouse over its HTTP interface

**Status:** accepted · **Affects:** M6, M8

**Decision.** ClickHouse is the storage backend, reached over its HTTP
interface with libcurl — the same dependency already taken in D1. The native
TCP protocol is not used: its client libraries are C++, and the HTTP interface
accepts the same formats with none of the binding work.

Rows are sent as `RowBinary`, which is compact, cheap to generate from C, and
avoids escaping entirely:

```
POST /?query=INSERT%20INTO%20transactions%20FORMAT%20RowBinary
```

`JSONEachRow` is useful while developing because it is readable; `RowBinary` is
what the hot path should use.

### Why it fits

D1a commits the project to ~2600 transactions/s sustained. A columnar store
absorbs that comfortably, and the queries an indexer serves — scans and
aggregations over slots, accounts and programs — are what it is built for.

### Consequences

- **Batch size is a correctness concern, not just a tuning one.** Every insert
  creates a part, and too many small parts triggers `TOO_MANY_PARTS`. The
  writer accumulates rows and flushes on a row count or a time bound, whichever
  comes first — not once per block.
- **There are no upserts.** The M6 item "idempotent upserts" has to be
  expressed the ClickHouse way: `ReplacingMergeTree` keyed on the sort key with
  a version column, accepting that deduplication happens at merge time and that
  reads either tolerate duplicates or use `FINAL`.
- **Deletes are mutations and are expensive.** Rolling back an orphaned slot
  with `ALTER TABLE ... DELETE` is asynchronous and heavy. See D4.
- **The schema is denormalized on purpose.** Wide tables ordered by
  `(slot, transaction_index)`, partitioned by slot range, with per-column
  codecs. Joins across normalized tables are the wrong shape here.
- **No new build dependency.** libcurl covers it.

---

## D4 — Two tiers: PostgreSQL for `confirmed`, ClickHouse for `finalized`

**Status:** accepted · **Affects:** M4, M6, M8

Deleting rows in ClickHouse is expensive (D3), which would make reorg handling
its worst case. Splitting by commitment level removes the problem instead of
working around it.

- **`confirmed` tier — PostgreSQL.** Holds the unfinalized window. Mutable: a
  reorg overwrites it.
- **`finalized` tier — ClickHouse.** Append-only. A reorg can never reach it,
  so it never deletes.

Consumers join the two into a consolidated view. That is outside this project's
scope; the indexer's contract is that each tier is internally consistent.

### Why PostgreSQL for the confirmed tier

The unfinalized window is ~13 slots, so this tier holds seconds of data — the
columnar argument does not apply at that size. What it does need is exactly
what ClickHouse is worst at:

- Reorg is `DELETE FROM ... WHERE slot >= $1` on a small indexed table.
- Bars need read-modify-write, expressed as one atomic `INSERT ... ON CONFLICT
  DO UPDATE` with `greatest`/`least`/`+` on the OHLCV columns.
- The rollback and the rewrite belong in one transaction, so consumers never
  observe a half-applied reorg.

libpq is a first-class C library, the same shape of integration as libcurl.

Rejected: Redis (index design moves into key space; range queries by pool and
time get painful), SQLite (wrong if the consuming backend is a separate
service), ClickHouse for both (reorg deletes force slot-level partitioning —
about 216k partitions/day at 2.5 slots/s).

### Bars are recomputed, not deleted

A bar spans several slots, so a reorg can invalidate part of one. Deleting rows
by slot is not enough: after removing swaps at or above the reorged slot, the
affected buckets must be recomputed from the swaps that remain. This is a
plain SQL aggregate over a small window and is the strongest single argument
for a relational confirmed tier.

### One subscription, then promotion

Subscribing twice — once at `confirmed`, once at `finalized` — would double
ingress to ~24 MiB/s, about 2 TiB/day. Instead the indexer subscribes once at
`confirmed` and **promotes**: when a slot finalizes, its rows move from
PostgreSQL to ClickHouse without refetching the block. Finalization is tracked
from root notifications rather than a second block stream.

This also means the confirmed tier must retain data until finalization plus a
safety margin, and that promotion is a bulk read feeding the batching writer
described in D3.

---

## D5 — Domain decoding scope

**Status:** accepted · **Affects:** M5, M6, M7, M9

The tables named before this decision — trades, mints, transfers, bars — are
domain entities, not raw chain structures. Deriving them requires knowing what
the indexer is for, because a general-purpose ledger and a trading data source
keep almost disjoint sets of rows.

**Decision.** The indexer feeds a **trading terminal**. It persists balances,
transfers, swaps and the price series derived from swaps, and nothing else. It
indexes **only what it observes** in the block stream: no state is ever fetched
from a node to complete a record, and what was not seen does not exist.

### The M5/M6 boundary is the program, not the work

Instruction decoders for built-in programs (System, SPL Token, SPL Token-2022)
belong to M5; decoders for domain programs — the DEX venues — belong to M6. A
built-in instruction layout is a fixed discriminant plus fields, as much a
chain format as a v0 message or a lookup table, and it does not depend on which
venues this decision names. What M6 owns is the step after: turning a decoded
`Transfer` into a transfer row, a swap, or a bar.

Token-2022 is the exception worth bounding. Its base instruction set mirrors
SPL Token, but the extension surface (transfer fees, confidential transfers,
metadata pointers, transfer hooks) is larger than everything else in M5 put
together. M5 decodes the base set and identifies extension instructions;
per-extension payloads wait for a consumer that needs them.

### What is persisted

| Entity | Shape | Source | Key |
| --- | --- | --- | --- |
| `blocks` | state | block header | `slot` |
| `sol_balances` | state | `meta.pre/postBalances` | account |
| `token_balances` | state | `meta.pre/postTokenBalances` | token account |
| `sol_transfers` | event | System instructions | instruction path |
| `token_transfers` | event | SPL Token/2022 instructions | instruction path |
| `swaps` | event | per-venue decoders + balance deltas | instruction path |
| `pools` | dimension | observed swaps, creation instructions | pool address |
| `tokens` | dimension | balances, mint and metadata instructions | mint address |
| `bars_1s` `bars_1m` `bars_1d` | derived | swaps priced against a quote mint | `(pool, bucket)` |

The instruction path is `(slot, transaction_index, instruction_index,
inner_index)`. It identifies an event uniquely, survives the D4 reorg delete
because it leads with the slot, and lets any derived row be traced back to the
instruction that produced it.

`meta.pre/postBalances` covers every account the transaction touches, so SOL
balances are complete for observed accounts at no decoding cost. Token balances
are sparse by construction and carry `owner` and `decimals` in the wire form,
which is what makes "how much of each token does this wallet hold" a query
rather than a derivation.

Price is a nullable column on `swaps`, not a table: it is filled when one side
of the swap is a quote mint — SOL/WSOL, USDT, USDC, USD1, configurable — and
left empty otherwise. A swap between two non-quote tokens is still recorded; it
simply produces no price.

Every table carries `slot` so the D4 reorg path can delete by slot. The state
tables are versioned by slot rather than appended: an upsert in PostgreSQL, a
`ReplacingMergeTree` keyed on the account with the slot as version in
ClickHouse.

### Vote transactions are dropped

They are the majority of transactions in a mainnet block — every validator
votes every slot — and they produce no balance change, transfer or swap that
this indexer keeps. They are recognised by their program and discarded before
extraction. This is the single largest lever on storage volume in the project,
and it costs one comparison per transaction.

### Discovery replaces lookup

Not fetching account state sounds like it would cripple swap decoding, since a
swap instruction's payload rarely says which mint is which side. It does not:
the token balance deltas in `meta` for the pool's own accounts state exactly
which mints moved and by how much. The per-venue decoder supplies the account
list of that specific invocation, which is what attributes each delta to the
right pool when a transaction routes through several. Pool structure is
therefore learned from the first swap observed, and a creation instruction only
enriches a record that already exists.

Token metadata is where the rule costs something. `decimals` arrives free with
every token balance. Name and symbol live in the Metaplex Token Metadata
program, or in the Token-2022 `TokenMetadata` extension, so they are known only
for tokens whose metadata instruction was observed — in practice, tokens born
after indexing started. The description is not on chain at all: the metadata
account holds a URI to JSON on Arweave or IPFS. `tokens` stores the URI
unresolved. Resolving it is an HTTP fetch against a service that is not the
chain, and it belongs to a consumer, not to the indexer.

### Bars are keyed by pool, never aggregated across pools

The same pair trades in many pools, and a price series merged across them is
worse than any of its inputs: minor pools carry mostly arbitrage flow, and
their prints move the aggregate without anything having happened in the market
a user is watching. This was tried and it does not hold up. The terminal lists
the pools that trade a pair and the user picks one; `swaps` keeps the pool on
every row so a consumer can still combine them deliberately.

Two resolutions are stored: **1s** and **1m**. Every coarser interval a
terminal offers is built from those two on read. **1d** exists for a different
reason — a pool with no swaps for a long window is abandoned, and keeping
second-resolution bars for it is pure cost. Those pools are rolled up to daily
bars and their fine-grained rows dropped. `bars_1s` is the largest table in the
design, at 86400 buckets per active pool per day, and this rollup is what keeps
it bounded.

### What this rules out

- **No transaction table, and therefore no ledger queries.** Nothing stores
  transactions as such, so "transaction by signature" and "transactions by
  account" cannot be answered. Every event row carries its signature, which is
  enough to link out to an explorer, and M9 is scoped to the terminal's reads —
  wallet, token, pool — rather than to the chain.
- **No mint and burn events.** They change balances, and the balance state
  already reflects that. `InitializeMint` is still observed, because it is one
  of the sources for the `tokens` dimension.
- **No account state indexing**, which stays in the backlog, and no RPC call to
  complete a record the stream did not carry.
- **History of balances is not kept**, only their current value per slot. A
  consumer that wants a wallet's balance over time reconstructs it from the
  transfer and swap rows.

**Revisit if** the terminal grows a view that needs a chain-wide query rather
than a wallet-, token- or pool-anchored one, which would bring back a
transaction table and with it the volume this decision exists to avoid; or if
metadata coverage for pre-existing tokens turns out to matter more than the
no-lookup rule, in which case the narrowest exception is a one-off backfill of
the `tokens` dimension, never a fetch on the indexing path.

---

## D6 — Two threads, a bounded ring, and no backpressure on the socket

**Status:** accepted · **Affects:** M4, M7, M8

Follow mode runs the consumer inline on the receive loop. That is fine while
the consumer counts transactions, and stops being fine the moment M7 puts a
network round trip to PostgreSQL behind it: a consumer that blocks blocks the
socket read, TCP backpressure builds, and the provider drops the connection —
the failure D1a named as a design constraint rather than a refinement.

**Decision.** Two threads. A receive thread owns the socket and does nothing
but reassemble notifications and publish them. A processing thread decodes,
derives and writes. Between them sits a bounded ring of fixed-size descriptors
pointing into a pool of payload buffers. When the ring is full the receive
thread overwrites the oldest entry rather than waiting; the processing thread
notices the slot discontinuity and hands that range to the gap fetcher. The
socket is never made to wait for anything downstream.

The model is taken from Firedancer's `tango` layer
([firedancer-io/firedancer](https://github.com/firedancer-io/firedancer),
`src/tango/`), which is worth reading before changing any of this.

### Why an overrun is cheap here and expensive there

Firedancer's ring carries a global sequence number per fragment, and a consumer
that sees a gap in it knows it was overrun (`src/tango/fd_tango_base.h`). Its
flow control header is blunt about which side to prefer:

> backpressure is the worst thing in the world for a large scale distributed
> system [...] limit the number of strictly reliable consumers needed in the
> system to, ideally, zero. (`src/tango/fctl/fd_fctl.h`)

A validator pays for that with real data loss: a dropped shred is gone. The
indexer does not. Every slot the ring drops is recoverable with `getBlock` over
the HTTP path M3 already built, so an overrun costs a refetch, not data. The
argument for the unreliable-consumer model is therefore stronger here than in
the system it comes from.

Nothing new is needed to detect it. Slot numbers are already the sequence, and
the gap check the pipeline performs between consecutive slots already reports
exactly the range that was dropped.

### Overwrite the oldest, not the newest

Both keep the socket moving; they differ in where the survivor sits. Dropping
the newest keeps a stale window and pushes the indexer further behind the tip
with every overflow. Overwriting the oldest keeps the indexer near the tip and
sends the older slots — the ones most likely to still be served, and least
likely to be needed live — down a recovery path that already exists and can run
concurrently. Total lag is lower and the recovered work is the cheaper half.

### The parsed document is what crosses, by ownership

This was first written as a pool of payload buffers with the descriptors
pointing into it, mirroring Firedancer's split between its metadata cache and
its data cache. Implementing it showed the premise was wrong. `idx_pubsub` has
to parse each notification anyway, to read the subscription id and demultiplex
it, and it does so with the copying parse — so by the time the receive thread
has a slot number it is already holding a self-contained `idx_json_doc` that
owes the connection nothing.

Copying the payload into a pool and parsing it again on the far side would
therefore have been one copy and two parses of the same 5 to 11 MiB. The ring
carries the document instead, transferring ownership, and the receive thread
also passes the node it already located inside it. No copy, no second parse.

Ring depth stays a memory decision — a parsed block is tens of megabytes and
the ring may hold `depth` of them — and is configurable through
`queue_depth` rather than compiled in.

Firedancer cannot do this. Its payloads arrive as raw frames in a shared
memory region with no per-message ownership to hand over, which is what makes
its speculative read-then-recheck necessary in the first place. The difference
is that a parse already happened here, one layer down.

### Two threads, and not more

Decoding is not the constraint. D2 measured the parser at ~1878 MiB/s against a
requirement of ~12 MiB/s, and a live run against the demo endpoint saw the
release build only 36% ahead of an ASan debug build — if compute were binding,
that gap would be far wider. What can block is storage, and that is one
consumer. A third thread, or a pool of them, is a decision for M7 with a
measurement behind it, not now.

### Consequences

- **The ring is a liveness mechanism, not a throughput one.** It exists so a
  slow writer cannot cost the connection. Sizing, observability and correct
  behaviour under overflow matter; shaving nanoseconds off a handoff does not.
- **Ownership is the concurrency design.** `idx_ws` and `idx_pubsub` are
  documented as single-owner and belong to the receive thread. The slot cursor
  belongs to the processing thread, which observes slots as it dequeues them,
  so no field of it is written by two threads. `last_seen` therefore comes to
  mean the highest slot that entered the pipeline rather than the highest the
  socket delivered, which is the more useful of the two for gap detection.
- **Each thread gets its own arena**, as `docs/conventions.md` already
  requires.
- **Shutdown gains a drain step.** A stop request has to reach the receive
  thread, close the ring, and let the processing thread finish what is queued
  before the cursor is persisted. This is what the M4 shutdown item is waiting
  on.
- **The ring is testable without a socket.** It is fed directly, so the
  overflow policy gets a unit test with a synthetic producer — which matters,
  because the rate-limited demo endpoint delivers well under what the chain
  produces and will never overflow anything.
- **The ring is guarded by a mutex and a condition variable**, not by lock-free
  publication. This was first written the other way — a release store on the
  entry's sequence, paired with an acquire load — and the reason it is not is
  that dropping the oldest entry means the producer moves the consumer's read
  position, which single-writer-per-variable publication cannot express without
  a compare-and-swap. At 2.5 hand-offs per second, against critical sections of
  a few pointer moves that never contain I/O, the lock costs nothing measurable
  and the accounting is far easier to be sure of. What is deliberately not
  taken from Firedancer is its AVX-atomic stores and double cache-line padding:
  those buy nothing at these rates and would tie the project to x86-64, which
  is why that client supports only x86-64 in the first place.

### Not taken from Firedancer

Its shared-memory workspaces, NUMA placement and huge pages exist for
inter-process messaging; the indexer is one process. Tiles as sandboxed
processes answer a threat model about validator keys that does not apply here.
Busy-polling on a tick counter burns a core to save microseconds this workload
does not need to save. Fragment reassembly with origin ids and start/end
markers is solved a layer below by libcurl.

**Revisit if** storage turns out to need more than one consumer, in which case
the ring grows from single-consumer to multi-consumer and the sequencing has to
carry per-consumer positions; or if the ring is observed to overflow against a
paid endpoint, which would mean the pipeline is genuinely undersized rather
than merely protected.

---

## D8 — An aggregator route is not a pool swap

**Status:** accepted · **Affects:** M6, M7, M9

The venues M6 decodes are of two kinds. A pool holds liquidity and quotes a
price; an aggregator holds none and routes through the pools that do. Jupiter
is the second, and a route both is not a swap and *contains* the swaps that
are, one per leg, as inner instructions of the very same transaction.

**Decision.** Only a pool produces a `swaps` row. A route is decoded — its
`SwapEvent` states the mints and exact amounts of each leg — but it is recorded
as attribution, never as a swap of its own. `idx_venue_is_pool` is the line.

Three things follow.

**Double counting is the whole point.** A routed trade appears twice in a
block: once as Jupiter's `SwapEvent` and once as the pool's own instruction or
event. Counting both would double the volume of every venue Jupiter routes
through, and since most retail flow on Solana is routed, that is most of the
volume. Bars built on it would be wrong in a way no one would notice, because
the shape of the series would look right.

**`SwapEvent.amm` is not a pool address.** It is documented as the AMM that
filled the leg, and for a PumpSwap leg it carries the PumpSwap *program* id,
observed directly in a mainnet block. A pool column that is sometimes a program
is worse than no column, so the field is dropped rather than stored and
explained away. The leg's own venue supplies the pool when this indexer decodes
that venue.

**A route still reaches further than the decoders do.** Its event states mints
and amounts for legs through venues M6 has no decoder for, which is the only
place a swap on an unknown program is visible at all. That is worth keeping
even though it produces no pool row, and it is what makes the aggregator worth
decoding rather than skipping.

**Revisit if** the query side needs "what did this wallet trade" independently
of which pools filled it, in which case routes become an entity of their own —
still not a pool swap, but a row rather than an annotation.
