# tests/Makefile — Unit tests for live-server
#
# Uses doctest (C++ single-header) to test C modules.
#
# Prerequisites: build the main binary first via `make -C ..`
#
# Targets:
#   all / build    — compile all test binaries
#   test           — build + run all tests
#   download-doctest — fetch doctest.h from GitHub
#   clean          — remove test binaries
#
# Test binaries go to ../build/TEST-<name>.
# Object files from ../build/src/*.o are linked in.
# ============================================================================

CXXFLAGS  ?= -Wall -Wextra -Wpedantic -Wshadow -Wconversion -std=c++17 -g -O0
CXXFLAGS  += -I. -I../src $(CPPFLAGS)

BUILD     = ../build
SRC_OBJS  = $(filter-out $(BUILD)/src/main.o,$(wildcard $(BUILD)/src/*.o))
TEST_BINS = $(patsubst test_%.cpp,$(BUILD)/TEST-%,$(wildcard test_*.cpp))

UNAME_S   = $(shell uname -s)
LDLIBS    = -lpthread
ifeq ($(UNAME_S),Darwin)
	LDLIBS += -largp
endif

DOCTEST_URL = https://raw.githubusercontent.com/doctest/doctest/refs/tags/v2.5.3/doctest/doctest.h

.PHONY: all build test download-doctest clean

all: build

build: $(TEST_BINS)

test: build
	@set -e; \
	fail=0; \
	for bin in $(TEST_BINS); do \
		echo ""; \
		echo "  RUN  $$bin"; \
		$$bin || fail=1; \
	done; \
	exit $$fail

download-doctest:
	curl -fsSL "$(DOCTEST_URL)" -o doctest.h

$(BUILD)/TEST-%: test_%.cpp doctest.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(TEST_LDFLAGS) -o $@ $< $(SRC_OBJS) $(LDLIBS)

clean:
	rm -f $(TEST_BINS)
