# Build system for solana_c_indexer.
#
#   make            build the debug binary (sanitizers enabled)
#   make release    build the optimized binary
#   make test       build and run the unit tests
#   make clean      remove all build output
#
# Variables:
#   BUILD=debug|release   selects the profile (default: debug)
#   SANITIZE=0            disables the sanitizers in debug builds
#   CC=clang              overrides the compiler

CC       ?= cc
BUILD    ?= debug
SANITIZE ?= 1
V        ?= 0

# V=1 prints the full compiler command lines instead of a one-line summary.
ifeq ($(V),1)
  Q :=
  ECHO := @true
else
  Q := @
  ECHO := @echo
endif

BIN_NAME := indexer
BUILD_DIR := build/$(BUILD)
OBJ_DIR   := $(BUILD_DIR)/obj
BIN       := $(BUILD_DIR)/$(BIN_NAME)

CURL_CFLAGS := $(shell curl-config --cflags 2>/dev/null)
CURL_LIBS   := $(shell curl-config --libs 2>/dev/null)
ifeq ($(CURL_LIBS),)
  CURL_LIBS := -lcurl
endif

# PostgreSQL client (libpq), milestone M7. Optional: the pg module compiles to
# nothing and its test is skipped when libpq is not found, so the project still
# builds without it. Detection prefers pkg-config, then pg_config. Override
# PG_CFLAGS and PG_LIBS (both) to point at a libpq in a non-standard location.
ifeq ($(origin PG_LIBS),undefined)
  ifeq ($(shell pkg-config --exists libpq 2>/dev/null && echo y),y)
    PG_CFLAGS := $(shell pkg-config --cflags libpq)
    PG_LIBS   := $(shell pkg-config --libs libpq)
  else ifneq ($(shell command -v pg_config 2>/dev/null),)
    PG_CFLAGS := -I$(shell pg_config --includedir)
    PG_LIBS   := -L$(shell pg_config --libdir) -lpq
  endif
endif
HAVE_LIBPQ := $(if $(strip $(PG_LIBS)),1,)

CFLAGS_BASE := -std=c11 -Wall -Wextra -Werror -pedantic \
               -Wshadow -Wconversion -Wstrict-prototypes \
               -D_POSIX_C_SOURCE=200809L -Iinclude -Ivendor \
               $(CURL_CFLAGS) -MMD -MP
LDLIBS_BASE := -lpthread $(CURL_LIBS)

# When libpq is present, define IDX_HAVE_LIBPQ (which gates src/pg.c) and link
# it into every binary. The one unused -lpq per test is harmless.
ifdef HAVE_LIBPQ
  CFLAGS_BASE += -DIDX_HAVE_LIBPQ $(PG_CFLAGS)
  LDLIBS_BASE += $(PG_LIBS)
endif

ifeq ($(BUILD),debug)
  CFLAGS_PROFILE := -O0 -g3 -DIDX_BUILD_DEBUG
  ifeq ($(SANITIZE),1)
    SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
  endif
else ifeq ($(BUILD),release)
  CFLAGS_PROFILE := -O2 -g -DNDEBUG
else
  $(error unknown BUILD '$(BUILD)', expected 'debug' or 'release')
endif

CFLAGS  := $(CFLAGS_BASE) $(CFLAGS_PROFILE) $(SAN_FLAGS) $(EXTRA_CFLAGS)
LDFLAGS := $(SAN_FLAGS) $(EXTRA_LDFLAGS)
LDLIBS  := $(LDLIBS_BASE)

SRCS     := $(wildcard src/*.c)
OBJS     := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))

# Vendored third-party code is compiled with warnings off: it is not ours to
# fix, and -Werror on someone else's source only breaks the build on upgrade.
VENDOR_SRCS   := $(wildcard vendor/*/*.c)
VENDOR_OBJS   := $(patsubst vendor/%.c,$(OBJ_DIR)/vendor/%.o,$(VENDOR_SRCS))
CFLAGS_VENDOR := -std=c11 -Ivendor -w -MMD -MP \
                 $(CFLAGS_PROFILE) $(SAN_FLAGS)

LIB_OBJS := $(filter-out $(OBJ_DIR)/main.o,$(OBJS)) $(VENDOR_OBJS)

TEST_SRCS := $(wildcard tests/test_*.c)
# The pg tests need libpq to link; drop them when libpq is absent.
ifndef HAVE_LIBPQ
  TEST_SRCS := $(filter-out tests/test_pg.c tests/test_pg_store.c,$(TEST_SRCS))
endif
TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%,$(TEST_SRCS))

# Diagnostic programs that talk to a live endpoint. Built by `make tools`, not
# by `all`, and never run by `make test`.
TOOL_SRCS := $(wildcard tools/*.c)
TOOL_BINS := $(patsubst tools/%.c,$(BUILD_DIR)/%,$(TOOL_SRCS))

.PHONY: all debug release test tools clean help

# The empty recipe suppresses "Nothing to be done" when already up to date.
all: $(BIN)
	@:

debug:
	@$(MAKE) --no-print-directory BUILD=debug all

release:
	@$(MAKE) --no-print-directory BUILD=release all

$(BIN): $(OBJS) $(VENDOR_OBJS) | $(BUILD_DIR)
	$(ECHO) "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(ECHO) "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/vendor/%.o: vendor/%.c
	$(ECHO) "  CC      $< (vendored)"
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS_VENDOR) -c -o $@ $<

$(BUILD_DIR)/tests/%: tests/%.c $(LIB_OBJS) | $(BUILD_DIR)/tests
	$(ECHO) "  CCLD    $<"
	$(Q)$(CC) $(CFLAGS) -Itests $(LDFLAGS) -o $@ $< $(LIB_OBJS) $(LDLIBS)

tools: $(TOOL_BINS)
	@:

$(BUILD_DIR)/%: tools/%.c $(LIB_OBJS) | $(BUILD_DIR)
	$(ECHO) "  CCLD    $<"
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_OBJS) $(LDLIBS)

test: $(TEST_BINS)
	@failed=0; \
	for t in $(TEST_BINS); do \
	  echo "  TEST    $$(basename $$t)"; \
	  ./$$t || failed=1; \
	done; \
	if [ $$failed -ne 0 ]; then echo "FAILED"; exit 1; fi; \
	echo "  all tests passed"

$(BUILD_DIR) $(OBJ_DIR) $(BUILD_DIR)/tests:
	@mkdir -p $@

clean:
	$(ECHO) "  RM      build/"
	$(Q)rm -rf build

help:
	@sed -n '1,14p' $(firstword $(MAKEFILE_LIST))

-include $(OBJS:.o=.d) $(VENDOR_OBJS:.o=.d) $(TEST_BINS:=.d)
