# Parsing specification

What this indexer reads out of the Solana block stream, and how. It is written
to be implementable in any language: nothing here depends on C, on the module
layout of this repository, or on the libraries it happens to use. Where a
concrete implementation is worth pointing at, the source file is named in
parentheses.

The companion document [STORAGE.md](STORAGE.md) describes what is done with the
result. This one stops at the point where an entity is fully derived.

Design decisions referenced as `D<n>` are in [../decisions.md](../decisions.md).

---

## 1. Scope

### 1.1 What the indexer is for

The indexer feeds a **trading terminal** (D5). That single fact decides
everything below: it derives balances, transfers, swaps, the pools and tokens
those swaps name, and the OHLCV price series built from them. It does not build
a general ledger, and it stores no transaction table.

### 1.2 The observation rule

**Only what the block stream carried is indexed.** No account is ever fetched to
complete a record, no metadata is resolved off-chain, and nothing that was not
observed exists. This is not a simplification to be lifted later — it is the
constraint that makes the throughput in §2 achievable, and several decoding
rules below exist *because* of it (mint resolution from `meta`, pool structure
learned from the first swap, token metadata known only for tokens born after
indexing started).

The consequence to internalise before implementing: a decoder never asks "what
is this account?" It asks "what does this block say about this account?"

### 1.3 What is deliberately not parsed

| Not parsed | Why |
| --- | --- |
| Vote transactions | The majority of a mainnet block, and they produce none of the entities above (§5) |
| Rewards | `showRewards=false` on the subscription; nothing consumes them |
| Account state (non-transaction) | Backlog; the stream carries transactions |
| Transaction as an entity | No ledger queries (D5); every event row carries its signature instead |
| `CloseAccount` amounts | The amount is only in `meta`, not in the instruction (§6.3) |
| Confidential transfers | The amounts are encrypted; there is nothing to record |
| Off-chain token metadata JSON | The URI is stored unresolved; fetching it is a consumer's job |

---

## 2. Input

### 2.1 Source and shape

Blocks arrive over a WebSocket `blockSubscribe` subscription at `confirmed`
commitment, and over HTTP `getBlock` for anything the socket missed (D1a). Both
deliver the same JSON block object, and the decoder cannot tell them apart.

The subscription this specification assumes:

| Option | Value | Note |
| --- | --- | --- |
| filter | `"all"` | Narrowing it creates permanent blind spots (D1a): a pool that appears tomorrow is discovered only by watching every block today |
| `encoding` | `json` | The parsed shape below. `base64` would require the full transaction wire format to be decoded by hand |
| `transactionDetails` | `full` | Without it there is no `meta` and no message, and every entity here is empty |
| `showRewards` | `false` | Nothing consumes them |
| `commitment` | `confirmed` | The finalized tier is reached by promotion, not by a second subscription (D4) |

### 2.2 Volume the parser must sustain

Measured on mainnet (D1a): 9–11 MiB per block, one block every 0.4–0.65 s,
~2600 transactions/s, ~12 MiB/s of JSON sustained. Roughly 76% of a block is
`meta` (35% token balances, 20% log messages), 23% the transaction bodies.

Two implications for an implementation in any language:

- **The JSON parser is on the critical path.** A parser that allocates a node
  per value will not keep up. The one used here measures ~1878 MiB/s in-situ,
  two orders of magnitude of headroom (D2); anything within one order is fine.
- **Storage, not parsing, is the bottleneck.** Do not optimise decoding before
  measuring the write path.

### 2.3 The JSON fields that are read

Everything else in the block object is ignored. `?` marks optional.

```
block
├── blockhash              base58 string  → 32 bytes
├── previousBlockhash?     base58 string  → 32 bytes
├── parentSlot             u64
├── blockHeight?           u64            (distinct from the slot)
├── blockTime?             i64            unix seconds, may be null
└── transactions[]
    ├── version?           "legacy" | 0   absent means legacy
    ├── transaction
    │   ├── signatures[]   base58 string  → 64 bytes each
    │   └── message
    │       ├── header
    │       │   ├── numRequiredSignatures        u8
    │       │   ├── numReadonlySignedAccounts    u8
    │       │   └── numReadonlyUnsignedAccounts  u8
    │       ├── recentBlockhash  base58 string → 32 bytes
    │       ├── accountKeys[]    base58 string → 32 bytes each
    │       └── instructions[]
    │           ├── programIdIndex  u8  index into the resolved account list
    │           ├── accounts[]      u8  indices into the same list
    │           ├── data            base58 string → opaque bytes
    │           └── stackHeight?    u16 (0 when the provider omits it)
    └── meta?
        ├── err                    null on success, an object otherwise
        ├── fee                    u64 lamports
        ├── preBalances[]          u64, one per resolved account
        ├── postBalances[]         u64, same length and order
        ├── preTokenBalances[]     sparse, see below
        ├── postTokenBalances[]    sparse, see below
        ├── innerInstructions[]
        │   ├── index              u8, the top-level instruction expanded
        │   └── instructions[]     same shape as message.instructions
        ├── loadedAddresses?       v0 only
        │   ├── writable[]         base58 string → 32 bytes
        │   └── readonly[]         base58 string → 32 bytes
        └── logMessages[]?         strings; may be absent or truncated

token balance entry
├── accountIndex     u8, into the resolved account list
├── mint             base58 string → 32 bytes
├── owner?           base58 string → 32 bytes (absent in older blocks)
├── programId?       base58 string → 32 bytes (absent in older blocks)
└── uiTokenAmount
    ├── amount       decimal string → u64   (a string because it can reach the
    │                                        top of the u64 range)
    └── decimals     u8
```

`uiTokenAmount.uiAmount` and `uiAmountString` are derived values and are not
read. Parse `amount` as an exact unsigned 64-bit integer: digits only, no sign,
no prefix, and an overflow is a parse error rather than a wrap.

---

## 3. Stage 1 — Structural decode

Turns the JSON into a typed block model. This stage interprets no program: an
instruction's `data` comes out as opaque bytes. (`src/block.c`)

### 3.1 Block header

| Field | Source | Note |
| --- | --- | --- |
| `slot` | Supplied by the caller | The block object does not carry its own slot |
| `blockhash` | `blockhash` | Required |
| `previous_blockhash` | `previousBlockhash` | Optional; absent leaves it zero |
| `parent_slot` | `parentSlot` | Required |
| `block_height` | `blockHeight` | Optional, flagged |
| `block_time` | `blockTime` | Optional and nullable, flagged. **The only timestamp the chain offers** — every transaction and every swap in a block shares it |

### 3.2 Transaction version

- absent, or the string `"legacy"` → legacy
- the number `0` → v0
- anything else → **reject the block**. Guessing at a message version this
  decoder does not model would silently mis-resolve every account index in it.

### 3.3 The resolved account list

This is the single most important structure in the decode: every instruction
addresses accounts as indices into it, and getting the order wrong silently
mis-attributes everything downstream.

Order, exactly:

1. `message.accountKeys` — the static keys
2. `meta.loadedAddresses.writable` — v0 only
3. `meta.loadedAddresses.readonly` — v0 only

A legacy transaction has only part 1. A v0 transaction with no lookup tables
also has only part 1, and its `loadedAddresses` arrays are empty rather than
absent.

Per-account flags, derived (not read) from the message header:

```
sigs         = numRequiredSignatures
ro_signed    = numReadonlySignedAccounts
ro_unsigned  = numReadonlyUnsignedAccounts
static_count = len(accountKeys)

reject unless sigs <= static_count
          and ro_signed <= sigs
          and ro_unsigned <= static_count - sigs

for i in [0, static_count):
    is_signer   = i < sigs
    is_writable = is_signer ? i < (sigs - ro_signed)
                            : i < (static_count - ro_unsigned)

loaded writable: is_signer = false, is_writable = true
loaded readonly: is_signer = false, is_writable = false
```

Every loaded address is marked `from_lookup_table`, which is what distinguishes
"the transaction named this key" from "the runtime resolved it".

### 3.4 Instructions

For each compiled instruction, top-level and inner alike:

- `programIdIndex` — **validate in range** against the resolved account list.
- `accounts[]` — each index validated in range. After this, downstream decoders
  may index without rechecking, which is what makes the per-program decoders
  in §4 simple.
- `data` — base58 **decode** to bytes. An empty string yields an empty byte
  string, not an error.
- `stackHeight` — kept when present, 0 otherwise. Not used by any rule here;
  the top-level/inner distinction comes from the structure, not from this.

Inner instructions arrive grouped by the top-level instruction they expand:
`{index, instructions[]}`. Keep the grouping — the instruction path in §9
depends on it.

An out-of-range index is a **parse error for the whole block**. It means the
decoder disagrees with a transaction the chain executed.

### 3.5 Metadata

| Field | Rule |
| --- | --- |
| `has_meta` | Whether a `meta` object was present at all. Without it, no entity in §6 is derivable |
| `success` | `meta.err` is absent or null |
| `fee` | Optional; 0 when absent |
| `pre/postBalances` | Both present or both absent. Same length, and that length **must equal the resolved account count** — a mismatch is malformed |
| `pre/postTokenBalances` | Sparse and independent; absent or empty is normal |
| `logMessages` | Absent or null when the runtime truncated them — not an error (see §7.4) |

---

## 4. Stage 2 — Program instruction decoding

Three built-in programs are decoded from their instruction bytes. They are three
different wire formats; do not assume one covers the others.

Shared rules for all three:

- **An unknown discriminant is not an error.** It is a program upgrade or an
  instruction this decoder does not model. Report "not found" and skip it.
- **A truncated payload is an error.** A field, or a length-prefixed value, that
  runs past the end of the data means the decoder disagrees with a transaction
  the chain ran.
- **Trailing bytes after a variant's fields are ignored**, because the runtime
  ignores them too. Rejecting on length would drop real events the moment a
  program appends a field.
- **Named accounts are resolved through the instruction's own account list**,
  positionally. Fewer accounts than a variant needs is a parse error.

Program ids:

| Program | Address |
| --- | --- |
| System | `11111111111111111111111111111111` (all zero bytes) |
| SPL Token | `TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA` |
| SPL Token-2022 | `TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb` |
| Vote | `Vote111111111111111111111111111111111111111` |
| Memo | `MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr` |

### 4.1 System program — bincode, u32 discriminant

Little-endian `u32` discriminant, then fields in declaration order,
little-endian and unpadded. A `String` (the seeds) is a little-endian `u64`
length followed by that many bytes. (`src/system_program.c`)

| # | Variant | Accounts read | Data after the discriminant |
| --- | --- | --- | --- |
| 0 | `CreateAccount` | 0 funder, 1 account | `lamports` u64, `space` u64, `owner` pubkey |
| 1 | `Assign` | 0 account | `owner` pubkey |
| 2 | `Transfer` | 0 from, 1 to | `lamports` u64 |
| 3 | `CreateAccountWithSeed` | 0 funder, 1 account | `base` pubkey, `seed` string, `lamports` u64, `space` u64, `owner` pubkey |
| 4 | `AdvanceNonceAccount` | 0 nonce, 2 authority (1 is the blockhashes sysvar) | — |
| 5 | `WithdrawNonceAccount` | 0 nonce, 1 to, 4 authority (2, 3 are sysvars) | `lamports` u64 |
| 6 | `InitializeNonceAccount` | 0 nonce | `new_authority` pubkey |
| 7 | `AuthorizeNonceAccount` | 0 nonce, 1 authority | `new_authority` pubkey |
| 8 | `Allocate` | 0 account | `space` u64 |
| 9 | `AllocateWithSeed` | 0 account, 1 base signer | `base` pubkey, `seed` string, `space` u64, `owner` pubkey |
| 10 | `AssignWithSeed` | 0 account, 1 base signer | `base` pubkey, `seed` string, `owner` pubkey |
| 11 | `TransferWithSeed` | 0 from, 1 base signer, 2 to | `lamports` u64, `from_seed` string, `from_owner` pubkey |
| 12 | `UpgradeNonceAccount` | 0 nonce | — |

A pubkey carried in the *data* is a value; a pubkey named as an *account* is one
the transaction touched, and therefore the only kind with a balance delta in
`meta`. Keep the distinction — §6.3 relies on it.

### 4.2 SPL Token — packed, u8 discriminant

A single-byte discriminant, then fields packed little-endian and unpadded. This
is **not** bincode. An optional pubkey (`COption<Pubkey>`) is a one-byte tag: 1
followed by 32 bytes, or 0 alone. Any other tag is malformed.
(`src/token_program.c`)

| # | Variant | Accounts read | Data after the discriminant |
| --- | --- | --- | --- |
| 0 | `InitializeMint` | 0 mint | `decimals` u8, `mint_authority` pubkey, `freeze_authority` COption |
| 1 | `InitializeAccount` | 0 account, 1 mint, 2 owner | — |
| 2 | `InitializeMultisig` | 0 multisig, 2.. signers | `m` u8 |
| 3 | `Transfer` | 0 source, 1 destination, 2 authority | `amount` u64 |
| 4 | `Approve` | 0 source, 1 delegate, 2 owner | `amount` u64 |
| 5 | `Revoke` | 0 source, 1 owner | — |
| 6 | `SetAuthority` | 0 account, 1 authority | `authority_type` u8, `new_authority` COption |
| 7 | `MintTo` | 0 mint, 1 account, 2 authority | `amount` u64 |
| 8 | `Burn` | 0 account, 1 mint, 2 authority | `amount` u64 |
| 9 | `CloseAccount` | 0 account, 1 destination, 2 owner | — |
| 10 | `FreezeAccount` | 0 account, 1 mint, 2 authority | — |
| 11 | `ThawAccount` | 0 account, 1 mint, 2 authority | — |
| 12 | `TransferChecked` | 0 source, **1 mint**, 2 destination, 3 authority | `amount` u64, `decimals` u8 |
| 13 | `ApproveChecked` | 0 source, 1 mint, 2 delegate, 3 owner | `amount` u64, `decimals` u8 |
| 14 | `MintToChecked` | 0 mint, 1 account, 2 authority | `amount` u64, `decimals` u8 |
| 15 | `BurnChecked` | 0 account, 1 mint, 2 authority | `amount` u64, `decimals` u8 |
| 16 | `InitializeAccount2` | 0 account, 1 mint | `owner` pubkey |
| 17 | `SyncNative` | 0 account | — |
| 18 | `InitializeAccount3` | 0 account, 1 mint | `owner` pubkey |
| 19 | `InitializeMultisig2` | 0 multisig, 1.. signers | `m` u8 |
| 20 | `InitializeMint2` | 0 mint | `decimals` u8, `mint_authority` pubkey, `freeze_authority` COption |
| 21 | `GetAccountDataSize` | 0 mint | (extension types, ignored) |
| 22 | `InitializeImmutableOwner` | 0 account | — |
| 23 | `AmountToUiAmount` | 0 mint | `amount` u64 |
| 24 | `UiAmountToAmount` | 0 mint | decimal text, to the end of the data |

Two shape notes that catch implementers out:

- **The checked forms insert the mint at account 1**, shifting everything after
  it. `Transfer` and `TransferChecked` are not the same account order.
- An authority may be a **multisig**, in which case its member signers follow as
  further accounts. They are not resolved: nothing derived here depends on which
  members signed.

The same decoder serves both token programs — Token-2022's base set is identical.

### 4.3 SPL Token-2022 — the base set plus extensions

Discriminants 0–24 are §4.2. Beyond that (`src/token_2022.c`):

| # | Instruction | Shape |
| --- | --- | --- |
| 25 | `InitializeMintCloseAuthority` | account 0 mint; `close_authority` COption |
| 26 | `TransferFeeExtension` | group |
| 27 | `ConfidentialTransferExtension` | group |
| 28 | `DefaultAccountStateExtension` | group |
| 29 | `Reallocate` | accounts 0 account, 1 payer, 3 owner; data is a bare sequence of `u16` extension types, no length prefix (an odd byte count is malformed) |
| 30 | `MemoTransferExtension` | group |
| 31 | `CreateNativeMint` | accounts 0 payer, 1 mint |
| 32 | `InitializeNonTransferableMint` | account 0 mint |
| 33 | `InterestBearingMintExtension` | group |
| 34 | `CpiGuardExtension` | group |
| 35 | `InitializePermanentDelegate` | account 0 mint; `delegate` pubkey |
| 36 | `TransferHookExtension` | group |
| 37 | `ConfidentialTransferFeeExtension` | group |
| 38 | `WithdrawExcessLamports` | accounts 0 source, 1 destination, 2 authority |
| 39 | `MetadataPointerExtension` | group |
| 40 | `GroupPointerExtension` | group |
| 41 | `GroupMemberPointerExtension` | group |
| 42 | `ConfidentialMintBurnExtension` | group |
| 43 | `ScaledUiAmountExtension` | group |
| 44 | `PausableExtension` | group |

An **extension group** is: the group discriminant, then a second byte selecting
the sub-instruction, then a payload whose layout is that extension's own. The
group and sub-discriminant are decoded; the payload is left as bytes (D5 — the
extension surface is larger than everything else in this stage put together).

One payload is decoded, because an entity needs it (§6.3):

**`TransferFeeExtension` / sub-discriminant 1 — `TransferCheckedWithFee`**
Accounts, in `TransferChecked` order: 0 source, 1 mint, 2 destination,
3 authority. Payload: `amount` u64, `decimals` u8, `fee` u64. `amount` is what
leaves the source *including* the fee; the destination is credited with the
difference.

Not decoded: the token **metadata and token-group interfaces**, which
Token-2022 dispatches by an eight-byte discriminator when the first byte matches
no instruction above. They come back as "not found" and are skipped.

---

## 5. Stage 3 — The vote filter

A transaction is dropped before any extraction when **it has at least one
instruction and every top-level instruction invokes the Vote program**.
(`src/vote_filter.c`)

Three details that are the rule, not an optimisation of it:

- A transaction with **no** instructions is not a vote. It carries nothing
  either, but saying otherwise makes the empty case the one shape that reaches
  the extractors classified as a vote.
- **Inner instructions are not examined.** The Vote program calls nothing, so an
  all-vote transaction has none; and a transaction that invokes something else
  fails the test on that instruction before its inner ones matter.
- The test errs conservatively **on purpose**. Failing to recognise a vote costs
  storage; mistaking anything else for a vote loses an event that cannot be
  recovered without refetching the block.

This is the single largest lever on storage volume in the design, and it costs
one program-id comparison per instruction.

---

## 6. Stage 4 — Entity extraction

Per transaction, after the vote filter. Every extractor is a pure function of
one decoded transaction; none of them looks at another transaction, another
block, or any external state.

### 6.1 SOL balances (state)

From `meta.pre/postBalances`, which covers **every** account the transaction
touched, in resolved-account-list order. (`src/balance.c`)

```
for i in [0, balance_count):
    if pre[i] == post[i]: skip
    emit { account: accounts[i], lamports: post[i], delta: post[i] - pre[i] }
```

- **Only accounts that moved are emitted.** The ones that never move are exactly
  the ones in every transaction — program ids, sysvars, accounts a swap reads
  without touching — so emitting them would rewrite the hottest keys in the
  system to say nothing happened.
- **Failed transactions are extracted.** The fee was still charged, so the
  payer's post balance is a real observation; what rolled back never reached the
  post balances.
- `delta` is signed. A magnitude beyond `int64` is a malformed block, not
  something the supply allows.

### 6.2 Token balances (state)

The two wire lists are sparse and independent, so an observation is their
**join**, keyed on `(account_index, mint)`. (`src/balance.c`)

- An account on the post side only is a **creation** (`existed_before = false`).
- An account on the pre side only is a **close** (`closed = true`, amount 0).
- An account on both sides with an unchanged amount is dropped, like SOL.
- The same `(account, mint)` listed twice on one side is malformed — the join
  would pair one account with two amounts.

Emitted per observation: the token account, its mint, its `owner` (when the
block carries one), the amount **after**, the amount **before**, `decimals`, and
the `existed_before` / `closed` flags.

Why `amount` + `previous` rather than a signed delta: a lamport balance is
bounded by a supply two orders of magnitude below `int64`, but a raw token
amount is bounded only by the `uint64` its mint's supply lives in. Mints with
more than `INT64_MAX` raw units in circulation exist, so the subtraction is left
to a consumer that knows what width it needs.

Keying the join on the mint as well as the account matters only for the exotic
close-and-recreate-as-another-mint case, where it reads correctly as one account
emptied and one account funded instead of a subtraction across two tokens.

### 6.3 Transfers (events)

One row per instruction that moved value, walked over **top-level and inner
instructions alike** — most token movement on Solana happens inside a CPI from a
venue's program, so a top-level-only walk misses the majority of it.
(`src/transfer.c`)

What produces a row:

| Kind | Source instructions |
| --- | --- |
| `sol` | System `Transfer`, `TransferWithSeed`, `CreateAccount`, `CreateAccountWithSeed`, `WithdrawNonceAccount` |
| `token` | `Transfer`, `TransferChecked` (either token program), Token-2022 `TransferCheckedWithFee` |
| `mint` | `MintTo`, `MintToChecked` |
| `burn` | `Burn`, `BurnChecked` |

Rules:

- **Mint and burn are transfers to and from the mint account itself.** The
  alternative is a wallet whose balance grows with no event to explain it, and a
  log that only sometimes reconciles is worse than one extra row shape.
- Funding a new account **is** a movement: the rent that creates a token account
  leaves a wallet and has to be accounted for.
- **A zero amount emits nothing.** No value moved.
- **A failed transaction emits nothing.** Its instructions rolled back. (The
  balance extractor does the opposite, and for the same reason: the fee really
  was charged and the rest really was not.)
- Not emitted: the transaction **fee** (a lamport movement with no instruction
  behind it, already on the transaction), and **`CloseAccount`** (its amount is
  only in `meta`, and balance-delta reasoning is assigned to the swap path).

Fields the instruction does not name are **resolved from `meta`'s token
balances** for the same accounts:

- the **mint** and **decimals** of an unchecked `Transfer`, which names neither;
- `source_owner` and `destination_owner`, the wallets behind the token accounts.

Owners are resolved **now, not left to a join**: a token account's owner can
change, and the state tier only ever holds the latest one, so an old transfer
joined against it would be attributed to whoever holds the account today.

Row identity within the transaction is `(instruction_index, inner_index, inner)`
— see §9.

---

## 7. Stage 5 — Venue (DEX) decoding

Turns a swap instruction, or the event it emitted, into what the venue *states*.
Everything a venue leaves unsaid is filled in by §8.

### 7.1 Two shapes of venue

| Shape | Read from | Venues |
| --- | --- | --- |
| Anchor program emitting a CPI event | **the event** | pump.fun curve, PumpSwap, Jupiter |
| No event | the instruction, plus balance deltas | Raydium AMM v4, Raydium CLMM |

Where an event exists it is the authority, for two reasons: it states what was
*traded* rather than what was *requested*, and it survives the account-order
changes a program upgrade brings — pump's curve already ships two account
layouts at once.

### 7.2 Anchor CPI events

An Anchor program emits an event by self-invoking with a fixed 8-byte marker:

```
IDX_ANCHOR_EVENT_DISCRIMINATOR = e4 45 a5 2e 51 cb 9a 1d   // sha256("anchor:event")[..8]
```

So an instruction whose data starts with that marker is an event. What follows
is the event's own 8-byte discriminator — `sha256("event:<Name>")[..8]` — and
then its Borsh-encoded fields.

```
instruction data = [8-byte anchor marker][8-byte event discriminator][fields...]
```

**Read events by prefix.** Take the leading fields you need and ignore the rest:
pump's `TradeEvent` has been appended to twice and ends in a Borsh string, so
anything that depends on total length is already broken.

### 7.3 Venue program ids and event discriminators

| Venue | Program id | Is a pool? |
| --- | --- | --- |
| pump.fun curve | `6EF8rrecthR5Dkzon8Nwu78hRvfCKubJ14M5uBEwF6P` | yes |
| PumpSwap (pump AMM) | `pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA` | yes |
| Raydium AMM v4 | `675kPX9MHTjS2zt1qfr1NYHuzeLXfQM9H24wFSUt1Mp8` | yes |
| Raydium CLMM | `CAMMCzo5YL8w4VFF8KVHrK22GGUsp5VTaW7grrKgrWqK` | yes |
| Raydium CPMM | `CPMMoo8L3F4NbTegBCKVNunggL7H1ZpdTHKxQB5qKP1C` | yes (no decoder yet) |
| Jupiter v6 | `JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4` | **no** — aggregator |

| Discriminator | Meaning |
| --- | --- |
| `bd db 7f d3 4e e6 61 ee` | pump curve `TradeEvent` |
| `1b 72 a9 4d de eb 63 76` | pump curve `CreateEvent` |
| `67 f4 52 1f 2c f5 77 77` | PumpSwap `BuyEvent` |
| `3e 2f 37 0a a5 03 dc 2a` | PumpSwap `SellEvent` |
| `40 c6 cd e8 26 08 71 e2` | Jupiter `SwapEvent` |
| `66 06 3d 12 01 da eb ea` | PumpSwap `buy` instruction |
| `33 e6 85 a4 01 7f 83 ad` | PumpSwap `sell` instruction |
| `c6 2e 15 52 b4 d9 e8 70` | PumpSwap `buy_exact_quote_in` instruction |
| `f8 c6 9e 91 e1 75 87 c8` | Raydium CLMM `swap` |
| `2b 04 ed 0b 1a c9 1e 62` | Raydium CLMM `swapV2` |

Wrapped SOL, used as an identity below:
`So11111111111111111111111111111111111111112`, 9 decimals.

### 7.4 pump.fun bonding curve

A meme token is born on the curve, where the counter-side is **native lamports
held by the curve itself**, and graduates to PumpSwap once the curve fills. They
are two venues, not one: different accounts, different reserves, and the price
series genuinely has a seam there.

`TradeEvent`, leading fields (`src/venue_pump.c`):

```
mint          pubkey   the token traded
sol_amount    u64      lamports moved
token_amount  u64      raw token units moved
is_buy        u8       true when the trader bought the token
user          pubkey   the trader
(timestamp, virtual/real reserves, fee breakdown, a trailing string — ignored)
```

- `is_buy` → input is SOL, output is the token; otherwise the reverse.
- The SOL side is reported as the **wrapped SOL mint** even though the curve
  moves native lamports. Nothing is wrapped by saying so; it is the identity a
  quote set can match, and D5 already treats SOL and WSOL as one quote.
- **The pool identity of a curve trade is the token mint**, since there is one
  curve per mint.

`CreateEvent`, leading fields — the source of both a pool creation and a token's
metadata:

```
name           borsh string (u32 LE length + bytes)
symbol         borsh string
uri            borsh string
mint           pubkey
bonding_curve  pubkey
user           pubkey    ← taken as the creator
(a later-appended `creator` field, timestamp, initial reserves — ignored)
```

`user` is taken as the creator rather than the appended `creator` field because
it is always present. Verified against a mainnet event (mint `…pump`, name
"United States Water Reserve", symbol "USWR").

### 7.5 PumpSwap

Amounts come from `BuyEvent` / `SellEvent`, which share a field order. Read by
fixed offset into the event fields (prefix length 240 bytes):

| Offset | Field |
| --- | --- |
| 0 | `timestamp` i64 |
| 8 | `base_amount_out` / `base_amount_in` u64 — the token side |
| 104 | `user_quote_amount_in` / `user_quote_amount_out` u64 — what the trader paid or got |
| 112 | `pool` pubkey |
| 144 | `user` pubkey |
| 176 | `user_base_token_account` pubkey |
| 208 | `user_quote_token_account` pubkey |

The **trader's** quote amount is taken, not the pool's: it is what left or
reached the wallet, it matches what an aggregator reports for the same leg, and
the fee split between the two belongs to the pool's accounting rather than to
the trade.

Neither event names the mints — base and quote are properties of the *pool*, not
of the trade — so the **instruction** supplies them. Accounts of a `buy`,
`sell` or `buy_exact_quote_in` instruction:

| Index | Account |
| --- | --- |
| 0 | pool |
| 3 | base mint |
| 4 | quote mint |
| 5 | user base token account |
| 7 | pool base vault |
| 8 | pool quote vault |

(minimum 9 accounts; the three variants share this prefix.)

The mints are read from the instruction rather than resolved from the trader's
token accounts because the quote-side account is often a wrapped-SOL account
created and closed inside the same transaction, so it never appears in the
block's token balances — while the mints, named outright as accounts, always do.

### 7.6 Raydium AMM v4

Not Anchor, no event. One-byte discriminant:

| Discriminant | Instruction | Payload |
| --- | --- | --- |
| 9 | `SwapBaseIn` | `amount_in` u64, `minimum_amount_out` u64 |
| 11 | `SwapBaseOut` | `max_amount_in` u64, `amount_out` u64 |

**Only the fixed side is recorded** — the other value is a limit the caller
asked for, not what happened.

Account layouts, addressed from both ends because what varies is in the middle
(a pool with no OpenBook market pads the serum accounts, and
`amm_target_orders` is present in one shape and absent in the other):

| Account count | Pool | Vaults | Trader's source, destination, owner |
| --- | --- | --- | --- |
| 17 | index 1 | 4, 5 | the last three |
| 18 | index 1 | 5, 6 | the last three |

**An instruction whose account count is neither 17 nor 18 is skipped**, not
guessed at: naming the wrong account as a vault would attribute someone else's
balance delta to this pool.

The vaults are ordered coin/pc, **not** input/output — §8 matches them to mints
rather than taking them by position.

**`ray_log`.** The program logs a base64 line in `meta.logMessages`:

```
Program log: ray_log: <base64>
```

Decoded, the first byte is the log type (3 = `SwapBaseIn`, 4 = `SwapBaseOut`),
followed by seven little-endian `u64`s:

```
SwapBaseIn:   amount_in, minimum_out, direction, user_source, pool_coin, pool_pc, out_amount
SwapBaseOut:  max_in,    amount_out,  direction, user_source, pool_coin, pool_pc, deduct_in
```

So `amount_in` is field 0 of one variant and field 6 of the other, and
`amount_out` is field 6 of one and field 1 of the other. `deduct_in` is what the
pool actually took, which is why it is the input rather than `max_in`.

These are ordinary program logs, so the runtime can drop them under its
cumulative log-size limit. That is rare in practice but possible, and §8 handles
it by falling through to the vault deltas.

### 7.7 Raydium CLMM

Anchor, but no trade event. `swap` and `swapV2` share the payload:

```
amount                 u64
other_amount_threshold u64   (the caller's limit — not recorded)
sqrt_price_limit_x64   u128  (skipped)
is_base_input          u8    which side `amount` is
```

Accounts:

| Index | Account |
| --- | --- |
| 0 | payer (the trader) |
| 2 | pool state |
| 3 | user input token account |
| 4 | user output token account |
| 5 | input vault |
| 6 | output vault |
| 11, 12 | input mint, output mint — **`swapV2` only** |

Minimum 7 accounts for `swap`, 13 for `swapV2`. `swapV2` is the one Raydium
shape that names its mints outright and needs nothing resolved.

Here the vaults *are* ordered input/output, which is why §8 treats CLMM
differently from AMM v4.

### 7.8 Jupiter v6 — an aggregator, not a pool

`SwapEvent`, all of it:

```
amm            pubkey   ← dropped, see below
input_mint     pubkey
input_amount   u64
output_mint    pubkey
output_amount  u64
```

Three rules follow from D8 and must be implemented together:

1. **A route never produces a pool swap row.** A routed trade appears twice in a
   block — once as Jupiter's `SwapEvent`, once as the pool's own instruction or
   event. Counting both doubles the volume of every venue Jupiter routes
   through, and since most retail flow on Solana is routed, that is most of the
   volume. The resulting bars would be wrong in a way nobody notices, because
   the *shape* of the series still looks right.
2. **`SwapEvent.amm` is dropped.** It is documented as the AMM that filled the
   leg, but for a PumpSwap leg it carries the PumpSwap *program* id — observed
   directly in a mainnet block. A pool column that is sometimes a program is
   worse than no column.
3. **A route still reaches further than the decoders do.** Its event states
   mints and exact amounts for legs through venues with no decoder here, which
   is the only place a swap on an unknown program is visible at all. That is
   what makes it worth decoding rather than skipping.

Legs are accumulated and netted into one completed-trade row — §8.5.

### 7.9 Raydium CPMM — not implemented

Recognised as a venue, but no swap decoder: no observed block so far contained
its account layout, and a layout that has not been verified against real data
does not go in. An instruction of this program currently produces nothing.

---

## 8. Stage 6 — Swap normalization

A venue decoder states part of a trade; this turns each into a complete row —
both mints, both amounts, both scales, the pool and the trader — resolving the
rest against what the block already carried, and **fetching nothing**.
(`src/swap.c`)

### 8.1 Attribution is per invocation

Each swap instruction names its own accounts, so a route through three pools
yields three rows, one per pool, each resolved against that pool's own accounts.
This is what lets per-transaction vault deltas still attribute correctly when a
transaction touches several pools — as long as no single pool is touched twice,
which is exactly the case the per-invocation sources below cover.

### 8.2 Amount sources, ranked (D9)

For each swap, take the amounts from the highest source available:

1. **The program's own event.** Exact amounts the program computed, both sides,
   per invocation, immune to account-order drift. Nothing beats it.
2. **Raydium's `ray_log`.** Both sides, per invocation. Measured to agree to the
   unit with the vault deltas.
3. **Vault balance deltas.** The change in the pool's own vault across the
   transaction — the last resort.

Why the ranking and not deltas everywhere: vault deltas are per *transaction*,
not per instruction, so they cannot split two swaps that touch the same pool in
one transaction — the arbitrage case, where a bot buys and sells the same pool
atomically and the vault nets the two. Deltas are also silent about a swap that
moved a mint the transaction touched elsewhere.

The source is recorded on the row (`event` / `raylog` / `delta`), because "how
do we know?" is worth being able to ask of a price that looks wrong.

### 8.3 Mint and decimals resolution

- **From a token account:** find that account in `meta.post/preTokenBalances`
  (post first — it is the transaction's own final view; pre answers for an
  account the transaction closed). The entry states the mint and its decimals.
- **From a mint directly:** scan the same lists for any entry naming it and take
  its decimals.
- **Wrapped SOL is known without a balance:** 9 decimals, hard-coded. The
  pump curve's native-SOL side depends on this, since a curve trade often has no
  wrapped-SOL token account anywhere in the transaction.

A side that cannot be resolved leaves its flag unset rather than guessing. A row
that names one mint and one amount is still worth keeping; §9 decides what can
be done with a partial one.

### 8.4 Per-venue normalization

| Venue | Amounts | Mints | Pool |
| --- | --- | --- | --- |
| pump curve | from `TradeEvent`, both sides | stated by the event (token + WSOL) | **the token mint** |
| PumpSwap | from `Buy`/`SellEvent`, both sides | from the paired swap *instruction*'s base/quote mint accounts; the side that is the trader's base account decides which is input | from the event |
| Raydium AMM v4 | `ray_log` if the pairing checks out, else vault deltas | from the trader's source/destination token accounts | account 1 |
| Raydium CLMM / CPMM | the fixed side from the instruction, the other from the matching vault's delta | `swapV2` states them; otherwise from the trader's accounts | account 2 |

**PumpSwap event/instruction pairing.** The event carries the amounts, the
instruction carries the mints and vaults; the two are paired **on the pool
address**, searching the transaction's top-level and inner instructions. This is
unambiguous even when the same pool is traded twice in one transaction, since
the mints are identical either way. If no instruction can be paired (a truncated
inner list), fall back to resolving the trader's two token accounts — which
recovers whichever side is not a temporary wrapped-SOL account.

**Raydium v4 `ray_log` pairing.** The lines carry no instruction index, so the
k-th swap log pairs with the k-th v4 invocation in execution order. Cross-check
it: the side the instruction fixed must match the same side in the log. If it
disagrees, distrust the pairing and fall through to the vault deltas. If the
instruction fixed nothing (amount 0), accept the log.

**Vault matching.** For AMM v4 the vaults are matched to the input and output
**mints** (find the vault whose token balance names that mint, take the
magnitude of its delta), because v4 orders them coin/pc. For CLMM they are taken
**by position**: vault A is the input, vault B the output.

### 8.5 Jupiter route netting (D8)

A route is recorded as **one completed trade**: the wallet paid A and received
D. The endpoints are not read off the first and last leg, because a route can
fan out — A into B over two pools in parallel, then B into C — and then there is
no single first leg.

```
for each SwapEvent leg:
    net[input_mint].in  += input_amount
    net[output_mint].out += output_amount

input  = the mint with the largest positive (in - out)     ← what the wallet paid
output = the mint with the largest positive (out - in)     ← what it received
```

Intermediate mints cancel to zero. Verified on a two-leg mainnet route:
`D9cC…pump -634524165458`, `WSOL 0`, `USDC +207536010`, with the intermediate
leg boundary matching to the unit.

Edge cases, accepted rather than solved:

- **A cyclic route** (starts and ends in the same mint) nets everything to about
  zero and has no endpoints in this sense. It is a self-trade, not a user swap,
  and yields **no row**.
- **A platform fee taken on the output after the last swap** is not in the
  events, so the route's D is what the pools delivered, not necessarily what net
  of fee reached the wallet. That is the right number for a price between
  endpoints, which is the use this serves.
- More distinct mints than the netting table holds (24 here) → give up and emit
  no aggregated row, rather than grow unboundedly on hostile input.

The trader on the aggregated row is the transaction's fee payer (resolved
account 0). The row is emitted **last**, after every pool row.

### 8.6 Preconditions

The whole stage yields nothing for a transaction that **failed** or that has
**no `meta`**: without the token balances there is nothing to resolve against,
and a rolled-back swap did not happen.

---

## 9. Stage 7 — Pricing

A raw amount is a count of base units and says nothing about value. A price
appears only when one side is a mint whose value is already understood — a
**quote**. (`src/price.c`)

### 9.1 The quote set

An **ordered** set; order is priority. Default, highest first:

| Rank | Quote | Mint |
| --- | --- | --- |
| 0 | USDC | `EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v` |
| 1 | USDT | `Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB` |
| 2 | USD1 | `USD1ttGY1N17NEEHLmELoaybftRBUSErhqYiQzvEmuB` |
| 3 | SOL / WSOL | `So11111111111111111111111111111111111111112` |

Configurable as a comma-separated list of well-known names (`sol`/`wsol`,
`usdc`, `usdt`, `usd1`) or base58 mint addresses, in priority order. A repeated
mint keeps its first, highest-priority position. An empty list disables pricing.

### 9.2 The rule

- **One side is a quote** → that side is the quote, the other is the base.
- **Both sides are quotes** (a SOL/USDC pool) → the higher-priority one wins, so
  the base is the lower-priority side. With the default order, such a pool
  reports SOL's price in dollars rather than dollars priced in SOL.
- **Neither side is a quote** → the swap is still recorded, it simply has no
  price.

```
price = (quote_raw / 10^quote_decimals) / (base_raw / 10^base_decimals)
```

— quote units per **one whole base unit**.

### 9.3 Priced, and priced with a number

These are distinct states and both are worth keeping:

- `priced` — one side is a quote mint.
- `has_price` — a number could actually be computed, which additionally needs
  both mints, both amounts, both decimals, and a non-zero base amount.

A quote side whose decimals the block never carried leaves a row *priced but
numberless*, which is a different fact from a swap with no quote side at all.

Amounts stay **raw** on the row and both decimals ride along; scaling happens
here and nowhere earlier. The price is a `double`: the dynamic range runs from a
memecoin at 1e-9 SOL to a stablecoin at ~1, and ~15–16 significant digits cover
it with room to spare. It is a convenience, not the system of record — the raw
amounts and decimals stay on the row so a consumer can recompute exactly.

Aggregated route rows are priced too, on their netted endpoints.

---

## 10. Stage 8 — Dimensions

Two registries that **accumulate across blocks**, unlike everything above.

### 10.1 Pools (`src/pool.c`)

Keyed by pool address.

- **Structure is learned from the first swap observed.** A swap names the pool,
  its two mints and their decimals, which is the whole of what a pool dimension
  needs. A pool never seen trading is a pool with no price series, so it is not
  a row at all.
- The two mints are an **unordered pair**: a swap's input and output depend on
  direction, so a mint is matched to a slot by identity, not position, and
  either slot may fill first. Later swaps fill whatever is still unknown.
- **A creation only enriches a record that already exists.** It adds what a swap
  cannot state — who created the pool — and never creates the record. A creation
  seen for a pool that has not traded is dropped and counted. In practice a pool
  is created and first traded in the same transaction, so the swap registers it
  and the creation enriches it in one pass; run the swap observation first.
- Tracked per pool: `first_seen_slot`, `swap_count`, and the creation slot and
  creator when observed.

Creation decoding currently covers the pump.fun curve `CreateEvent` only — by
far the most pool births — read from the event for the same reason trades are.
The other venues register from their swaps but are not creation-enriched.

### 10.2 Tokens (`src/token.c`)

Keyed by mint address.

- **Address and decimals are free**: every token balance in `meta` carries both,
  so a token is registered the moment any account holding it moves.
- **Name, symbol and URI cost an observation.** They come from a metadata
  instruction — the pump.fun `CreateEvent` here; the general case is the
  Metaplex Token Metadata program, or the Token-2022 `TokenMetadata` extension.
  So they are known only for tokens whose metadata instruction was observed, in
  practice tokens born after indexing started.
- **The URI is stored unresolved.** It points at JSON on Arweave or IPFS;
  resolving it is an HTTP fetch against something that is not the chain, and it
  belongs to a consumer.
- The first metadata seen wins; later ones do not overwrite. A metadata
  observation may create the record — a token is a token whether or not a
  balance has been seen.

Metaplex Token Metadata is **not decoded yet**: recent mainnet blocks scanned
while building this carried no `CreateMetadataAccountV3` to verify the layout
against, and it will not go in from an IDL alone.

---

## 11. Stage 9 — Bars (OHLCV)

For one pool and one time bucket: open, high, low, close and volume.
(`src/bar.c`)

### 11.1 Shape

- Keyed `(pool, interval, bucket)`. **Never aggregated across pools.** The same
  pair trades in many pools, and a merged series is worse than any of its
  inputs: minor pools carry mostly arbitrage flow, and their prints move the
  aggregate without anything having happened in the market a user is watching.
  This was tried and it does not hold up.
- Two resolutions stored: **1s** and **1m**. Every coarser interval a terminal
  offers is built from those on read.
- `bucket = floor(block_time / width) * width`, in unix seconds. `block_time` is
  the block's, since it is the only timestamp the chain offers.
- Inputs: **priced pool swaps only.** A route is not a pool (D8) and contributes
  nothing; a swap with no price or no block time contributes nothing.
- Volume is carried on **both sides**, scaled by their decimals:
  `base_volume += base_raw / 10^base_dec`, likewise for quote.

### 11.2 Open and close under out-of-order arrival

Blocks can commit out of order — a backfilled slot arrives after a later one —
so open and close follow **execution order, not arrival order**. Each swap
carries a sequence:

```
seq = (slot, transaction_index, instruction_index, inner, inner_index)
```

compared lexicographically, with a **top-level instruction ordering before the
inner ones it expands into**. The earliest seq in a bucket sets the open, the
latest sets the close, whichever arrived first.

That also makes a bar a **pure fold of its swaps**, which is what the reorg path
below relies on.

### 11.3 Recomputation after a reorg

```
affected = every bar whose close_seq.slot >= from_slot
           (a bar's close is its latest swap, and seq orders by slot first,
            so this is exactly the set holding a reorged swap)
clear affected
re-fold the surviving swaps, but only into buckets in `affected`
```

Only cleared buckets are rebuilt, so a survivor in an untouched bucket is not
double-counted, and a bucket whose swaps were all reorged away stays gone. It is
exact precisely because a bar is a pure fold.

---

## 12. Identity and ordering

### 12.1 The instruction path

```
(slot, transaction_index, instruction_index, inner_index)
```

It identifies an event uniquely, leads with the slot so a reorg delete is a
range operation, and lets any derived row be traced back to the instruction that
produced it. `inner_index` is 0 for a top-level instruction.

An extractor produces the last three components; the caller, which is walking a
block, supplies the slot and transaction index — repeating them on every row of
every transaction would cost more than it explains. The transaction **signature**
travels with them, since no transaction table exists to join against.

### 12.2 The bar sequence key

For a storage tier that must compare sequences in SQL, pack it big-endian so a
bytewise comparison reproduces §11.2 exactly — 15 bytes:

```
[slot u64 BE][transaction_index u16 BE][instruction_index u16 BE][inner u8][inner_index u16 BE]
```

`inner` is 0 for top-level and 1 for inner, which places top-level first.

---

## 13. Error handling contract

The distinction that matters most: **an unknown thing is not a broken thing.**

| Situation | Response |
| --- | --- |
| Unknown program | Skip. The normal case — most programs are not modelled |
| Known program, unknown discriminant | Skip. A program upgrade, not bad data |
| Known program, known variant, **truncated payload** | Error. The decoder disagrees with a transaction the chain ran — a bug here |
| Known variant naming **fewer accounts** than it operates on | Error, same reason |
| Account index out of range | Error — the block is malformed |
| Unsupported message version | Error — never guess |
| Balance array length ≠ account count | Error |
| Same `(account, mint)` twice on one side of the token balances | Error — the join is ambiguous |
| `meta` absent | Not an error. Yields no entities |
| Logs absent or truncated | Not an error. Fall back per §8.4 |
| `blockTime` absent | Not an error. No bars from that block |
| Venue instruction with an unrecognised account-count shape | Skip — naming the wrong vault is worse than missing a swap |

Trailing bytes after a variant's fields are always ignored (§4).

---

## 14. Determinism

Every stage from §3 to §11 is a pure function of the block plus, for the
registries and bars, previously observed blocks. Given the same block stream in
any order, the derived entities are identical, with two consequences worth
stating explicitly:

- **Re-indexing a slot converges.** The same block yields the same rows with the
  same keys, so a re-index is an idempotent overwrite rather than a duplicate.
- **Arrival order does not affect content.** Only the registries and bars are
  order-sensitive at all, and both are written to be order-independent (§10
  fills unknown fields, §11 orders by execution sequence).

---

## 15. Verification

The layouts above were verified against **real mainnet blocks**, not against
IDLs. `tests/golden/golden_block.json` holds seven real transactions from slot
435146411 — a pump.fun create-and-buy, two more pump curve trades, two PumpSwap
and two Raydium AMM v4 — and the golden test runs them through the entire path
(decode → filter → extract → normalize → price → registries → bars), checking
the result against facts read off the chain independently: the token's mint,
decimals, name, symbol and creator, the exact price of its first trade, and the
aggregate entity counts.

An implementation in another language should reuse that fixture, or build the
equivalent. Two specific things it catches that unit tests do not:

- an event layout read from an IDL that does not match the bytes on chain;
- an account-order assumption that holds for one program version and not the
  next.

---

## 16. Implementation status

| Stage | Status |
| --- | --- |
| §3 Structural decode | Complete |
| §4.1 System | Complete |
| §4.2 SPL Token | Complete |
| §4.3 Token-2022 | Base set and extension identification complete; only the `TransferCheckedWithFee` payload is decoded; metadata/group interfaces not decoded |
| §5 Vote filter | Complete |
| §6 Balances, transfers | Complete |
| §7 Venues | pump curve, PumpSwap, Raydium AMM v4, Raydium CLMM, Jupiter v6 complete. **Raydium CPMM has no decoder** |
| §7 Creations | pump.fun curve only |
| §7 Token metadata | pump.fun `CreateEvent` only; **Metaplex Token Metadata not decoded** |
| §8 Normalization | Complete, including route netting |
| §9 Pricing | Complete |
| §10 Dimensions | Complete (in-process registries) |
| §11 Bars | Complete, including reorg recomputation |

The gaps are deliberate and all have the same cause: no layout goes in until it
has been verified against a real block.
