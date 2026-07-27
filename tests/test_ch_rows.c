/*
 * ClickHouse row serialization tests.
 *
 * Offline: the exact bytes each type produces in RowBinary — including the
 * LEB128 length boundaries and the Nullable rule that a null writes its flag
 * and no value — the JSON escaping rule that keeps binary keys intact, and the
 * writer's own guards (row width, open row, argument checks).
 *
 * Online (only when IDX_TEST_CH_URL points at a server): the same rows are
 * inserted in both formats and the two tables are compared with EXCEPT in both
 * directions. That is the assertion that matters — not that the bytes match
 * what this test expects, but that ClickHouse decodes them into the values the
 * writer was given, and that the debugging format is not quietly a different
 * one.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"
#include "ch_rows.h"
#include "test.h"

/* Compares the writer's body against `expected`, byte for byte. */
static void check_body(const idx_ch_rows *rows, const void *expected,
                       size_t len) {
    idx_slice body = idx_ch_rows_body(rows);
    TEST_EQ_UINT(body.len, len);
    if (body.len != len) {
        return;
    }
    TEST_ASSERT(memcmp(body.data, expected, len) == 0);
}

static void check_text(const idx_ch_rows *rows, const char *expected) {
    check_body(rows, expected, strlen(expected));
}

/* ------------------------------------------------------------- RowBinary -- */

static void test_binary_integers(void) {
    idx_ch_rows w;
    idx_ch_rows_init(&w, IDX_CH_FORMAT_ROW_BINARY);
    idx_error err;
    idx_error_clear(&err);

    TEST_EQ_INT(idx_ch_rows_begin(&w, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_u8(&w, "a", 0x01, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_u16(&w, "b", 0x0102, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_u32(&w, "c", 0x01020304u, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_u64(&w, "d", UINT64_MAX, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_i8(&w, "e", -1, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_i16(&w, "f", -2, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_i32(&w, "g", -3, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_i64(&w, "h", INT64_MIN, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_bool(&w, "i", true, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_end(&w, &err), IDX_OK);

    /* Little-endian throughout; the signed ones are two's complement. */
    static const uint8_t expected[] = {
        0x01,                                            /* u8  */
        0x02, 0x01,                                      /* u16 */
        0x04, 0x03, 0x02, 0x01,                          /* u32 */
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  /* u64 max */
        0xff,                                            /* i8  -1 */
        0xfe, 0xff,                                      /* i16 -2 */
        0xfd, 0xff, 0xff, 0xff,                          /* i32 -3 */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,  /* i64 min */
        0x01,                                            /* bool true */
    };
    check_body(&w, expected, sizeof expected);
    TEST_EQ_UINT(idx_ch_rows_count(&w), 1);
    TEST_EQ_UINT(idx_ch_rows_size(&w), sizeof expected);
    idx_ch_rows_free(&w);
}

static void test_binary_float(void) {
    idx_ch_rows w;
    idx_ch_rows_init(&w, IDX_CH_FORMAT_ROW_BINARY);

    TEST_EQ_INT(idx_ch_rows_begin(&w, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_f64(&w, "p", 1.0, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_end(&w, NULL), IDX_OK);
    /* IEEE 754 1.0, little-endian. */
    static const uint8_t one[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0,
                                  0x3f};
    check_body(&w, one, sizeof one);
    idx_ch_rows_free(&w);
}

static void test_binary_strings(void) {
    idx_ch_rows w;
    idx_ch_rows_init(&w, IDX_CH_FORMAT_ROW_BINARY);

    /* A String is a varint length then the bytes; a FixedString is the bytes
     * alone. */
    TEST_EQ_INT(idx_ch_rows_begin(&w, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_str(&w, "s", "hi", 2, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_fixed(&w, "k", "\xde\xad\xbe\xef", 4, NULL),
                IDX_OK);
    TEST_EQ_INT(idx_ch_rows_end(&w, NULL), IDX_OK);
    static const uint8_t expected[] = {0x02, 'h',  'i',  0xde,
                                       0xad, 0xbe, 0xef};
    check_body(&w, expected, sizeof expected);

    /* An empty String is a lone zero length, and NULL data is allowed only
     * there. */
    idx_ch_rows_reset(&w);
    TEST_EQ_INT(idx_ch_rows_begin(&w, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_str(&w, "s", NULL, 0, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_end(&w, NULL), IDX_OK);
    check_body(&w, "\x00", 1);

    idx_ch_rows_free(&w);
}

/* The LEB128 boundaries: one byte to 127, two from 128. */
static void test_binary_varint(void) {
    idx_ch_rows w;
    idx_ch_rows_init(&w, IDX_CH_FORMAT_ROW_BINARY);
    char payload[300];
    memset(payload, 'x', sizeof payload);

    const struct {
        size_t len;
        const char *prefix;
        size_t prefix_len;
    } cases[] = {
        {0, "\x00", 1},
        {1, "\x01", 1},
        {127, "\x7f", 1},
        {128, "\x80\x01", 2},
        {300, "\xac\x02", 2},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        idx_ch_rows_reset(&w);
        TEST_EQ_INT(idx_ch_rows_begin(&w, NULL), IDX_OK);
        TEST_EQ_INT(idx_ch_rows_str(&w, "s", payload, cases[i].len, NULL),
                    IDX_OK);
        TEST_EQ_INT(idx_ch_rows_end(&w, NULL), IDX_OK);
        idx_slice body = idx_ch_rows_body(&w);
        TEST_EQ_UINT(body.len, cases[i].prefix_len + cases[i].len);
        TEST_ASSERT(memcmp(body.data, cases[i].prefix, cases[i].prefix_len) ==
                    0);
    }
    idx_ch_rows_free(&w);
}

/*
 * The Nullable rule verified against the server in probing this format: the
 * flag byte alone for a null, flag plus value for a present one. Writing the
 * value anyway would shift every following column.
 */
static void test_binary_nullable(void) {
    idx_ch_rows w;
    idx_ch_rows_init(&w, IDX_CH_FORMAT_ROW_BINARY);

    TEST_EQ_INT(idx_ch_rows_begin(&w, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_nullable_i64(&w, "a", 7, true, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_nullable_i64(&w, "b", 7, false, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_nullable_u8(&w, "c", 9, false, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_nullable_fixed(&w, "d", "\x01\x02", 2, false, NULL),
                IDX_OK);
    TEST_EQ_INT(idx_ch_rows_nullable_str(&w, "e", "zz", 2, true, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_end(&w, NULL), IDX_OK);

    static const uint8_t expected[] = {
        0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 7 */
        0x01,                                                 /* null */
        0x01,                                                 /* null */
        0x01,                                                 /* null */
        0x00, 0x02, 'z', 'z',                                 /* "zz" */
    };
    check_body(&w, expected, sizeof expected);
    idx_ch_rows_free(&w);
}

/* ----------------------------------------------------------- JSONEachRow -- */

static void test_json_row(void) {
    idx_ch_rows w;
    idx_ch_rows_init(&w, IDX_CH_FORMAT_JSON_EACH_ROW);

    TEST_EQ_INT(idx_ch_rows_begin(&w, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_u64(&w, "slot", UINT64_MAX, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_i64(&w, "delta", -5, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_bool(&w, "closed", false, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_nullable_i64(&w, "block_time", 0, false, NULL),
                IDX_OK);
    TEST_EQ_INT(idx_ch_rows_str(&w, "symbol", "USWR", 4, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_end(&w, NULL), IDX_OK);

    /* A uint64 past 2^53 goes out unquoted and exact: ClickHouse parses the
     * text, so there is no double to round it. */
    check_text(&w,
               "{\"slot\":18446744073709551615,\"delta\":-5,\"closed\":0,"
               "\"block_time\":null,\"symbol\":\"USWR\"}\n");
    TEST_EQ_UINT(idx_ch_rows_count(&w), 1);
    idx_ch_rows_free(&w);
}

/*
 * The escaping rule. Only what JSON requires is escaped, and only below 0x20:
 * a byte at 0x80 or above must travel raw, because ClickHouse reads an
 * escape as a codepoint and would deliver two UTF-8 bytes, overflowing a
 * FixedString.
 */
static void test_json_escaping(void) {
    idx_ch_rows w;
    idx_ch_rows_init(&w, IDX_CH_FORMAT_JSON_EACH_ROW);

    static const uint8_t bytes[] = {'"', '\\', '\n', '\t', 0x01, 0x1f, 0x80,
                                    0xff};
    TEST_EQ_INT(idx_ch_rows_begin(&w, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_fixed(&w, "k", bytes, sizeof bytes, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_end(&w, NULL), IDX_OK);

    check_text(&w, "{\"k\":\"\\\"\\\\\\n\\t\\u0001\\u001F\x80\xff\"}\n");
    idx_ch_rows_free(&w);
}

/*
 * A non-finite double is quoted in JSON. ClickHouse accepts "inf" and "nan" as
 * strings and rejects them bare, so an unquoted one would produce a body the
 * server refuses. Neither is expected on a price; this is about failing
 * visibly rather than at insert time.
 */
static void test_json_non_finite(void) {
    idx_ch_rows w;
    idx_ch_rows_init(&w, IDX_CH_FORMAT_JSON_EACH_ROW);

    TEST_EQ_INT(idx_ch_rows_begin(&w, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_f64(&w, "p", INFINITY, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_nullable_f64(&w, "q", -INFINITY, true, NULL),
                IDX_OK);
    TEST_EQ_INT(idx_ch_rows_end(&w, NULL), IDX_OK);
    check_text(&w, "{\"p\":\"inf\",\"q\":\"-inf\"}\n");

    /* A finite one is bare, and round-trips through %.17g. */
    idx_ch_rows_reset(&w);
    TEST_EQ_INT(idx_ch_rows_begin(&w, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_f64(&w, "p", 0.5, NULL), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_end(&w, NULL), IDX_OK);
    check_text(&w, "{\"p\":0.5}\n");
    idx_ch_rows_free(&w);
}

/* ---------------------------------------------------------------- guards -- */

static void test_guards(void) {
    idx_ch_rows w;
    idx_ch_rows_init(&w, IDX_CH_FORMAT_ROW_BINARY);
    idx_error err;
    idx_error_clear(&err);

    /* A column outside a row, and a second begin inside one. */
    TEST_EQ_INT(idx_ch_rows_u8(&w, "a", 1, &err), IDX_ERR_INTERNAL);
    TEST_EQ_INT(idx_ch_rows_end(&w, &err), IDX_ERR_INTERNAL);
    TEST_EQ_INT(idx_ch_rows_begin(&w, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_begin(&w, &err), IDX_ERR_INTERNAL);

    /* A body never contains a half-written row. */
    TEST_EQ_INT(idx_ch_rows_u8(&w, "a", 1, &err), IDX_OK);
    TEST_EQ_UINT(idx_ch_rows_body(&w).len, 0);
    TEST_EQ_UINT(idx_ch_rows_size(&w), 0);
    TEST_EQ_INT(idx_ch_rows_end(&w, &err), IDX_OK);
    TEST_EQ_UINT(idx_ch_rows_body(&w).len, 1);

    /* A row of a different width than the first is refused, which is a schema
     * bug RowBinary cannot otherwise express. */
    TEST_EQ_INT(idx_ch_rows_begin(&w, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_u8(&w, "a", 1, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_u8(&w, "b", 2, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_end(&w, &err), IDX_ERR_INTERNAL);

    /* Argument checks. */
    idx_ch_rows_reset(&w);
    TEST_EQ_INT(idx_ch_rows_begin(&w, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_u8(&w, NULL, 1, &err), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_ch_rows_str(&w, "s", NULL, 4, &err), IDX_ERR_INVALID_ARG);
    /* A rejected column left nothing behind: the row is still empty. */
    TEST_EQ_UINT(w.column_count, 0);
    TEST_EQ_INT(idx_ch_rows_u8(NULL, "a", 1, &err), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_ch_rows_begin(NULL, &err), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_ch_rows_end(NULL, &err), IDX_ERR_INVALID_ARG);

    /* Reset drops the rows and the expected width but keeps the writer
     * usable. */
    idx_ch_rows_reset(&w);
    TEST_EQ_UINT(idx_ch_rows_count(&w), 0);
    TEST_EQ_UINT(idx_ch_rows_size(&w), 0);
    TEST_EQ_INT(idx_ch_rows_begin(&w, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_u8(&w, "a", 1, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_u8(&w, "b", 2, &err), IDX_OK);
    TEST_EQ_INT(idx_ch_rows_end(&w, &err), IDX_OK);

    idx_ch_rows_free(&w);

    /* NULL is safe everywhere it can be. */
    idx_ch_rows_free(NULL);
    idx_ch_rows_reset(NULL);
    TEST_EQ_UINT(idx_ch_rows_count(NULL), 0);
    TEST_EQ_UINT(idx_ch_rows_size(NULL), 0);
    TEST_EQ_UINT(idx_ch_rows_body(NULL).len, 0);
}

static void test_format_name(void) {
    TEST_EQ_STR(idx_ch_row_format_name(IDX_CH_FORMAT_ROW_BINARY), "RowBinary");
    TEST_EQ_STR(idx_ch_row_format_name(IDX_CH_FORMAT_JSON_EACH_ROW),
                "JSONEachRow");
}

/* ---------------------------------------------------------------- online -- */

/* Every column type the finalized schema will need, in one table. */
static const char *const g_columns =
    "(u8v UInt8, u16v UInt16, u32v UInt32, u64v UInt64, "
    "i8v Int8, i16v Int16, i32v Int32, i64v Int64, "
    "f64v Float64, boolv UInt8, strv String, fixedv FixedString(32), "
    "nu8 Nullable(UInt8), nu64 Nullable(UInt64), ni64 Nullable(Int64), "
    "nf64 Nullable(Float64), nstr Nullable(String), "
    "nfixed Nullable(FixedString(32))) ENGINE = MergeTree ORDER BY u64v";

/*
 * Two rows exercising the edges: one with every nullable present and the
 * extreme integer values, one with every nullable absent. The FixedString is 32
 * bytes of high values, which is what a pubkey looks like and what the JSON
 * escaping rule exists for.
 */
static idx_status write_rows(idx_ch_rows *w, idx_error *err) {
    uint8_t key[32], key2[32];
    for (size_t i = 0; i < sizeof key; i++) {
        key[i] = (uint8_t)(0x80 + i);
        key2[i] = (uint8_t)i;
    }

    for (int present = 1; present >= 0; present--) {
        bool p = present != 0;
        IDX_TRY(idx_ch_rows_begin(w, err));
        IDX_TRY(idx_ch_rows_u8(w, "u8v", 255, err));
        IDX_TRY(idx_ch_rows_u16(w, "u16v", 65535, err));
        IDX_TRY(idx_ch_rows_u32(w, "u32v", 4294967295u, err));
        IDX_TRY(idx_ch_rows_u64(w, "u64v", p ? UINT64_MAX : 1, err));
        IDX_TRY(idx_ch_rows_i8(w, "i8v", -128, err));
        IDX_TRY(idx_ch_rows_i16(w, "i16v", -32768, err));
        IDX_TRY(idx_ch_rows_i32(w, "i32v", -2147483647 - 1, err));
        IDX_TRY(idx_ch_rows_i64(w, "i64v", INT64_MIN, err));
        IDX_TRY(idx_ch_rows_f64(w, "f64v", 1.7976931348623157e308, err));
        IDX_TRY(idx_ch_rows_bool(w, "boolv", p, err));
        IDX_TRY(idx_ch_rows_str(w, "strv", "a\"b\\c\nd", 7, err));
        IDX_TRY(idx_ch_rows_fixed(w, "fixedv", p ? key : key2, 32, err));
        IDX_TRY(idx_ch_rows_nullable_u8(w, "nu8", 7, p, err));
        IDX_TRY(idx_ch_rows_nullable_u64(w, "nu64", UINT64_MAX, p, err));
        IDX_TRY(idx_ch_rows_nullable_i64(w, "ni64", INT64_MIN, p, err));
        IDX_TRY(idx_ch_rows_nullable_f64(w, "nf64", -0.5, p, err));
        IDX_TRY(idx_ch_rows_nullable_str(w, "nstr", "x\x01y", 3, p, err));
        IDX_TRY(idx_ch_rows_nullable_fixed(w, "nfixed", key, 32, p, err));
        IDX_TRY(idx_ch_rows_end(w, err));
    }
    return IDX_OK;
}

static long long scalar(idx_ch_conn *c, const char *sql) {
    idx_ch_result *r = NULL;
    if (idx_ch_query(c, sql, &r, NULL) != IDX_OK) {
        return -1;
    }
    const char *body = idx_ch_result_body(r);
    long long v = body != NULL ? atoll(body) : -1;
    idx_ch_result_free(r);
    return v;
}

/* Inserts `w`'s body into `table` in the writer's own format. */
static void insert_rows(idx_ch_conn *conn, const char *table, idx_ch_rows *w) {
    char sql[128];
    snprintf(sql, sizeof sql, "INSERT INTO %s FORMAT %s", table,
             idx_ch_row_format_name(w->format));
    idx_slice body = idx_ch_rows_body(w);
    idx_error err;
    idx_error_clear(&err);
    idx_status st = idx_ch_insert(conn, sql, body.data, body.len, &err);
    TEST_CHECK(st == IDX_OK, "%s insert: %s", table, err.message);
}

static void test_online(void) {
    idx_ch_options opt;
    idx_ch_options_init(&opt);
    opt.url = getenv("IDX_TEST_CH_URL");

    idx_error err;
    idx_error_clear(&err);
    idx_ch_conn *conn = NULL;
    TEST_EQ_INT(idx_ch_open(&opt, &conn, &err), IDX_OK);
    if (conn == NULL) {
        return;
    }

    char sql[512];
    const char *tables[] = {"idx_ch_rows_bin", "idx_ch_rows_json"};
    const idx_ch_row_format formats[] = {IDX_CH_FORMAT_ROW_BINARY,
                                         IDX_CH_FORMAT_JSON_EACH_ROW};

    for (size_t i = 0; i < 2; i++) {
        snprintf(sql, sizeof sql, "DROP TABLE IF EXISTS %s", tables[i]);
        TEST_EQ_INT(idx_ch_query(conn, sql, NULL, &err), IDX_OK);
        snprintf(sql, sizeof sql, "CREATE TABLE %s %s", tables[i], g_columns);
        TEST_EQ_INT(idx_ch_query(conn, sql, NULL, &err), IDX_OK);

        idx_ch_rows w;
        idx_ch_rows_init(&w, formats[i]);
        TEST_EQ_INT(write_rows(&w, &err), IDX_OK);
        TEST_EQ_UINT(idx_ch_rows_count(&w), 2);
        insert_rows(conn, tables[i], &w);
        idx_ch_rows_free(&w);

        snprintf(sql, sizeof sql, "SELECT count() FROM %s", tables[i]);
        TEST_EQ_INT(scalar(conn, sql), 2);
    }

    /*
     * The two formats must decode to the same rows. EXCEPT in both directions
     * is the whole assertion: any difference in a value, a null or a byte of a
     * key leaves a row on one side.
     */
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM (SELECT * FROM "
                             "idx_ch_rows_bin EXCEPT SELECT * FROM "
                             "idx_ch_rows_json)"),
                0);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM (SELECT * FROM "
                             "idx_ch_rows_json EXCEPT SELECT * FROM "
                             "idx_ch_rows_bin)"),
                0);

    /* And the values are the ones written, not merely equal to each other. */
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM idx_ch_rows_bin WHERE "
                             "u8v = 255 AND u16v = 65535 AND "
                             "u32v = 4294967295 AND i8v = -128 AND "
                             "i16v = -32768 AND i64v = -9223372036854775808"),
                2);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM idx_ch_rows_bin WHERE "
                             "u64v = 18446744073709551615"),
                1);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM idx_ch_rows_bin WHERE "
                             "nu64 IS NULL AND nfixed IS NULL AND "
                             "nstr IS NULL AND nf64 IS NULL"),
                1);
    /* The 32 high bytes survived both encodings intact. */
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM idx_ch_rows_json WHERE "
                             "hex(fixedv) = "
                             "'808182838485868788898A8B8C8D8E8F"
                             "909192939495969798999A9B9C9D9E9F'"),
                1);
    /* And so did a string needing JSON escapes. */
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM idx_ch_rows_json WHERE "
                             "strv = 'a\"b\\\\c\\nd'"),
                2);

    for (size_t i = 0; i < 2; i++) {
        snprintf(sql, sizeof sql, "DROP TABLE IF EXISTS %s", tables[i]);
        TEST_EQ_INT(idx_ch_query(conn, sql, NULL, &err), IDX_OK);
    }
    idx_ch_close(conn);
}

TEST_MAIN({
    TEST_RUN(test_format_name);
    TEST_RUN(test_binary_integers);
    TEST_RUN(test_binary_float);
    TEST_RUN(test_binary_strings);
    TEST_RUN(test_binary_varint);
    TEST_RUN(test_binary_nullable);
    TEST_RUN(test_json_row);
    TEST_RUN(test_json_escaping);
    TEST_RUN(test_json_non_finite);
    TEST_RUN(test_guards);
    if (getenv("IDX_TEST_CH_URL") != NULL) {
        TEST_RUN(test_online);
    } else {
        printf("  (skipping ch rows online tests: set IDX_TEST_CH_URL)\n");
    }
})
