# AGENTS.md — live-server

## macOS prerequisite

```sh
brew install argp-standalone
```

## Build

| Command                             | Description                             |
| ----------------------------------- | --------------------------------------- |
| `make`                              | Release build (`-O3`)                   |
| `make debug -B O_DEBUG=1`           | Debug build (`-g3 -DDEBUG`, ASan+UBSan) |
| `make strip`                        | Release build + strip symbols           |
| `sudo make install`                 | Install to `/usr/local/bin`             |
| `sudo make install PREFIX=~/.local` | Install to `~/.local/bin`               |

Binary placed at `./live-server`.

## Test

Two separate test suites:

- **Unit tests** (C++ doctest): `make test` — compiles and runs `tests/test_*.cpp`, links against all `build/src/*.o` except `main.o`.
- **Integration tests** (bash+curl): `bash tests/test_server.sh [./live-server] [9999]`

Before running `make test`, build the main binary first (`make`). Requires `tests/doctest.h` (gitignored; fetch via `make -C tests download-doctest`).

## Lint

```sh
clang-tidy src/*.c -- -Isrc -std=c17
```

## Architecture

- Flat `src/` with `.c/.h` pairs; no subdirs.
- Entry: `src/main.c` → argp CLI parsing → `server_run()`.
- `transport.c` wraps plain POSIX sockets as an opaque `Transport` type.
- `server.c` owns the accept loop, thread pool dispatch, keep-alive, and rate limiting.
- `livereload.c` manages SSE connections and broadcasts; `watcher.c` polls (macOS) or uses inotify (Linux).
- `response.c` handles HTTP response formatting including `Transfer-Encoding: chunked`.
- No runtime dependencies beyond POSIX + pthreads. No config files or env vars — all config via CLI flags.

## Quirks

- `--live-reload` (`-U`) and `--live-hard-reload` (`-W`) are mutually exclusive.
- macOS always uses poll-based file watcher; Linux uses inotify by default (`--poll` to force poll).
- `compile_commands.json` is gitignored — regenerate if needed (e.g., via Bear or compiledb).
- Logging goes to **stderr** with millisecond timestamps and source location (compile-time flags `LOG_SHOW_TIME_STAMP`, `LOG_SHOW_SOURCE_LOCATION`).
- No CI workflows in repo.
- Version `1.1.1` in `src/project_config.h`.
