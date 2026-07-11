# live-server

A zero-dependency static file server with live reload, written in C17 for Linux and macOS. Just build and run — no runtime dependencies beyond POSIX.

---

## Philosophy

**Do one thing well.** This server serves static files and reloads the browser when they change. Nothing more.

- **No framework, no runtime, no config files.** A single binary you can drop anywhere.
- **Explicit over implicit.** All configuration via CLI flags — no hidden `.rc` files, no environment variables, no magic.
- **Correctness over features.** Thread-safe, memory-safe (ASan/UBSan clean), tested. Edge cases handled: path traversal, conditional requests, byte ranges, graceful shutdown.
- **Performance without complexity.** `sendfile()` on Linux, `writev()` everywhere, pre-computed headers, thread-local buffers, lock-free logging — but the code stays readable.
- **Local-first.** Designed for `localhost` and trusted LAN. No TLS (use a reverse proxy), no compression (dev tools show uncompressed anyway), no directory listing (add `index.html`).

---

## Features

| Category          | Details                                                                                            |
| ----------------- | -------------------------------------------------------------------------------------------------- |
| **Core**          | Static files, correct MIME types, ETag/If-Modified-Since, byte ranges, keep-alive                  |
| **Live reload**   | SSE-based; soft reload (`-U`) or cache-busting hard reload (`-W`); injected at serve time          |
| **Concurrency**   | Fixed-size thread pool (1–256 workers), circular work queue, no thread-per-connection              |
| **Rate limiting** | Per-IP connection cap with dynamic hash table (grows at 75% load)                                  |
| **Auth**          | HTTP Basic Auth (optional, default `admin`/`admin`)                                                |
| **File watcher**  | Linux: inotify (recursive, auto-adds new dirs) · macOS: kqueue (recursive) · Fallback: 500 ms poll |
| **Logging**       | Lock-free ring buffer, dedicated consumer thread, millisecond timestamps, ANSI colors on TTY       |
| **Observability** | Access log (IP, method, path, status, bytes, MIME) + optional full request dump                    |

---

## Quick Start

```sh
# macOS
brew install argp-standalone

# Clone
git clone "https://github.com/pritam12426/live_server"
cd "live_server"

# Build
make          # release (-O3)
# or
make debug O_DEBUG=1    # debug + ASan + UBSan

# Run
./live-server
# → serving ./ on http://localhost:8080

# Installing 
make install
```

---

## CLI Reference

| Flag                 | Short | Default     | Description                                              |
| -------------------- | ----- | ----------- | -------------------------------------------------------- |
| `--dir`              | `-I`  | `.`         | Directory to serve                                       |
| `--port`             | `-P`  | `8080`      | Listen port                                              |
| `--host`             | `-H`  | `localhost` | Bind address (`0.0.0.0` for LAN)                         |
| `--threads`          | `-t`  | `4`         | Worker threads (1–256)                                   |
| `--keep-alive`       | `-k`  | `5`         | Keep-alive timeout (0 = off)                             |
| `--max-conns`        | `-M`  | `0`         | Max concurrent conns/IP (0 = unlimited)                  |
| `--browser`          | `-B`  | —           | Open browser on start (`open`, `xdg-open`, `firefox`, …) |
| `--log-level`        | `-L`  | `info`      | `error` \| `warn` \| `info` \| `debug`                   |
| `--print-request`    | `-R`  | —           | Log full request headers                                 |
| `--live-reload`      | `-U`  | —           | Soft reload on file change                               |
| `--live-hard-reload` | `-W`  | —           | Hard reload (cache-bust)                                 |
| `--user`             | `-u`  | `admin`     | Basic Auth username                                      |
| `--pass`             | `-p`  | `admin`     | Basic Auth password                                      |
| `--poll`             | `-o`  | —           | Force poll watcher (Linux)                               |
| `--ignore`           | `-i`  | —           | Skip hidden files in watcher                             |

> `-U` and `-W` are mutually exclusive.

---

## Common Recipes

```sh
# Basic
live-server

# Custom directory + port
live-server -I ./dist -P 3000

# LAN access (phone testing)
live-server -H 0.0.0.0 -P 8080

# Live reload during development
live-server -I ./site -U

# Hard reload (CSS/JS cache issues)
live-server -I ./site -W

# Rate limit + auth
live-server -M 10 -u me -p secret

# Open browser on start (macOS)
live-server -B open
```

---

## How Live Reload Works

1. Server injects a tiny `<script>` into every served HTML page (connects to `/livereload` via SSE).
2. File watcher (inotify/kqueue/poll) detects any change in the served tree.
3. Server broadcasts `reload` or `hard-reload` event to all connected clients.
4. Browser executes:
   - **Soft** (`-U`): `window.location.reload()` — fast, uses cache
   - **Hard** (`-W`): Appends `_lr=<timestamp>` query param and navigates — forces re-fetch

No browser extension, no manual script tag.

---

## Build Targets

```sh
make                # release (-O3)
make debug O_DEBUG=1  # debug (-g3 -DDEBUG -fsanitize=address,undefined)
make strip          # release + strip symbols
make install        # /usr/local/bin (or PREFIX=~/.local)
make uninstall
make clean
make help
```

---

## Testing

```sh
# Integration tests (26 tests via curl)
bash tests/test_server.sh ./live-server 9999

# Unit tests (doctest, 11 cases / 54 assertions)
make test
```

All tests pass under ASan+UBSan.

---

## Project Structure

```
src/
├── main.c              # CLI entry, argp parsing
├── server.c/.h         # Accept loop, thread pool, rate limit, lifecycle
├── transport.c/.h      # Opaque socket wrapper (writev, timeout, TLS-ready)
├── http.c/.h           # Request parser (buffered headers, URL decode)
├── header_cache.c/.h   # Pre-computed Date/Server/Connection headers
├── response.c/.h       # Response builder (error pages, redirects)
├── file.c/.h           # Static file serving, sendfile, live-reload injection
├── thread_pool.c/.h    # Fixed pool, circular queue, mutex + 2 condvars
├── ratelimit.c/.h      # Dynamic open-addressing hash table (IP → count)
├── auth.c/.h           # Basic Auth (hand-rolled Base64)
├── livereload.c/.h     # SSE registry (hash map), broadcast, heartbeat
├── watcher.c/.h        # inotify / kqueue / poll backends
├── mime.c/.h           # Extension → MIME lookup table
├── log.c/.h            # Lock-free SPSC ring buffer logger
├── thread_buffer.c/.h  # TLS reusable buffers (no malloc/free on hot path)
├── types.h             # Shared enums (LivereloadMode)
└── project_config.h    # Build-time constants (version, name, URL)
```

---

## Design Decisions (TL;DR)

| Decision                                      | Why                                          |
| --------------------------------------------- | -------------------------------------------- |
| Blocking I/O + thread pool                    | Simpler than async; POSIX threads everywhere |
| `sendfile()` on Linux                         | Zero-copy kernel→socket for static files     |
| `writev()` for headers+body                   | Single syscall per response                  |
| Pre-computed header cache                     | Date/Server/Connection formatted once/sec    |
| Thread-local buffers                          | Eliminate malloc/free on read path           |
| Lock-free ring logger                         | No mutex contention under load               |
| Dynamic hash tables (ratelimit, SSE, watcher) | O(1) ops, grow under load                    |
| kqueue on macOS                               | Native event-driven, no polling              |
| No TLS / compression / dir listing            | Out of scope — use a reverse proxy for prod  |

---

## License

MIT — see [LICENSE](LICENSE).
