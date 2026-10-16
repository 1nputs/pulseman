# md-runtime: `make` builds the library, tests and examples; `make check` runs the tests.
# MODE=debug (default: -O1, address and undefined sanitizers) or MODE=release. CC=gcc or clang.
# FM_CORES: the FM cores built in, chosen at run time (mdrt_config.fm_core): "nuked ymfm" (the
# default; Nuked-OPN2 is the default core), or one of them: FM_CORES=ymfm has no LGPL code
# (Nuked-OPN2 is LGPL-2.1, ymfm BSD-3 and C++: a build with it links -lstdc++). FM_CORE=x is
# FM_CORES=x.
# Test vectors (from the client's oracle, never committed; docs/VECTORS.md) are read from
# MDRT_VECTORS, default ../ninety-four/out/vectors; the vector tests are skipped when it is absent, or
# fail with MDRT_REQUIRE_VECTORS=1.
CC      ?= cc
MODE    ?= debug
WERROR  ?= -Werror
WARN    := -std=c11 -Wall -Wextra $(WERROR) -Wshadow -Wstrict-prototypes -Wmissing-prototypes
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
MDRT_VECTORS ?= ../ninety-four/out/vectors
export MDRT_VECTORS

FM_CORES ?= $(if $(FM_CORE),$(FM_CORE),nuked ymfm)
LIBSRC  := $(wildcard src/vdp/*.c src/psg/*.c src/fm/fm.c src/mix/*.c src/pad/*.c src/sram/*.c src/mdrt/*.c)
ifneq ($(filter nuked,$(FM_CORES)),)
LIBSRC  += src/fm/nuked.c third_party/nuked-opn2/ym3438.c
CFLAGS  += -DMDRT_HAVE_NUKED
endif
ifneq ($(filter ymfm,$(FM_CORES)),)
LIBCXX  := src/fm/ymfm.cpp third_party/ymfm/src/ymfm_opn.cpp third_party/ymfm/src/ymfm_adpcm.cpp third_party/ymfm/src/ymfm_ssg.cpp
LDLIBS  += -lstdc++
CFLAGS  += -DMDRT_HAVE_YMFM
CXXFLAGS += -DMDRT_HAVE_YMFM
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
	$(AR) rcs $@ $^

$(B)/tests/%: tests/%.c $(TESTLIB) $(TESTLIB:.c=.h) $(B)/libmdrt.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests -o $@ $< $(TESTLIB) $(B)/libmdrt.a $(LDFLAGS) $(LDLIBS) -lm

# the SDL2 platform and the examples: make sdl
SDLCFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDLLIBS   := $(filter-out -mwindows,$(shell pkg-config --libs sdl2 2>/dev/null))   # (Windows: keep the console)
EXAMPLES  := $(patsubst examples/%.c,$(B)/examples/%,$(wildcard examples/*.c))
$(B)/src/platform/sdl2/%.o: src/platform/sdl2/%.c $(wildcard include/mdrt/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDLCFLAGS) -c $< -o $@
$(B)/libmdrt_sdl2.a: $(B)/src/platform/sdl2/sdl2.o
	$(AR) rcs $@ $^
$(B)/examples/%: examples/%.c $(B)/libmdrt_sdl2.a $(B)/libmdrt.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDLCFLAGS) -o $@ $< $(B)/libmdrt_sdl2.a $(B)/libmdrt.a $(LDFLAGS) $(LDLIBS) $(SDLLIBS) -lm
sdl: $(B)/libmdrt_sdl2.a $(EXAMPLES)

# (record_test records the runtime's own traces into build/record; they are replayed as vectors)
check: all
	@fail=0; for t in $(TESTS); do echo "== $$t"; $$t || fail=1; done; \
	for t in vdp_trace pad_trace mix_trace; do echo "== $(B)/tests/$$t on build/record (the runtime's own traces)"; \
	    MDRT_VECTORS=build/record MDRT_REQUIRE_VECTORS=1 $(B)/tests/$$t || fail=1; done; \
	if [ $$fail = 0 ]; then echo "all tests passed"; else echo "TESTS FAILED"; exit 1; fi

clean:
	rm -rf build

.PHONY: all check clean sdl
