/*
 * Solana value types.
 *
 * Pubkeys, signatures and hashes are fixed-size byte arrays wrapped in structs
 * rather than bare arrays, so they can be returned by value, compared, and —
 * most importantly — not silently confused with one another by the compiler.
 */
#ifndef IDX_TYPES_H
#define IDX_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bytes.h"
#include "error.h"

#define IDX_PUBKEY_LEN 32
#define IDX_SIGNATURE_LEN 64
#define IDX_HASH_LEN 32

/* Buffer sizes for base58 text, including the NUL terminator. */
#define IDX_PUBKEY_STR_MAX 45
#define IDX_SIGNATURE_STR_MAX 89
#define IDX_HASH_STR_MAX 45

typedef struct {
    uint8_t bytes[IDX_PUBKEY_LEN];
} idx_pubkey;

typedef struct {
    uint8_t bytes[IDX_SIGNATURE_LEN];
} idx_signature;

typedef struct {
    uint8_t bytes[IDX_HASH_LEN];
} idx_hash;

/* A slot number. Distinct from a block height, which is not the same thing. */
typedef uint64_t idx_slot;

/* ---------------------------------------------------------------- pubkey -- */

extern const idx_pubkey IDX_PUBKEY_DEFAULT; /* all zeros, the system program */

idx_pubkey idx_pubkey_from_bytes(const uint8_t bytes[IDX_PUBKEY_LEN]);
bool idx_pubkey_equal(const idx_pubkey *a, const idx_pubkey *b);
bool idx_pubkey_is_default(const idx_pubkey *key);
idx_slice idx_pubkey_slice(const idx_pubkey *key);

idx_status idx_pubkey_from_base58(const char *text, size_t text_len,
                                  idx_pubkey *out, idx_error *err);
idx_status idx_pubkey_to_base58(const idx_pubkey *key,
                                char out[IDX_PUBKEY_STR_MAX], idx_error *err);

/* ------------------------------------------------------------- signature -- */

idx_status idx_signature_from_base58(const char *text, size_t text_len,
                                     idx_signature *out, idx_error *err);
idx_status idx_signature_to_base58(const idx_signature *signature,
                                   char out[IDX_SIGNATURE_STR_MAX],
                                   idx_error *err);
bool idx_signature_equal(const idx_signature *a, const idx_signature *b);

/* ------------------------------------------------------------------ hash -- */

idx_status idx_hash_from_base58(const char *text, size_t text_len,
                                idx_hash *out, idx_error *err);
idx_status idx_hash_to_base58(const idx_hash *hash, char out[IDX_HASH_STR_MAX],
                              idx_error *err);
bool idx_hash_equal(const idx_hash *a, const idx_hash *b);

/* --------------------------------------------------- well-known programs -- */

extern const idx_pubkey IDX_PROGRAM_SYSTEM;
extern const idx_pubkey IDX_PROGRAM_TOKEN;
extern const idx_pubkey IDX_PROGRAM_TOKEN_2022;
extern const idx_pubkey IDX_PROGRAM_MEMO;

#endif /* IDX_TYPES_H */
