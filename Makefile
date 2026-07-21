# Makefile — live-server build system
#
# Targets:
#   all     — release build (-O3)
#   debug   — debug build (-g3 -DDEBUG)
#   strip   — strip debug symbols
#   install — install to PREFIX
#   uninstall
#   clean
#
# Options (set via environment or on the command line):
#   make debug -B O_DEBUG=1  — debug build
#
# macOS prerequisite: brew install argp-standalone

UNAME_S := $(shell uname -s)

PREFIX ?= /usr/local
BINPREFIX ?= $(PREFIX)/bin
MANPREFIX ?= $(PREFIX)/share/man/man1

STRIP ?= strip
PKG_CONFIG ?= pkg-config
INSTALL ?= install

CFLAGS_OPTIMIZATION ?= -O3

BUILD = build
BIN   = live-server

HEADERS   = $(wildcard src/*.h)
SRC       = $(wildcard src/*.c)

# Compiler warnings
CFLAGS +=  -Wall -Wextra -Wpedantic \
           -Wstrict-prototypes -Wmissing-prototypes \
           -Wshadow -Wconversion \
           -Wno-missing-field-initializers

# Common flags
CFLAGS += -Isrc -std=c17
LDLIBS +=  -lpthread

# Convert targets to flags for backwards compatibility
O_DEBUG := 0  # Debug binary (0 = release, 1 = debug)

ifneq ($(filter debug,$(MAKECMDGOALS)),)
	O_DEBUG := 1
endif

ifeq ($(strip $(O_DEBUG)),1)
	# Debug build: include symbols, enable DEBUG macro
	CFLAGS += -g3 -DDEBUG -fstack-usage \
	          -fsanitize=address -fsanitize=undefined
	LDFLAGS += -fsanitize=address -fsanitize=undefined
    ifneq (,$(findstring clang,$(CC)))
		CFLAGS += -ffreestanding
    endif
else
	# Release build: optimisation only
	CFLAGS += $(CFLAGS_OPTIMIZATION)
endif

# Platform-specific settings
ifeq ($(UNAME_S),Darwin)
	# macOS: need argp from Homebrew (brew install argp-standalone)
	LDLIBS += -largp
else
	# Linux: _GNU_SOURCE for strptime, etc.
	CFLAGS += -D_GNU_SOURCE
endif

OUT = $(SRC:%.c=$(BUILD)/%.o)

all: $(BIN)

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
	awk 'BEGIN {FS = ":.*?## "}; {printf "\033[33m%-20s\033[0m %s\n", $$1, $$2}'

$(BUILD): ## Create build directories automatically
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OUT) ## Build the live-server binary
	$(CC) $(LDFLAGS) -o $@ $(OUT) $(LDLIBS)

debug: $(BIN) ## Build the debug binary run `make debug -B O_DEBUG=1`

install: all ## Install the live-server binary
	$(INSTALL) -m 0755 -d $(DESTDIR)$(BINPREFIX)
	$(INSTALL) -m 0755 $(BIN) $(DESTDIR)$(BINPREFIX)

	$(INSTALL) -m 0755 -d $(DESTDIR)$(MANPREFIX)
	$(INSTALL) -m 0755 live-server.1 $(DESTDIR)$(MANPREFIX)

clean: ## Clean up build artifacts
	$(RM) -rf $(OUT) $(BIN)

uninstall: ## Uninstall the live-server binary
	$(RM) $(DESTDIR)$(BINPREFIX)/$(BIN)
	$(RM) $(DESTDIR)$(MANPREFIX)/live-server.1

strip: $(BIN) ## Strip the live-server binary
	$(STRIP) $^

test: $(BIN) ## Build and run unit tests (via tests/Makefile)
	$(MAKE) -C tests test TEST_LDFLAGS="$(LDFLAGS)"

.PHONY: all install uninstall strip clean debug test
