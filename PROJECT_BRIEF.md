# live-server — Complete Codebase Reference for A new contributor

> **Purpose**: This document gives a new contributor a complete, accurate mental model of the codebase in one read. No speculation — only what exists in the repository as of the current commit.

---

## 1. Project Identity

| Attribute        | Value                                                                                                                                                                   |
| ---------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Name**         | live-server                                                                                                                                                             |
| **Language**     | C17 (strict: `-std=c17 -Wall -Wextra -Wpedantic -Wstrict-prototypes -Wmissing-prototypes -Wshadow -Wconversion`)                                                        |
| **Platforms**    | Linux (inotify + sendfile), macOS (kqueue + poll fallback)                                                                                                              |
| **Dependencies** | Zero runtime deps. Build-time: `argp-standalone` on macOS (Homebrew), `pthread`, C library                                                                              |
| **Binary**       | Single static-like executable `./live-server` (~300 KB stripped)                                                                                                        |
| **License**      | MIT                                                                                                                                                                     |
| **Philosophy**   | _Do one thing well._ Serve static files + live reload. No config files, no env vars, no TLS, no compression, no HTTP/2, no directory listing. All config via CLI flags. |

**Not a framework.** Not a library. Not extensible at runtime. A single-purpose tool for local web development.

---

## 2. High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        main thread                               │
│  argp CLI → log_init → header_cache_init → thread_pool_create   │
│       ↓                ↓                  ↓                    │
│  ratelimit_create → livereload_start → make_listener            │
│       ↓                                                          │
│  accept() loop ──────────────────────────────────────────────┐  │
│       │                                                      │  │
│       ▼                                                      │  │
│  transport_new() + ratelimit_accept() + thread_pool_submit() │  │
└───────┼──────────────────────────────────────────────────────┼──┘
        │                                                      │
        ▼                                                      ▼
┌─────────────────────────┐                    ┌─────────────────────────┐
│   thread pool (N workers)│                    │  dedicated threads      │
│  ┌─────┐ ┌─────┐ ┌─────┐ │                    │  ┌──────────────────┐  │
│  │ W1  │ │ W2  │ │ WN  │ │                    │  │ log consumer     │  │
│  └─────┘ └─────┘ └─────┘ │                    │  │ (drains ring buf)│  │
│        ▲        ▲        │                    │  └──────────────────┘  │
│        │        │        │                    │  ┌──────────────────┐  │
│  circular work queue     │                    │  │ watcher thread   │  │
│  (4096 slots, mutex +    │                    │  │ (inotify/kqueue/ │  │
│   2 condvars)            │                    │  │  poll)           │  │
└──────────────────────────┘                    │  └──────────────────┘  │
                                               │  ┌──────────────────┐  │
                                               │  │ SSE client 1     │  │
                                               │  │ (pthread, 15s    │  │
                                               │  │  heartbeat)      │  │
                                               │  └──────────────────┘  │
                                               │  ┌──────────────────┐  │
                                               │  │ SSE client N     │  │
                                               │  └──────────────────┘  │
                                               └─────────────────────────┘
```

**Key invariants:**

- Main thread only accepts + dispatches. Never blocks on I/O.
- Worker threads handle full request lifecycle (parse → serve → keep-alive loop).
- SSE clients get their own detached pthread (one per connection).
- Log consumer is single-threaded; workers are lock-free producers.
- Watcher is single-threaded regardless of backend.

---

## 3. Source Tree

```
src/
├── main.c              # Entry point, argp parsing, ServerConfig construction
├── server.c / .h       # Accept loop, signal handling, thread pool dispatch, keep-alive
├── transport.c / .h    # Opaque Transport (fd wrapper), read/write/writev, timeouts
├── http.c / .h         # Request parser (4 KB buffered reads, HttpRequest struct)
├── response.c / .h     # High-level response builders (error, redirect, send)
├── header_cache.c / .h # Pre-computed Date/Server/Connection headers (1 Hz update)
├── file.c / .h         # Static file serving, sendfile, range, ETag, live-reload injection
├── thread_buffer.c / .h# TLS grow-only body buffers (no malloc/free per request)
├── auth.c / .h         # Basic Auth (Base64 decode, credential check)
├── thread_pool.c / .h  # Fixed-size pool, circular queue, mutex + 2 condvars
├── ratelimit.c / .h    # Dynamic hash table (djb2, linear probe, grows 2× at 75%)
├── livereload.c / .h   # SSE registry (1024-bucket hash map), broadcast, heartbeat
├── watcher.c / .h      # File watcher: inotify (Linux) / kqueue (macOS) / poll (fallback)
├── mime.c / .h         # Extension → MIME lookup (static table, strcasecmp)
├── log.c / .h          # Lock-free SPSC ring (4096 slots), consumer thread
├── types.h             # LivereloadMode enum
└── project_config.h    # VERSION, BINARY_NAME, HOMEPAGE_URL constants
```

**No subdirectories.** Flat `src/` with `.c/.h` pairs. `Makefile` uses `$(wildcard src/*.c)`.

---

## 4. Module Deep Dive

### 4.1 `main.c` — Entry & CLI

- `argp_parse()` with `parse_opt()` callback → fills global `Arguments G_Args`.
- Mutually exclusive: `-U` (soft reload) vs `-W` (hard reload).
- `-p` without value → both user/pass = `"admin"`.
- `--dir` validated against filesystem at parse time (except `"."`).
- Clamps: threads 1–256, port 1–65535, keep-alive 0–3600, max-conns 0–1000.
- Calls `log_init(NULL)` → starts consumer thread.
- Builds `ServerConfig` from `Arguments`, calls `server_run()`.

### 4.2 `server.c` — Server Core

- **Signals**: SIGINT/SIGTERM → `g_shutdown=1`; SIGPIPE → `SIG_IGN`.
- **Listener**: `getaddrinfo()` → socket → `SO_REUSEADDR|SO_REUSEPORT` → `listen(128)`.
- **Accept loop**: blocking `accept()`, wraps fd in `Transport`, `transport_accept()` (noop), `peer_addr()` for IP, `ratelimit_accept()`, `thread_pool_submit(ClientJob)`.
- **ClientJob**: owns `Transport*`, client IP/port, copied `ServerConfig`, `RateLimit*`.
- **Shutdown**: closes listener → `thread_pool_destroy()` → `ratelimit_destroy()` → `livereload_stop()`.

### 4.3 `transport.c` — Socket Abstraction

```c
struct Transport { int fd; };  // opaque to callers
```

- `transport_write()`: handles partial writes, retries on `EINTR`.
- `transport_writev()`: `writev()` for header+body in one syscall.
- `transport_set_timeout()`: `SO_RCVTIMEO` for keep-alive.
- `transport_destroy()`: nullifies caller's pointer after `free()`.
- `TCP_NODELAY` set in `transport_new()`.
- No TLS (removed in earlier version).

### 4.4 `http.c` — Request Parser

- **Buffered reads**: 4 KB chunks into `HttpRequest.raw[8192]` until `\r\n\r\n`.
- **Request line**: method (GET/HEAD/OTHER), URI split on `?`, URL-decode path (`%XX`, `+`).
- **Path traversal check**: rejects `..` in decoded path immediately.
- **Headers**: lowercased names, extracts Host, Authorization, Connection, If-None-Match, If-Modified-Since, Range (suffix `-N` supported).
- **No body parsing** — only GET/HEAD supported.

### 4.5 `header_cache.c` — Static Header Components

- **Date**: `time()` + `gmtime_r()` + `strftime()` once/sec (double-checked locking with `pthread_mutex`).
- **Server**: `"Server: live-server/2.7.0\r\n"` built once at init.
- **Connection**: two static strings (`keep-alive` / `close`).
- `header_cache_build()`: single `snprintf` merges all pieces.

### 4.6 `response.c` — Response Builders

- `response_send()`: calls `header_cache_build()` + `transport_writev()` (header + body) or `transport_write()` (header only).
- `response_error()`: styled HTML error page with `prefers-color-scheme` dark mode.
- `response_redirect()`: 301 with `Location`.

### 4.7 `file.c` — Static File Serving (Optimized)

**Entry**: `file_serve(root, req, transport, client_ip, port, livereload_mode, print_request, keep_alive)`

1. **Method check**: GET/HEAD only → 405.
2. **Path safety**: `safe_path()` resolves against cached `g_real_root` (realpath once at init). Prefix check prevents traversal. Falls back to dirname resolution for non-existent files.
3. **Directory handling**: no trailing `/` → 301 redirect; trailing `/` → look for `index.html`; else 404.
4. **`send_file()` core**:
   - **ETag**: hand-rolled hex: `"<mtime-hex>-<size-hex>"` (e.g. `"1a2b3c4d-5678"`).
   - **Conditional GET**: `If-None-Match` / `If-Modified-Since` → 304.
   - **Live-reload injection** (HTML + mode enabled):
     - Read entire file into **thread-local buffer** (`thread_buffer_get_body()`).
     - Single-pass scan for `</body>` / `</BODY>` → inject `<script>EventSource('/livereload')</script>`.
     - `Cache-Control: no-store`.
   - **Range requests**: parses `bytes=N-M`, `N-`, `-N`; validates; seeks with `lseek()`.
   - **Linux `sendfile()`** (non-range GET): writes header via `transport_write()`, then `sendfile()` kernel→socket. Falls back to read/write for range/HEAD/non-Linux.
   - **Fallback path**: reads into thread-local buffer, `response_send()` with `writev()`.

**Access logging**: `LOG_INFO` with IP, method, path, version, status, bytes, MIME. `--print-request` → raw headers at `LOG_DEBUG`.

### 4.8 `thread_buffer.c` — TLS Reusable Buffers

- `__thread ThreadBuffers* g_tls_buffers` per worker.
- `thread_buffer_get_body(min_size)`: grows 2× (min 4 KB), never shrinks.
- `thread_buffer_cleanup()`: frees on thread exit (not currently called by pool workers — OS reclaims at process exit).

### 4.9 `auth.c` — Basic Auth

- `auth_check()`: extracts `Authorization: Basic <b64>`, decodes (hand-rolled), splits on `:`, compares.
- `auth_send_challenge()`: 401 + `WWW-Authenticate: Basic realm="live-server"`.
- No external deps.

### 4.10 `thread_pool.c` — Fixed-Size Pool

- Circular queue: 4096 `Task { void (*func)(void*), void* arg }`.
- Mutex + `not_empty` + `not_full` condvars.
- Workers: `lock → wait(not_empty) → dequeue → unlock → func(arg)`.
- `thread_pool_submit()`: blocks if queue full (backpressures accept loop).
- `thread_pool_destroy()`: `stop=1`, broadcast both condvars, join all.

### 4.11 `ratelimit.c` — Per-IP Connection Limit

- **Dynamic open-addressing hash table** (djb2, linear probe).
- Starts at 256 slots, grows 2× when `used * 4 >= size * 3` (75% load).
- `RLEntry { char* ip; int count; }` — IP strings owned, freed at count=0.
- `ratelimit_accept()`: increments, returns 0/-1.
- `ratelimit_leave()`: decrements, frees entry at 0.
- Single mutex covers all ops.
- **Integer load check**: `used * 4 >= size * 3` (no float).

### 4.12 `livereload.c` — SSE Management

- **Registry**: 1024-bucket hash map (separate chaining) → `SSEClient { Transport* t; char ip[INET6_ADDRSTRLEN]; int port; SSEClient* next; }`.
- `livereload_start()`: allocates registry, copies mode, creates watcher, starts it.
- `livereload_handle_sse()` (per-client pthread):
  1. Write SSE headers (`text/event-stream`, `no-cache`, `Access-Control-Allow-Origin: *`).
  2. `lr_add_client()` → insert into registry.
  3. Heartbeat loop: `: heartbeat\n\n` every 15s via `transport_write()`. On write failure → remove client.
  4. On exit: `lr_remove_client()`.
- `livereload_broadcast(event)`: iterates all buckets, writes `data: <event>\n\n`. Failed writes → client removed.
- Watcher callback → `livereload_broadcast("reload")` or `"hard-reload"`.

### 4.13 `watcher.c` — File System Watcher

**Three backends (compile-time):**

| Platform | Default | Fallback |
| -------- | ------- | -------- |
| Linux    | inotify | `--poll` |
| macOS    | kqueue  | `--poll` |

**inotify (Linux):**

- `inotify_init1(IN_NONBLOCK)` → recursive `inotify_add_watch()` on all dirs.
- Thread blocks on `select()` (inotify fd + wake pipe).
- Events: `IN_CREATE|IN_DELETE|IN_MODIFY|IN_MOVED_FROM|IN_MOVED_TO|IN_CLOSE_WRITE`.
- New dir created (`IN_CREATE|IN_MOVED_TO` + `S_ISDIR`) → recursive add.
- Wake pipe byte → unblocks `select()` on shutdown.

**kqueue (macOS):**

- `kqueue()` → `open(path, O_EVTONLY|O_DIRECTORY)` for each dir.
- `EV_SET(..., EVFILT_VNODE, EV_ADD|EV_CLEAR, NOTE_DELETE|NOTE_WRITE|NOTE_EXTEND|NOTE_ATTRIB|NOTE_LINK|NOTE_RENAME|NOTE_REVOKE, ...)`
- Thread blocks on `kevent()` with 2s timeout + wake pipe (`EVFILT_READ|EV_ONESHOT`).
- On `NOTE_WRITE|NOTE_LINK|NOTE_RENAME` → scan dir for new subdirs → add watches.
- On `NOTE_DELETE|NOTE_RENAME` (dir itself) → remove watch.

**Poll fallback:**

- Snapshot all files + mtimes → `PollState { PollEntry* entries; int count, cap; }`.
- `PollMap` (heap-allocated, 4096 slots): full-path keys (truncated to 63 chars) → mtime.
- Every 500ms: new snapshot → O(N) diff via hash map (count mismatch OR mtime mismatch).
- `select()` on wake pipe for interruptible sleep.
- Calls callback with `path=NULL` (knows _something_ changed, not _what_).

### 4.14 `mime.c` — MIME Lookup

- Static `struct { const char* ext; const char* mime; } table[]`.
- Case-insensitive `strcasecmp()` on extension (after last `.`).
- Fallback: `application/octet-stream`.
- Covers: HTML, CSS, JS/TS, JSON, XML, images (PNG, JPEG, GIF, ICO, WebP, AVIF, BMP, TIFF, SVG), fonts (WOFF, WOFF2, TTF, OTF), audio (MP3, OGG, Opus, WAV, FLAC, AAC), video (MP4, WebM, OGV, MOV, AVI), text (TXT, MD, CSV), PDF, archives (ZIP, GZ, TAR), WASM.

### 4.15 `log.c` — Lock-Free Ring Logger

- **Levels**: `LOG_LEVEL_ERROR` (0), `WARN` (1), `INFO` (2), `DEBUG` (3).
- **Ring**: 4096 `LogSlot { char buf[256]; int len; atomic_int ready; }`.
- **Producers (workers)**: `atomic_fetch_add(&head, 1)` → format full line (timestamp, level, file:line, message) into slot → `atomic_store(&slot.ready, 1)`.
- **Consumer (dedicated thread)**: busy-waits `ready` flag → `fwrite()` batch → `fflush()`.
- **Timestamp**: captured at call site (accurate), not in consumer.
- **Colours**: ANSI auto-detected via `isatty(fileno(stderr))`. File output → no colours.
- **Macros**: `LOG_ERROR()`, `LOG_WARN()`, `LOG_INFO()`, `LOG_DEBUG()`, `LOG_PERROR()` (adds `strerror(errno)`).

---

## 5. Request Lifecycle (Happy Path)

```
1. main thread: accept() → cfd
2. transport_new(cfd) → Transport* (TCP_NODELAY set)
3. peer_addr() → client_ip:port
4. ratelimit_accept(ip) → OK
5. ClientJob* malloc'd, filled
6. thread_pool_submit(handle_client, job)

Worker thread:
7. http_parse_request() → HttpRequest (4 KB buffered read)
8. If auth configured → auth_check() → 401 or continue
9. If path == "/livereload" → SSE path:
   a. malloc SSEJob, pthread_create(sse_worker)
   b. sse_worker: write SSE headers, lr_add_client(), 15s heartbeat loop
   c. main handle_client() returns (t == NULL)
10. Else: file_serve()
    a. Method check (GET/HEAD)
    b. safe_path() → fs_path (cached realpath root)
    c. stat() → directory? redirect/index.html/404 : not regular? 403
    d. send_file():
       i. build_etag() (hand-rolled hex)
       ii. Conditional GET? → 304
       iii. HTML + livereload? → read into TLS buffer, single-pass </body> inject
       iv. Range? → validate, lseek()
       v. Linux non-range GET? → sendfile() zero-copy
       vi. Else → read into TLS buffer, response_send() via writev()
11. response_send() → header_cache_build() + transport_writev()
12. keep_alive? → transport_set_timeout() → loop to step 7
13. cleanup: ratelimit_leave(), transport_destroy(), free(job)
```

---

## 6. Build System

### `Makefile` (top-level)

```make
CC = clang
CFLAGS = -Wall -Wextra -Wpedantic -Wstrict-prototypes -Wmissing-prototypes \
         -Wshadow -Wconversion -Wno-missing-field-initializers -std=c17 -Isrc
# Compile-time log features (always on):
CFLAGS += -DLOG_SHOW_TIME_STAMP -DLOG_SHOW_SOURCE_LOCATION

# macOS
LDFLAGS += -largp
# Linux
CFLAGS += -D_GNU_SOURCE

# Debug target:
make debug O_DEBUG=1
# → -g3 -DDEBUG -fstack-usage -fsanitize=address -fsanitize=undefined -ffreestanding

# Release:
make            # -O3
make strip      # -O3 + strip symbols
sudo make install            # /usr/local/bin
sudo make install PREFIX=~/.local  # ~/.local/bin
```

### `tests/Makefile`

- Unit tests: C++17 + doctest, links all `build/src/*.o` **except** `main.o`.
- `make test` → builds main binary first, then runs doctest binaries.
- Integration tests: `bash tests/test_server.sh [binary] [port]` (23 tests via curl).

### No dependency tracking

- Headers not in `Makefile` deps. Relies on `.c → .o` timestamps. Run `make clean` after header changes.

---

## 7. Testing

| Suite       | Command                                        | What It Covers                                                                                                                                        |
| ----------- | ---------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| Unit        | `make test`                                    | `mime_from_path()` — 54 assertions (extensions, case-insensitivity, fallback, edge cases)                                                             |
| Integration | `bash tests/test_server.sh ./live-server 9999` | 23 tests: 200/301/304/404/405/416, ETag, ranges, keep-alive headers, Basic Auth (missing/correct), path traversal (3 variants), `--help`, `--version` |

**Test server flags**: `-L error -T 2 -K 5` (quiet, 2 threads, 5s keep-alive).

---

## 8. Key Design Decisions (Rationale)

| Decision                          | Why                                                                                    |
| --------------------------------- | -------------------------------------------------------------------------------------- |
| Blocking I/O + thread pool        | Simpler than async; POSIX threads universal; pool prevents fork bomb                   |
| 4 KB buffered header reads        | Reduces syscalls from O(header_bytes) to ~1; avoids partial-read complexity            |
| Entire file in memory for serving | Simplicity; files are small (dev assets); live-reload injection needs full body anyway |
| No directory listing              | Intentional. No `index.html` → 404.                                                    |
| No compression                    | Complicates live-reload injection (would need decompress→inject→recompress)            |
| No TLS                            | Zero-dep promise. Use reverse proxy (Caddy, nginx, mkcert) for HTTPS                   |
| Poll on macOS (historical)        | Now **kqueue** default. Poll remains via `--poll` flag or kqueue failure               |
| Opaque `Transport`                | Future TLS swap without touching callers                                               |
| Dynamic hash table (ratelimit)    | Avoids fixed 1024-slot degradation under churn                                         |
| Lock-free ring logger             | Eliminates mutex contention under load; consumer batches I/O                           |
| Pre-computed headers              | Date/Server/Connection formatted once/sec, not per-request                             |
| TLS buffers                       | Eliminates allocator contention on hot path                                            |
| `writev()` header+body            | Single syscall, fewer TCP segments                                                     |
| `sendfile()` (Linux)              | Zero-copy kernel→socket for static files                                               |
| Hand-rolled ETag hex              | Avoids `snprintf` in hot path                                                          |
| Single-pass `</body>`             | Replaces two `strcasestr()` calls                                                      |
| Full-path keys in poll map        | Fixes basename-collision bug (different dirs, same filename)                           |
| Heap `PollMap`                    | 304 KB too large for macOS 512 KB default thread stack                                 |

---

## 9. Performance Characteristics

| Metric                         | Mechanism                       | Impact                                                             |
| ------------------------------ | ------------------------------- | ------------------------------------------------------------------ |
| Static file throughput (Linux) | `sendfile()` zero-copy          | 2–5× vs read/write                                                 |
| Per-request alloc              | Thread-local grow-only buffers  | Near-zero allocator contention                                     |
| Logging overhead               | Lock-free ring + batched fwrite | ~50 ns / call under load                                           |
| Header formatting              | Cached Date/Server/Connection   | Saves 3× `snprintf` + `time()` + `gmtime_r()` + `strftime()` / req |
| Syscall count (response)       | `writev()` header+body          | 1 syscall vs 2+                                                    |
| Rate limiter scaling           | Dynamic table, grows 2× at 75%  | No fixed-slot degradation                                          |
| SSE broadcast                  | 1024-bucket hash map            | O(1) vs O(N) linear scan                                           |
| Poll watcher diff              | Hash map lookup                 | O(N) vs O(N²) nested loop                                          |

---

## 10. Known Limitations (By Design)

- **No HTTPS** — pair with Caddy/nginx/mkcert.
- **No compression** — `gzip`/`brotli` would break live-reload injection.
- **No directory listing** — missing `index.html` → 404.
- **No HTTP/2** — single-threaded accept + thread pool is HTTP/1.1 only.
- **Whole-file memory for injection** — not a streaming server; unsuitable for multi-GB files.
- **No config file / env vars** — all CLI flags (explicit, reproducible).
- **No WebSocket** — SSE covers reload use case; bidirectional not needed.
- **No metrics endpoint** — not an observability target.

---

## 11. Version History (Current: 2.7.0)

| Version | Changes                                                                                                                                                                                                                              |
| ------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 2.7.0   | kqueue backend (macOS), heap PollMap, full-path poll keys, double ratelimit_leave fix, FD leak fix, snprintf guards, build_etag bounds, ratelimit integer math, zero warnings under ASan+UBSan, DEV.md rewrite, D2 diagrams, manpage |
| 2.x     | sendfile, writev, header_cache, thread_buffer, dynamic ratelimit, SSE hash map, lock-free logger, buffered reads, single-pass body scan, cached realpath, poll O(N) diff                                                             |

---

## 12. Files an Agent Might Need to Touch

| Task                          | Files                                                                                          |
| ----------------------------- | ---------------------------------------------------------------------------------------------- |
| Add CLI flag                  | `main.c` (argp options + `parse_opt`), `server.h` (ServerConfig), `server.c` (config plumbing) |
| New MIME type                 | `mime.c` (table entry)                                                                         |
| New log level                 | `log.h` (enum), `log.c` (macro + colour)                                                       |
| New watcher backend           | `watcher.c` (new `#elif` block + thread fn), `watcher.h` (if new struct fields)                |
| Change thread pool queue size | `thread_pool.c` (QUEUE_CAPACITY)                                                               |
| Adjust rate limiter growth    | `ratelimit.c` (RL_INITIAL_SIZE, RL_MAX_LOAD_FACTOR, RL_GROWTH_FACTOR)                          |
| Modify live-reload script     | `file.c` (LIVERELOAD_SCRIPT constant)                                                          |
| Add response header           | `header_cache.c` (if static) or `response.c` (if dynamic)                                      |

---

## 13. Mental Model Checklist for Agents

- [ ] Single binary, no runtime deps, CLI-only config
- [ ] Main thread = accept + dispatch only
- [ ] Workers = full request lifecycle (parse → serve → keep-alive)
- [ ] SSE clients = 1 pthread each, detached
- [ ] Log consumer = 1 pthread, drain ring buffer
- [ ] Watcher = 1 pthread (inotify/kqueue/poll)
- [ ] All shared state protected by mutexes (ratelimit, header_cache, livereload registry, thread pool queue)
- [ ] Lock-free only in logger (SPSC ring)
- [ ] TLS only for thread-local body buffers
- [ ] `Transport` is the **only** way to touch sockets
- [ ] `sendfile()` only on Linux, non-range GET, regular files
- [ ] `writev()` for all other responses
- [ ] No streaming request bodies (GET/HEAD only)
- [ ] No directory listing, no compression, no TLS, no HTTP/2

---

## 14. Quick Commands

```bash
# Build release
make

# Build debug (ASan+UBSan)
make debug O_DEBUG=1

# Run integration tests
bash tests/test_server.sh ./live-server 9999

# Run unit tests
make test

# Install
sudo make install           # /usr/local/bin
sudo make install PREFIX=~/.local  # ~/.local/bin

# Manual test
./live-server -I ./test_data -P 8080 -U -L debug
```

---

_Generated from codebase inspection. Update when architecture changes._
