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

`make help` shows all targets with descriptions.

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

## Key source files

| File                                                                      | Responsibility                                          |
| ------------------------------------------------------------------------- | ------------------------------------------------------- |
| `main.c`                                                                  | argp CLI parsing, config assembly, `server_run()` entry |
| `server.c`                                                                | Accept loop, thread pool, keep-alive, rate limiting     |
| `transport.c`                                                             | Opaque socket wrapper (POSIX)                           |
| `livereload.c`                                                            | SSE connections, broadcast, HTML injection              |
| `watcher.c` / `watcher_inotify.c` / `watcher_poll.c` / `watcher_kqueue.c` | File watching backends                                  |
| `response.c`                                                              | HTTP formatting, chunked encoding                       |
| `http.c`                                                                  | Request parsing, header handling                        |
| `file.c` / `file_send.c`                                                  | File serving, range requests, ETags                     |
| `ratelimit.c`                                                             | Per-IP connection limiting                              |
| `auth.c`                                                                  | HTTP Basic Auth                                         |
| `mime.c`                                                                  | MIME type detection                                     |
| `log.c`                                                                   | Timestamped stderr logging                              |

## Quirks

- `--live-reload` (`-U`) and `--live-hard-reload` (`-W`) are mutually exclusive.
- macOS always uses poll-based file watcher; Linux uses inotify by default (`--poll` to force poll).
- `compile_commands.json` is gitignored — regenerate if needed (e.g., via Bear or compiledb).
- Logging goes to **stderr** with millisecond timestamps and source location (compile-time flags `LOG_SHOW_TIME_STAMP`, `LOG_SHOW_SOURCE_LOCATION`).
- No CI workflows in repo.
- Version `2.7.1` in `src/project_config.h`.
- Project homepage: `https://github.com/pritam12426/http_server_c`

## CLI flags reference (from main.c)

```
Logging:        -L/--log-level [error|warn|info|debug]  (default: info)
                -R/--print-request                        Log each request
Live Reload:    -U/--live-reload                          SSE soft reload
                -W/--live-hard-reload                     Full cache-busting reload
Connection:     -P/--port <PORT>          (default: 8080)
                -H/--host <HOST>          (default: localhost)
                -T/--threads <NUM>        (default: 2, max 256)
                -K/--keep-alive <SECS>    (default: 3, 0=disable)
                -M/--max-conns <NUM>      (default: 0=unlimited, max 1000)
Authentication: -u/--user <USER>          (default when omitted: admin)
                -p/--pass <PASS>          (default when omitted: admin)
Serving:        -I/--dir <DIR>            (default: .)
                -i/--ignore               Hide hidden files
                -B/--browser <BROWSER>    Open browser on startup
                -o/--poll                 Force poll watcher (NFS/sshfs)
```

## Adding unit tests

1. Create `tests/test_<name>.cpp` using doctest
2. Run `make -C tests download-doctest` if needed
3. Run `make test` from repo root
