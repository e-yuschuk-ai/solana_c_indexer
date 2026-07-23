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

### Scope: general purpose

The indexer is general purpose, so it subscribes with `"all"` and
`transactionDetails=full`. The figures above are therefore the design target,
not a worst case: every milestone is sized for ~12 MiB/s and ~2600 tx/s
sustained.

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

Rejected: Redis (index design moves into key space; range queries by market and
time get painful), SQLite (wrong if the consuming backend is a separate
service), ClickHouse for both (reorg deletes force slot-level partitioning —
about 216k partitions/day at 2.5 slots/s).

### Bars are recomputed, not deleted

A bar spans several slots, so a reorg can invalidate part of one. Deleting rows
by slot is not enough: after removing trades at or above the reorged slot, the
affected buckets must be recomputed from the trades that remain. This is a
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

**Status:** open · **Due:** start of M5

The confirmed-tier tables named so far — trades, mints, transfers, bars — are
domain entities, not raw chain structures. Transfers and mints come from SPL
Token, which M5 already covers, but trades require per-DEX instruction decoders
(Raydium, Orca, Meteora, pump.fun and others), and bars are derived from
trades rather than decoded from anything.

M5 as written stops at generic instruction decoding. The domain layer —
per-program decoders plus the derivation of bars — needs its own milestone
between decoding and storage, and its scope depends on which venues matter.

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

### Descriptors and a buffer pool, not payloads in the ring

Blocks are 5 to 11 MiB, so a ring that holds payloads inline costs depth times
peak block size and copies every byte twice. The ring holds `{slot, buffer,
length}` and the payload stays where the receive thread reassembled it, which
is the split Firedancer makes between its metadata cache and its data cache.
Ring depth becomes a memory decision — depth times the message cap — and is
configurable rather than compiled in.

Firedancer lets a consumer read a payload speculatively and re-check the
sequence number afterwards to find out whether it was overwritten mid-read,
which removes buffer ownership entirely. That trade does not carry: redoing a
10 MiB parse costs far more than coordinating ownership at 2.5 handoffs per
second. The receive thread writes only into buffers the processing thread has
released, and "no free buffer" is what triggers the overflow policy above.

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
- **Publication uses C11 atomics**, a release store on the entry's sequence
  paired with an acquire load. Firedancer's AVX-atomic stores and double
  cache-line padding buy nothing at these rates and would tie the project to
  x86-64, which is why that client supports only x86-64 in the first place.

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
