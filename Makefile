# md-runtime: `make` builds the library, tests and examples; `make check` runs the tests.
# MODE=debug (default: -O1, address and undefined sanitizers) or MODE=release. CC=gcc or clang.
# Test vectors (from the client's oracle, never committed) are read from MDRT_VECTORS,
# default ../nhl95/out/vectors; the vector tests are skipped when it is absent.
CC      ?= cc
MODE    ?= debug
WARN    := -std=c11 -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes
SAN     := -fsanitize=address,undefined
ifeq ($(MODE),release)
OPT     := -O2 -DNDEBUG
SAN     :=
else
OPT     := -O1 -g -fno-omit-frame-pointer
endif
CFLAGS  += $(WARN) $(OPT) $(SAN) -Iinclude
LDFLAGS += $(SAN)
B       := build/$(MODE)
MDRT_VECTORS ?= ../nhl95/out/vectors
export MDRT_VECTORS

LIBSRC  := $(wildcard src/vdp/*.c src/psg/*.c src/pad/*.c src/sram/*.c src/mdrt/*.c)
LIBOBJ  := $(LIBSRC:%.c=$(B)/%.o)
TESTLIB := tests/check.c tests/png.c tests/sndtrace.c tests/spectrum.c
TESTS   := $(patsubst tests/%.c,$(B)/tests/%,$(filter-out $(TESTLIB),$(wildcard tests/*.c)))

all: $(B)/libmdrt.a $(TESTS)

$(B)/%.o: %.c $(wildcard include/mdrt/*.h) $(wildcard src/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/libmdrt.a: $(LIBOBJ)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

$(B)/tests/%: tests/%.c $(TESTLIB) $(TESTLIB:.c=.h) $(B)/libmdrt.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests -o $@ $< $(TESTLIB) $(B)/libmdrt.a $(LDFLAGS) -lm

check: all
	@fail=0; for t in $(TESTS); do echo "== $$t"; $$t || fail=1; done; \
	if [ $$fail = 0 ]; then echo "all tests passed"; else echo "TESTS FAILED"; exit 1; fi

clean:
	rm -rf build

.PHONY: all check clean
