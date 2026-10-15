# md-runtime: `make` builds the library, tests and examples; `make check` runs the tests.
# MODE=debug (default: -O1, address and undefined sanitizers) or MODE=release. CC=gcc or clang.
# FM_CORE=nuked (default: Nuked-OPN2, LGPL-2.1) or ymfm (BSD-3, C++).
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

FM_CORE ?= nuked
LIBSRC  := $(wildcard src/vdp/*.c src/psg/*.c src/fm/fm.c src/mix/*.c src/pad/*.c src/sram/*.c src/mdrt/*.c)
ifeq ($(FM_CORE),ymfm)
LIBCXX  := src/fm/ymfm.cpp third_party/ymfm/src/ymfm_opn.cpp third_party/ymfm/src/ymfm_adpcm.cpp third_party/ymfm/src/ymfm_ssg.cpp
LDLIBS  += -lstdc++
else
LIBSRC  += src/fm/nuked.c third_party/nuked-opn2/ym3438.c
endif
LIBOBJ  := $(LIBSRC:%.c=$(B)/%.o) $(LIBCXX:%.cpp=$(B)/%.o)
TESTLIB := tests/check.c tests/png.c tests/sndtrace.c tests/spectrum.c
TESTS   := $(patsubst tests/%.c,$(B)/tests/%,$(filter-out $(TESTLIB),$(wildcard tests/*.c)))

all: $(B)/libmdrt.a $(TESTS)

$(B)/%.o: %.c $(wildcard include/mdrt/*.h) $(wildcard src/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# vendored code: as published, which shifts negative numbers left (defined by GCC and Clang)
$(B)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(if $(SAN),-fno-sanitize=shift) -c $< -o $@

CXX     ?= c++
CXXFLAGS += -std=c++14 -Wall -Wextra -Wno-unused-parameter $(OPT) $(SAN) -Iinclude -Ithird_party/ymfm/src
$(B)/%.o: %.cpp $(wildcard src/fm/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/libmdrt.a: $(LIBOBJ)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

$(B)/tests/%: tests/%.c $(TESTLIB) $(TESTLIB:.c=.h) $(B)/libmdrt.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests -o $@ $< $(TESTLIB) $(B)/libmdrt.a $(LDFLAGS) $(LDLIBS) -lm

check: all
	@fail=0; for t in $(TESTS); do echo "== $$t"; $$t || fail=1; done; \
	if [ $$fail = 0 ]; then echo "all tests passed"; else echo "TESTS FAILED"; exit 1; fi

clean:
	rm -rf build

.PHONY: all check clean
