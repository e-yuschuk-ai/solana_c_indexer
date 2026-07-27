# Storage specification

What this indexer persists, where, and why it is split across two databases.
Written to be implementable in any language and against any pair of stores with
the same properties; the schemas below are the concrete ones this project uses.

[PARSING.md](PARSING.md) describes how every entity here is derived. This
document starts where that one stops.

Design decisions referenced as `D<n>` are in [../decisions.md](../decisions.md).

---

## 1. Why two storages

### 1.1 The problem

A columnar store is the obvious choice for this workload: ~2600 transactions/s
sustained, and the queries an indexer serves — scans and aggregations over
slots, accounts and pools — are exactly what it is built for (D3). Using it for
*everything* runs into two walls, both created by the same thing: **reorgs**.

- **Deletes are mutations and are expensive.** Rolling back an orphaned slot
  with `ALTER TABLE ... DELETE` is asynchronous and heavy. Making it cheap would
  require slot-level partitioning — about 216k partitions/day at 2.5 slots/s,
  which is not a viable table.
- **Bars need read-modify-write.** A bar spans several slots, so a reorg can
  invalidate part of one. Removing the swaps at or above the reorged slot is not
  enough: the affected buckets must be recomputed from the swaps that remain.
  That is a small transactional aggregate, and it is the single strongest
  argument for a relational store.

### 1.2 The split

Splitting by **commitment level** removes the problem rather than working around
it (D4):

| Tier | Store | Holds | Mutability | Operations |
| --- | --- | --- | --- | --- |
| `confirmed` | PostgreSQL | The unfinalized window (~13 slots — seconds of data) | Mutable | write, reorg, prune, read-range |
| `finalized` | ClickHouse | Everything finalized | **Append-only** | append, flush |

A reorg can never reach finalized data, so the finalized tier **never deletes**.
Every operation that is hard in a columnar store happens in the tier that is
seconds wide, small, indexed and transactional.

The unfinalized window is seconds of data, so the columnar argument simply does
not apply at that size. What it *does* need is what ClickHouse is worst at:
`DELETE FROM ... WHERE slot >= $1` on a small indexed table, an atomic
`INSERT ... ON CONFLICT DO UPDATE` folding OHLCV columns, and a rollback plus a
rewrite inside one transaction so consumers never observe a half-applied reorg.

### 1.3 One subscription, then promotion

Subscribing twice — once at `confirmed`, once at `finalized` — would double
ingress to ~24 MiB/s, about 2 TiB/day. Instead the indexer subscribes **once** at
`confirmed` and **promotes**: when a slot finalizes, its rows are bulk-read out
of PostgreSQL and batch-inserted into ClickHouse, **without refetching the
block**. Finalization is tracked from root notifications, not from a second
block stream.

Two requirements follow, and they are requirements rather than niceties:

- The confirmed tier must **retain** data until finalization plus a safety
  margin.
- Promotion is a bulk read feeding a batching writer, never a row-at-a-time
  copy.

### 1.4 What the indexer promises a consumer

**Each tier is internally consistent.** Joining the two into a single
consolidated view is the consumer's job and is outside this project's scope. In
practice: read finalized data from ClickHouse, overlay the last few seconds from
PostgreSQL, and prefer PostgreSQL for any slot present in both.

### 1.5 Rejected alternatives

| Rejected | Why |
| --- | --- |
| ClickHouse for both tiers | Reorg deletes force slot-level partitioning — ~216k partitions/day |
| PostgreSQL for both tiers | Will not absorb the finalized volume; the columnar argument is real at that size |
| Redis for the confirmed tier | Index design moves into key space; range queries by pool and time get painful |
| SQLite for the confirmed tier | Wrong if the consuming backend is a separate service |

---

## 2. The storage abstraction

The two tiers are **not one shape**, so forcing them behind one identical
interface would either give the finalized tier operations it must never perform
(delete, reorg) or deny the confirmed tier the ones it exists for. The seam is
therefore **one interface per tier over one shared write vocabulary**.
(`include/store.h`)

```
confirmed store                     finalized store
├── write(write_set)                ├── append(write_set)
├── reorg(from_slot, replacement)   ├── flush()
├── prune(below_slot)               └── close()
├── read_range(from, to) → set
└── close()
```

Both consume the same **write set**: the entities derived from one or more
slots, gathered into one unit of work.

```
write_set
├── blocks[]          block header rows
├── sol_balances[]    balance observation + ref
├── token_balances[]  balance observation + ref
├── transfers[]       transfer event + ref   (kind decides the target table)
├── swaps[]           swap event + price + block_time + ref
├── pools[]           merged registry records
├── tokens[]          merged registry records
└── bars[]            merged registry records
```

The `ref` is the part of the instruction path an entity does not carry on its
own:

```
ref = (slot, transaction_index, signature)
```

The entities carry their position *within* a transaction; the slot and
transaction index belong to the caller walking the block, and the signature
links a row back to an explorer since no transaction table exists.

Two properties of this design worth keeping in a re-implementation:

- **The pipeline never sees a database.** It holds two handles.
- **An in-memory reference backend of each tier ships with the abstraction.** It
  is the executable spec a real backend is contract-tested against, and it lets
  the whole pipeline run end to end with no database attached.

A store handle belongs to one thread — the processing thread. Nothing here is
thread-safe.

---

## 3. Entity catalogue

Nine entities, five shapes. The shape decides the write semantics far more than
the entity does.

| Entity | Shape | Derived from (PARSING.md) | Key |
| --- | --- | --- | --- |
| `blocks` | state | block header (§3.1) | `slot` |
| `sol_balances` | state | `meta.pre/postBalances` (§6.1) | account |
| `token_balances` | state | `meta.pre/postTokenBalances` (§6.2) | token account |
| `sol_transfers` | event | System instructions (§6.3) | instruction path |
| `token_transfers` | event | token program instructions (§6.3) | instruction path |
| `swaps` | event | venue decoders + normalization (§7, §8) | instruction path |
| `pools` | dimension | observed swaps, creations (§10.1) | pool address |
| `tokens` | dimension | balances, metadata instructions (§10.2) | mint address |
| `bars_1s`, `bars_1m`, `bars_1d` | derived | priced swaps (§11) | `(pool, bucket)` |

Write semantics per shape:

| Shape | Semantics |
| --- | --- |
| state | **Upsert on the key**, versioned by slot. One current value per account, not a log |
| event | **Insert, idempotent on the instruction path**. Re-indexing a slot converges |
| dimension | **Upsert, accumulating**: a later observation fills what an earlier one left unknown, never overwrites it |
| derived (bars) | **Merge**: high/low by extremes, volumes and counts summed, open/close by execution sequence |

### 3.1 What is deliberately not stored

| Not stored | Consequence, accepted |
| --- | --- |
| Transactions as such | "Transaction by signature" and "transactions by account" cannot be answered. Every event row carries its signature, which is enough to link out to an explorer |
| Vote transactions | The largest single lever on volume; they produce none of the entities above |
| Raw instructions, logs, account keys | Nothing consumes them after derivation |
| Mint/burn as their own events | They are transfer rows to and from the mint (§6.3), and the balance state already reflects them |
| Balance history | Only the current value per account. A consumer that wants a wallet's balance over time reconstructs it from the transfer and swap rows |
| Resolved token metadata JSON | The URI is stored unresolved |

### 3.2 Why price is a column and not a table

A swap's price is nullable on the swap row: filled when one side is a quote mint,
empty otherwise. A swap between two non-quote tokens is still recorded — it
simply produces no price. Making it a table would add a join to every read of
the entity that most needs to be cheap.

Note the two distinct null states (PARSING.md §9.3): `quote` set with `price`
null means *priced but numberless* (a quote side whose decimals the block never
carried); both null means *no quote side at all*.

### 3.3 Why bars are keyed by pool

The same pair trades in many pools, and a price series merged across them is
worse than any of its inputs: minor pools carry mostly arbitrage flow, and their
prints move the aggregate without anything having happened in the market a user
is watching. This was tried and it does not hold up. The terminal lists the
pools that trade a pair and the user picks one; `swaps` keeps the pool on every
row so a consumer can still combine them deliberately.

### 3.4 Enum encodings

Stored as small integers. A re-implementation must keep these values stable —
they are on disk.

| Enum | Values |
| --- | --- |
| transfer kind | 0 sol · 1 token · 2 mint · 3 burn |
| swap row kind | 0 pool · 1 aggregated (route) |
| venue | 0 none · 1 pump_curve · 2 pump_amm · 3 raydium_amm_v4 · 4 raydium_clmm · 5 raydium_cpmm · 6 jupiter |
| amount source | 0 none · 1 event · 2 raylog · 3 delta |
| quote | 0 none · 1 sol · 2 usdc · 3 usdt · 4 usd1 · 5 other |
| bar interval | 0 = 1s · 1 = 1m |

---

## 4. The confirmed tier — PostgreSQL

Implemented in `src/pg_store.c`, over a thin libpq wrapper (`src/pg.c`) that is
the only place the driver is reached. The schema is created on open with
`CREATE TABLE IF NOT EXISTS`; a dedicated migration path is a later item.

### 4.1 Type conventions

| Domain value | SQL type | Why |
| --- | --- | --- |
| Pubkey, blockhash | `BYTEA` (32) | Half the size of base58 text and directly comparable |
| Signature | `BYTEA` (64) | Same |
| Slot, block height, lamports | `BIGINT` | Lamports are bounded by a supply two orders of magnitude below `int64` |
| Raw token amount | `NUMERIC(20,0)` | A raw token amount is bounded only by `uint64`, which `BIGINT` cannot hold |
| Price, OHLC, volumes | `DOUBLE PRECISION` | ~15–16 significant digits covers 1e-9 to 1e9 with room to spare; the raw amounts stay on the swap row for exactness |
| Decimals, enums, flags | `SMALLINT` / `BOOLEAN` | |
| Bar sequence | `BYTEA` (15) | Packed big-endian so a bytewise comparison reproduces execution order (PARSING.md §12.2) |

**Every table carries `slot`** (bars carry `close_seq_slot`) **and is indexed by
it.** That single rule is what makes the reorg delete and the retention prune
range operations rather than scans, and it is the reason the tier can absorb a
reorg in milliseconds.

### 4.2 Schema

```sql
CREATE TABLE blocks (
  slot                BIGINT PRIMARY KEY,
  blockhash           BYTEA NOT NULL,
  previous_blockhash  BYTEA NOT NULL,
  parent_slot         BIGINT NOT NULL,
  block_time          BIGINT,            -- nullable: the chain's own is optional
  block_height        BIGINT,            -- nullable
  transaction_count   INTEGER NOT NULL); -- a convenience, not a join key

CREATE TABLE sol_balances (
  account            BYTEA PRIMARY KEY,  -- state: one row per account
  slot               BIGINT NOT NULL,    -- the version
  lamports           BIGINT NOT NULL,    -- after the transaction
  delta              BIGINT NOT NULL,    -- signed; never zero
  transaction_index  INTEGER NOT NULL,
  signature          BYTEA NOT NULL);
CREATE INDEX sol_balances_slot ON sol_balances (slot);

CREATE TABLE token_balances (
  account            BYTEA PRIMARY KEY,  -- the token account, not its owner
  slot               BIGINT NOT NULL,
  mint               BYTEA NOT NULL,
  owner              BYTEA,              -- absent in older blocks
  amount             NUMERIC(20,0) NOT NULL,  -- raw units after
  previous           NUMERIC(20,0) NOT NULL,  -- raw units before
  decimals           SMALLINT NOT NULL,
  closed             BOOLEAN NOT NULL,   -- held none after
  transaction_index  INTEGER NOT NULL,
  signature          BYTEA NOT NULL);
CREATE INDEX token_balances_slot ON token_balances (slot);

CREATE TABLE sol_transfers (
  slot               BIGINT NOT NULL,
  transaction_index  INTEGER NOT NULL,
  instruction_index  INTEGER NOT NULL,
  inner_index        INTEGER NOT NULL,
  signature          BYTEA NOT NULL,
  source             BYTEA NOT NULL,
  destination        BYTEA NOT NULL,
  authority          BYTEA,              -- the seeded/nonce signer, when named
  amount             NUMERIC(20,0) NOT NULL,
  PRIMARY KEY (slot, transaction_index, instruction_index, inner_index));

CREATE TABLE token_transfers (
  slot               BIGINT NOT NULL,
  transaction_index  INTEGER NOT NULL,
  instruction_index  INTEGER NOT NULL,
  inner_index        INTEGER NOT NULL,
  signature          BYTEA NOT NULL,
  kind               SMALLINT NOT NULL,  -- token | mint | burn
  source             BYTEA NOT NULL,     -- the mint itself for a mint row
  destination        BYTEA NOT NULL,     -- the mint itself for a burn row
  authority          BYTEA,
  mint               BYTEA,
  source_owner       BYTEA,              -- resolved at write time, never joined
  destination_owner  BYTEA,
  amount             NUMERIC(20,0) NOT NULL,
  fee                NUMERIC(20,0) NOT NULL,  -- Token-2022 transfer fee; 0 otherwise
  decimals           SMALLINT,
  PRIMARY KEY (slot, transaction_index, instruction_index, inner_index));

CREATE TABLE swaps (
  slot               BIGINT NOT NULL,
  transaction_index  INTEGER NOT NULL,
  instruction_index  INTEGER NOT NULL,
  inner_index        INTEGER NOT NULL,
  signature          BYTEA NOT NULL,
  kind               SMALLINT NOT NULL,  -- pool | aggregated route
  venue              SMALLINT NOT NULL,
  amount_source      SMALLINT NOT NULL,  -- event | raylog | delta
  pool               BYTEA,              -- the token mint for a curve trade
  trader             BYTEA,
  input_mint         BYTEA,
  input_amount       NUMERIC(20,0),
  input_decimals     SMALLINT,
  output_mint        BYTEA,
  output_amount      NUMERIC(20,0),
  output_decimals    SMALLINT,
  price              DOUBLE PRECISION,   -- quote units per whole base unit
  quote              SMALLINT,           -- which quote; set even when price is null
  block_time         BIGINT,
  PRIMARY KEY (slot, transaction_index, instruction_index, inner_index));
CREATE INDEX swaps_pool ON swaps (pool);

CREATE TABLE pools (
  address          BYTEA PRIMARY KEY,
  venue            SMALLINT NOT NULL,
  mint_a           BYTEA,               -- an unordered pair; either may fill first
  decimals_a       SMALLINT,
  mint_b           BYTEA,
  decimals_b       SMALLINT,
  first_seen_slot  BIGINT NOT NULL,     -- the slot of the first swap that revealed it
  swap_count       BIGINT NOT NULL,
  creation_slot    BIGINT,              -- creation enrichment, when observed
  creator          BYTEA);
CREATE INDEX pools_slot ON pools (first_seen_slot);

CREATE TABLE tokens (
  mint             BYTEA PRIMARY KEY,
  decimals         SMALLINT,            -- free from any token balance
  name             TEXT,                -- only when a metadata instruction was seen
  symbol           TEXT,
  uri              TEXT,                -- stored unresolved
  first_seen_slot  BIGINT NOT NULL);
CREATE INDEX tokens_slot ON tokens (first_seen_slot);

CREATE TABLE bars (
  pool            BYTEA NOT NULL,
  interval        SMALLINT NOT NULL,    -- 0 = 1s, 1 = 1m
  bucket          BIGINT NOT NULL,      -- the unix second the interval starts at
  quote           SMALLINT NOT NULL,    -- what the price is denominated in
  open            DOUBLE PRECISION NOT NULL,
  high            DOUBLE PRECISION NOT NULL,
  low             DOUBLE PRECISION NOT NULL,
  close           DOUBLE PRECISION NOT NULL,
  base_volume     DOUBLE PRECISION NOT NULL,
  quote_volume    DOUBLE PRECISION NOT NULL,
  swap_count      BIGINT NOT NULL,
  open_seq_key    BYTEA NOT NULL,       -- packed execution sequence, 15 bytes
  close_seq_key   BYTEA NOT NULL,
  close_seq_slot  BIGINT NOT NULL,      -- the reorg handle
  PRIMARY KEY (pool, interval, bucket));
CREATE INDEX bars_close_slot ON bars (close_seq_slot);
```

Both bar resolutions live in **one table** discriminated by `interval`, because
the confirmed tier holds seconds of data and there is nothing to gain from
splitting it. The finalized tier separates them (§5.4), where there is.

### 4.3 Write semantics

Every write is executed through a prepared statement, and a whole write set runs
inside **one transaction**: a reader never sees it half applied.

**Blocks** — upsert on `slot`, overwriting. Re-indexing a slot converges.

**State (`sol_balances`, `token_balances`)** — upsert on the account, guarded:

```sql
INSERT INTO sol_balances(...) VALUES(...)
ON CONFLICT (account) DO UPDATE SET ...
WHERE sol_balances.slot <= excluded.slot;
```

The guard is the point. Blocks can commit out of order (a backfilled slot
arrives after a later one), and without it an old observation would clobber a
newer value. `<=` rather than `<` so a re-index of the same slot still applies.

**Events (`sol_transfers`, `token_transfers`, `swaps`)** — insert keyed on the
instruction path with `ON CONFLICT DO NOTHING`. The path is unique by
construction, so re-indexing a slot is idempotent without a read. Transfers are
routed to one table or the other by `kind`: SOL to `sol_transfers`, everything
else to `token_transfers`.

**Dimensions (`pools`, `tokens`)** — upsert that **accumulates**:

```sql
ON CONFLICT (address) DO UPDATE SET
  mint_a          = coalesce(pools.mint_a, excluded.mint_a),
  decimals_a      = coalesce(pools.decimals_a, excluded.decimals_a),
  ...
  first_seen_slot = least(pools.first_seen_slot, excluded.first_seen_slot),
  swap_count      = greatest(pools.swap_count, excluded.swap_count),
  creation_slot   = coalesce(pools.creation_slot, excluded.creation_slot),
  creator         = coalesce(pools.creator, excluded.creator);
```

`coalesce` keeps the first non-null — a later observation fills what an earlier
one left unknown and never overwrites it. `least` keeps the earliest slot. This
mirrors the registry semantics in PARSING.md §10 exactly, which is what lets the
same rows be written repeatedly with no drift.

**Bars** — merge on `(pool, interval, bucket)`:

```sql
ON CONFLICT (pool, interval, bucket) DO UPDATE SET
  high          = greatest(bars.high, excluded.high),
  low           = least(bars.low, excluded.low),
  base_volume   = bars.base_volume  + excluded.base_volume,
  quote_volume  = bars.quote_volume + excluded.quote_volume,
  swap_count    = bars.swap_count   + excluded.swap_count,
  open          = CASE WHEN excluded.open_seq_key < bars.open_seq_key
                       THEN excluded.open ELSE bars.open END,
  open_seq_key  = least(bars.open_seq_key, excluded.open_seq_key),
  close         = CASE WHEN excluded.close_seq_key > bars.close_seq_key
                       THEN excluded.close ELSE bars.close END,
  close_seq_key = greatest(bars.close_seq_key, excluded.close_seq_key),
  close_seq_slot = greatest(bars.close_seq_slot, excluded.close_seq_slot);
```

Open and close are chosen by **execution order**, not arrival order, through the
packed sequence keys — a `BYTEA` comparison that reproduces the in-process
comparison byte for byte. That is why the key is packed big-endian with the
inner flag between the instruction and inner indices (PARSING.md §12.2).

Note that volumes and counts are **summed**, so a bucket spanning several write
calls is folded rather than overwritten — but also that writing the same bar
twice would double it. Idempotency of bars comes from the caller sending each
bar's *delta* once, and from the reorg path clearing before rewriting.

### 4.4 Reorg

One transaction (D4):

1. Delete at or above `from_slot` from **every** table.
2. Apply the caller's replacement through the ordinary write path.
3. Commit.

Which column each table deletes by:

| Tables | Column |
| --- | --- |
| `blocks`, `sol_balances`, `token_balances`, `sol_transfers`, `token_transfers`, `swaps` | `slot` |
| `pools`, `tokens` | `first_seen_slot` |
| `bars` | `close_seq_slot` |

The bar rule is the one that needs explaining: a bar's close is its latest swap
and a sequence orders by slot first, so **a bar holds a reorged swap exactly when
its `close_seq_slot` is at or above the cut**. No scan, no per-bar reasoning.

The replacement arrives with its bars **already recomputed** by the caller
(PARSING.md §11.3), which is what makes delete-then-rewrite exact and keeps the
store a pure sink. A `NULL` replacement is a pure delete.

There are no foreign keys between the tables, so the delete order is free.

### 4.5 Retention

```sql
DELETE FROM <each table> WHERE slot < $below_slot;   -- first_seen_slot / close_seq_slot as above
```

run as one multi-statement command.

**The store is the mechanism; the policy is the caller's**, expressed in its
choice of slot: how far below the finalized watermark is safe, once those rows
have been promoted. The slot index on every table is what makes this a range
delete rather than a scan.

### 4.6 Promotion read

`read_range(from_slot, to_slot)` gathers every row in a closed slot range into a
write set for the finalized tier's append path — the bulk read that avoids
refetching the block. **Not yet implemented**; the vtable slot is left unset and
the dispatch reports it unsupported rather than pretending.

---

## 5. The finalized tier — ClickHouse

**Status: the transport is implemented (`src/ch.c`); the schema and the writer
are not.** This section is the design those items build to, not a description of
running code. §7 lists exactly what exists.

### 5.1 Transport

ClickHouse is reached over its **HTTP interface**, not the native TCP protocol:
the native client libraries are C++, and the HTTP interface accepts the same
formats with none of the binding work — and it needs only the HTTP client the
project already depends on.

Two things the client does that a bare HTTP POST does not:

- **It separates the query from the data.** A plain query carries the SQL in the
  body; an insert puts the statement in the **URL** so the body is exactly the
  data, which is what lets an insert carry a binary payload.
- **It maps failures onto typed errors.** A transport failure is a network or
  timeout error; a server exception is a remote error carrying the numeric code
  from the `X-ClickHouse-Exception-Code` response header. That is how a caller
  distinguishes a retryable overload (252 `TOO_MANY_PARTS`, memory limit
  exceeded) from a fatal error **without parsing the message**.

### 5.2 Row format

`RowBinary` on the hot path: compact, cheap to generate, and it avoids escaping
entirely.

```
POST /?query=INSERT%20INTO%20swaps%20FORMAT%20RowBinary
<body: the rows, binary>
```

`JSONEachRow` is kept for development and debugging, where readability is worth
the size.

### 5.3 Batching is a correctness concern

Every insert creates a part, and too many small parts triggers `TOO_MANY_PARTS`.
The writer therefore **accumulates rows and flushes on a row count or a time
bound, whichever comes first — never once per block**. This is not tuning; a
per-block insert path fails outright at this rate.

`append` buffers, `flush` writes. Nothing is durable until a flush, and a flush
happens once more at shutdown.

### 5.4 Schema design

Denormalized on purpose: wide tables, no joins across normalized tables, which
is the wrong shape here.

| Table | Engine | Order by | Partition | Notes |
| --- | --- | --- | --- | --- |
| `blocks` | ReplacingMergeTree(slot) | `slot` | slot range | |
| `sol_balances` | ReplacingMergeTree(slot) | `account` | — | Latest observation wins without a delete |
| `token_balances` | ReplacingMergeTree(slot) | `account` | — | Same |
| `sol_transfers` | ReplacingMergeTree | `(slot, transaction_index, instruction_index, inner_index)` | slot range | |
| `token_transfers` | ReplacingMergeTree | same | slot range | |
| `swaps` | ReplacingMergeTree | same | slot range | |
| `pools` | ReplacingMergeTree | `address` | — | |
| `tokens` | ReplacingMergeTree | `mint` | — | |
| `bars_1s` | ReplacingMergeTree | `(pool, bucket)` | time range | The largest table in the design |
| `bars_1m` | ReplacingMergeTree | `(pool, bucket)` | time range | |
| `bars_1d` | ReplacingMergeTree | `(pool, bucket)` | time range | Rollup target for abandoned pools |

Column types map from §4.1: `FixedString(32)` / `FixedString(64)` for keys and
signatures, `UInt64` for raw token amounts (no `NUMERIC` needed — ClickHouse has
the unsigned type PostgreSQL lacks), `Float64` for prices and volumes, `Int64`
for slots and times, `UInt8`/`Enum8` for the enums in §3.4. Per-column codecs
where they pay off — `Delta` + `ZSTD` on the monotonic slot and bucket columns,
`ZSTD` on the wide key columns.

Bars are split into one table per resolution here, unlike §4.2, because the
retention and rollup policies genuinely differ per resolution.

### 5.5 There are no upserts

ClickHouse has none, so "idempotent re-index" is expressed its way:
`ReplacingMergeTree` keyed on the sort key with a **version column** — the slot
for state, the slot for events. Deduplication happens at **merge time**, which
means reads must either tolerate duplicates or use `FINAL`. That is a real
constraint on the query layer, not a footnote: design the read paths for it.

State tables carry no delete at all. The latest observation wins by version.

### 5.6 Bar rollup

`bars_1s` at 86400 buckets per active pool per day is the largest table in the
design, and unbounded growth in it is the main storage risk.

The bound: **a pool with no swaps for a configured window is abandoned.** Its
`1s` and `1m` bars are collapsed into `1d` bars and the fine-grained rows are
dropped. Most pools on Solana are memecoin pools that trade for hours and then
never again, so this reclaims the large majority of the table while leaving
every actively traded pool at full resolution.

Coarser intervals that a terminal offers (5m, 1h, …) are **built on read** from
`1s` and `1m`; only those two, plus the `1d` rollup, are stored.

---

## 6. Sizing

From the measurements in D1a, as the design target rather than a worst case:

| Quantity | Value |
| --- | --- |
| Block stream | ~12 MiB/s sustained, ~1 TiB/day of ingress |
| Transactions | ~2600/s before the vote filter |
| Blocks | ~2.5/s |
| Confirmed tier window | ~13 slots — seconds of data, a small table by any measure |
| `bars_1s` | 86400 rows per active pool per day, before rollup |

The vote filter removes the majority of transactions before anything reaches
storage, so the write rate is a fraction of 2600/s — but every milestone is
sized for the full stream, because what is *received* is not narrowed even
though what is *persisted* is.

---

## 7. Implementation status

| Item | Status |
| --- | --- |
| Storage abstraction, write vocabulary, two vtables | Done |
| In-memory reference backends (both tiers) | Done |
| libpq client (connection, prepared statements, transactions, reconnect re-prepare) | Done |
| Confirmed schema and write path | Done — verified against PostgreSQL 17 under ASan/UBSan |
| Confirmed state upsert with the slot guard | Done |
| Confirmed reorg in one transaction | Done |
| Confirmed retention prune | Done |
| ClickHouse HTTP client | Done — verified against ClickHouse 22.8 |
| `RowBinary` serialization | **Pending** |
| Finalized schema | **Pending** |
| Finalized state as `ReplacingMergeTree` | **Pending** |
| Batching writer | **Pending** |
| Bar rollup to `1d` | **Pending** |
| Promotion path (`read_range` + bulk insert) | **Pending** |
| Schema migrations, both tiers | **Pending** |
| Backpressure from the writers to the ingestion queue | **Pending** |

The confirmed tier is also **not yet wired into the live pipeline**: the
backends exist and are contract-tested, while the running indexer still
accumulates the dimensions and bars in process. Wiring is part of the promotion
and backpressure items above.

libpq is an **optional** build dependency: when it is absent the PostgreSQL
modules compile to nothing and the project still builds, with the in-memory
reference backend covering the interface.
