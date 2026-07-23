#include "slot_cursor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#define CURSOR_PATH "/tmp/idx_slot_cursor_test.cursor"

static void write_file(const char *path, const char *contents) {
    FILE *file = fopen(path, "w");
    TEST_ASSERT(file != NULL);
    if (file != NULL) {
        fputs(contents, file);
        fclose(file);
    }
}

static bool file_exists(const char *path) {
    FILE *file = fopen(path, "r");
    if (file != NULL) {
        fclose(file);
        return true;
    }
    return false;
}

static void test_init(void) {
    idx_slot_cursor cursor;
    idx_slot_cursor_init(&cursor, 250000000u);

    TEST_EQ_UINT(cursor.last_indexed, IDX_SLOT_NONE);
    TEST_EQ_UINT(cursor.last_seen, IDX_SLOT_NONE);
    TEST_EQ_UINT(cursor.start_slot, 250000000u);
    TEST_EQ_STR(cursor.path, "");
}

/*
 * The watermark can move backwards, and must: a hole found below the current
 * position lowers what can be trusted, and keeping the higher value would
 * resume past slots that were never indexed.
 */
static void test_set_indexed_moves_either_way(void) {
    idx_slot_cursor cursor;
    idx_slot_cursor_init(&cursor, 0);

    idx_slot_cursor_set_indexed(&cursor, 500);
    TEST_EQ_UINT(cursor.last_indexed, 500u);

    idx_slot_cursor_set_indexed(&cursor, 99);
    TEST_EQ_UINT(cursor.last_indexed, 99u);

    /* Which is exactly where record_indexed would have refused to go. */
    idx_slot_cursor_record_indexed(&cursor, 42);
    TEST_EQ_UINT(cursor.last_indexed, 99u);

    idx_slot_cursor_set_indexed(&cursor, IDX_SLOT_NONE);
    TEST_EQ_UINT(cursor.last_indexed, IDX_SLOT_NONE);
    /* And a fresh start reads as one again. */
    TEST_EQ_UINT(idx_slot_cursor_resume_slot(&cursor), 0u);

    idx_slot_cursor_set_indexed(NULL, 1); /* must not crash */
}

static void test_record_indexed_monotonic(void) {
    idx_slot_cursor cursor;
    idx_slot_cursor_init(&cursor, 0);

    idx_slot_cursor_record_indexed(&cursor, 100);
    TEST_EQ_UINT(cursor.last_indexed, 100u);

    /* Forward advances. */
    idx_slot_cursor_record_indexed(&cursor, 105);
    TEST_EQ_UINT(cursor.last_indexed, 105u);

    /* A lower or equal slot is ignored: replaying a committed slot is safe. */
    idx_slot_cursor_record_indexed(&cursor, 102);
    TEST_EQ_UINT(cursor.last_indexed, 105u);
    idx_slot_cursor_record_indexed(&cursor, 105);
    TEST_EQ_UINT(cursor.last_indexed, 105u);

    /* Slot 0 is a real slot, not the empty value. */
    idx_slot_cursor_init(&cursor, 0);
    idx_slot_cursor_record_indexed(&cursor, 0);
    TEST_EQ_UINT(cursor.last_indexed, 0u);

    /* record_indexed leaves the socket frontier alone. */
    TEST_EQ_UINT(cursor.last_seen, IDX_SLOT_NONE);
}

static void test_observe_monotonic(void) {
    idx_slot_cursor cursor;
    idx_slot_cursor_init(&cursor, 0);

    idx_slot_cursor_observe(&cursor, 200);
    TEST_EQ_UINT(cursor.last_seen, 200u);

    /* A gap ahead advances the frontier; this is the value a reconnect
     * compares against to find the missed range. */
    idx_slot_cursor_observe(&cursor, 210);
    TEST_EQ_UINT(cursor.last_seen, 210u);

    /* Late, lower arrivals never lower the frontier. */
    idx_slot_cursor_observe(&cursor, 205);
    TEST_EQ_UINT(cursor.last_seen, 210u);

    /* observe leaves the indexed frontier alone. */
    TEST_EQ_UINT(cursor.last_indexed, IDX_SLOT_NONE);
}

static void test_resume_slot(void) {
    idx_slot_cursor cursor;

    /* Fresh, follow the tip. */
    idx_slot_cursor_init(&cursor, 0);
    TEST_EQ_UINT(idx_slot_cursor_resume_slot(&cursor), 0u);

    /* Fresh, configured floor. */
    idx_slot_cursor_init(&cursor, 500);
    TEST_EQ_UINT(idx_slot_cursor_resume_slot(&cursor), 500u);

    /* With progress, resume one past it, and the persisted position wins over
     * the configured start. */
    idx_slot_cursor_record_indexed(&cursor, 900);
    TEST_EQ_UINT(idx_slot_cursor_resume_slot(&cursor), 901u);

    /* Genesis indexed resumes at slot 1, not the tip. */
    idx_slot_cursor_init(&cursor, 0);
    idx_slot_cursor_record_indexed(&cursor, 0);
    TEST_EQ_UINT(idx_slot_cursor_resume_slot(&cursor), 1u);
}

/* The core promise: index some slots, restart, resume past them. */
static void test_save_load_roundtrip(void) {
    remove(CURSOR_PATH);

    idx_error err;
    idx_error_clear(&err);

    idx_slot_cursor first;
    TEST_EQ_INT(idx_slot_cursor_open(&first, CURSOR_PATH, 100, &err), IDX_OK);
    TEST_EQ_UINT(idx_slot_cursor_resume_slot(&first), 100u); /* fresh start */

    idx_slot_cursor_record_indexed(&first, 105);
    idx_slot_cursor_record_indexed(&first, 110);
    TEST_EQ_INT(idx_slot_cursor_save(&first, &err), IDX_OK);

    /* The rename left no temporary behind. */
    TEST_ASSERT(!file_exists(CURSOR_PATH ".tmp"));

    /* A second process opens the same file and resumes. */
    idx_slot_cursor second;
    TEST_EQ_INT(idx_slot_cursor_open(&second, CURSOR_PATH, 100, &err), IDX_OK);
    TEST_EQ_UINT(second.last_indexed, 110u);
    TEST_EQ_UINT(idx_slot_cursor_resume_slot(&second), 111u);
    /* The runtime frontier is not persisted. */
    TEST_EQ_UINT(second.last_seen, IDX_SLOT_NONE);

    remove(CURSOR_PATH);
}

static void test_open_missing_is_fresh(void) {
    remove(CURSOR_PATH);

    idx_error err;
    idx_error_clear(&err);

    idx_slot_cursor cursor;
    TEST_EQ_INT(idx_slot_cursor_open(&cursor, CURSOR_PATH, 42, &err), IDX_OK);
    TEST_EQ_UINT(cursor.last_indexed, IDX_SLOT_NONE);
    TEST_EQ_UINT(idx_slot_cursor_resume_slot(&cursor), 42u);
    /* A missing file must not leave an error recorded. */
    TEST_EQ_INT(err.status, IDX_OK);
    TEST_EQ_STR(cursor.path, CURSOR_PATH);
}

static void test_in_memory_save_is_noop(void) {
    idx_slot_cursor cursor;
    idx_slot_cursor_init(&cursor, 0);
    idx_slot_cursor_record_indexed(&cursor, 5);
    /* No path: save succeeds and writes nothing. */
    TEST_EQ_INT(idx_slot_cursor_save(&cursor, NULL), IDX_OK);
}

/* A cursor file created before anything is indexed round-trips as "no
 * progress", so the pipeline can checkpoint from the start without special
 * cases. */
static void test_save_load_no_progress(void) {
    remove(CURSOR_PATH);

    idx_slot_cursor cursor;
    idx_slot_cursor_init(&cursor, 300);
    memcpy(cursor.path, CURSOR_PATH, sizeof(CURSOR_PATH));
    TEST_EQ_INT(idx_slot_cursor_save(&cursor, NULL), IDX_OK);

    idx_slot_cursor loaded;
    idx_slot_cursor_init(&loaded, 300);
    TEST_EQ_INT(idx_slot_cursor_load(&loaded, CURSOR_PATH, NULL), IDX_OK);
    TEST_EQ_UINT(loaded.last_indexed, IDX_SLOT_NONE);
    TEST_EQ_UINT(idx_slot_cursor_resume_slot(&loaded), 300u);

    remove(CURSOR_PATH);
}

static void test_load_not_found(void) {
    remove(CURSOR_PATH);

    idx_error err;
    idx_error_clear(&err);

    idx_slot_cursor cursor;
    idx_slot_cursor_init(&cursor, 0);
    TEST_EQ_INT(idx_slot_cursor_load(&cursor, CURSOR_PATH, &err),
                IDX_ERR_NOT_FOUND);
    /* The cursor is untouched on failure. */
    TEST_EQ_UINT(cursor.last_indexed, IDX_SLOT_NONE);
}

static void test_load_comments_only(void) {
    write_file(CURSOR_PATH, "# just a header\n\n   \n");

    idx_slot_cursor cursor;
    idx_slot_cursor_init(&cursor, 0);
    idx_slot_cursor_record_indexed(&cursor, 7); /* overwritten by load */
    TEST_EQ_INT(idx_slot_cursor_load(&cursor, CURSOR_PATH, NULL), IDX_OK);
    TEST_EQ_UINT(cursor.last_indexed, IDX_SLOT_NONE);

    remove(CURSOR_PATH);
}

static void test_load_malformed(void) {
    idx_slot_cursor cursor;
    idx_slot_cursor_init(&cursor, 0);

    idx_error err;

    write_file(CURSOR_PATH, "last_indexed = notanumber\n");
    idx_error_clear(&err);
    TEST_EQ_INT(idx_slot_cursor_load(&cursor, CURSOR_PATH, &err), IDX_ERR_PARSE);
    TEST_ASSERT(strstr(err.message, "last_indexed") != NULL);

    write_file(CURSOR_PATH, "last_indexed = -5\n");
    TEST_EQ_INT(idx_slot_cursor_load(&cursor, CURSOR_PATH, NULL), IDX_ERR_RANGE);

    write_file(CURSOR_PATH, "unknown_key = 1\n");
    TEST_EQ_INT(idx_slot_cursor_load(&cursor, CURSOR_PATH, NULL), IDX_ERR_PARSE);

    write_file(CURSOR_PATH, "no separator here\n");
    TEST_EQ_INT(idx_slot_cursor_load(&cursor, CURSOR_PATH, NULL), IDX_ERR_PARSE);

    write_file(CURSOR_PATH, "last_indexed = \n");
    TEST_EQ_INT(idx_slot_cursor_load(&cursor, CURSOR_PATH, NULL), IDX_ERR_PARSE);

    /* Malformed input never mutates the cursor. */
    TEST_EQ_UINT(cursor.last_indexed, IDX_SLOT_NONE);

    remove(CURSOR_PATH);
}

static void test_open_path_too_long(void) {
    char long_path[IDX_SLOT_CURSOR_PATH_MAX + 16];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';

    idx_slot_cursor cursor;
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_slot_cursor_open(&cursor, long_path, 0, &err), IDX_ERR_RANGE);
}

static void test_invalid_args(void) {
    idx_error err;

    idx_error_clear(&err);
    TEST_EQ_INT(idx_slot_cursor_open(NULL, CURSOR_PATH, 0, &err),
                IDX_ERR_INVALID_ARG);
    idx_error_clear(&err);
    TEST_EQ_INT(idx_slot_cursor_load(NULL, CURSOR_PATH, &err),
                IDX_ERR_INVALID_ARG);
    idx_error_clear(&err);
    TEST_EQ_INT(idx_slot_cursor_save(NULL, &err), IDX_ERR_INVALID_ARG);

    /* The mutators tolerate NULL without crashing. */
    idx_slot_cursor_init(NULL, 0);
    idx_slot_cursor_record_indexed(NULL, 1);
    idx_slot_cursor_observe(NULL, 1);
    TEST_EQ_UINT(idx_slot_cursor_resume_slot(NULL), 0u);
}

TEST_MAIN({
    TEST_RUN(test_init);
    TEST_RUN(test_set_indexed_moves_either_way);
    TEST_RUN(test_record_indexed_monotonic);
    TEST_RUN(test_observe_monotonic);
    TEST_RUN(test_resume_slot);
    TEST_RUN(test_save_load_roundtrip);
    TEST_RUN(test_open_missing_is_fresh);
    TEST_RUN(test_in_memory_save_is_noop);
    TEST_RUN(test_save_load_no_progress);
    TEST_RUN(test_load_not_found);
    TEST_RUN(test_load_comments_only);
    TEST_RUN(test_load_malformed);
    TEST_RUN(test_open_path_too_long);
    TEST_RUN(test_invalid_args);
})
