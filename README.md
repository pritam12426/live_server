# live-server

A lightweight static file server with live reload, written in C. No runtime dependencies beyond POSIX — just build and run.

## Features

- Serves static files over HTTP with correct MIME types
- Live reload via Server-Sent Events — browser refreshes automatically on file save
- Hard reload mode — cache-busting reload for when a soft refresh isn't enough
- Conditional requests (ETag, If-Modified-Since) and byte-range support
- Basic Authentication (username/password)
- Keep-alive connections for multiple requests per TCP connection
- Thread pool (fixed-size, configurable) replaces pthread-per-connection
- Rate limiting — cap concurrent connections per client IP
- Graceful shutdown on SIGINT/SIGTERM
- Access log with client IP, method, path, and status on every request
- Cross-platform: macOS and Linux
- Colored, leveled logging with millisecond timestamps and source location

## Requirements

| Platform | Toolchain                                                                                    |
| -------- | -------------------------------------------------------------------------------------------- |
| Linux    | gcc, make                                                                                    |
| macOS    | clang (Xcode CLT), make, [argp-standalone](https://formulae.brew.sh/formula/argp-standalone) |

On macOS, install argp-standalone first:

```sh
brew install argp-standalone
```

## Build

```sh
make strip            # release build (-O3)
make debug            # debug build   (-g3 -DDEBUG, ASan+UBSan)
```

The binary is placed at `./live-server`.

## Project Structure

```
live-server
├── Makefile
├── tests/
│   ├── test_server.sh       # Integration test suite
│   └── test_data/           # Fixture files for tests
├── src/
│   ├── main.c               # CLI entry point, argp parsing
│   ├── server.c / .h        # Accept loop, thread pool, keep-alive, rate limiting
│   ├── transport.c / .h     # Opaque Transport type (plain socket wrapper)
│   ├── http.c / .h          # HTTP request parser, URL decoding
│   ├── response.c / .h      # HTTP response formatting and sending
│   ├── file.c / .h          # Static file serving, range support, live-reload injection
│   ├── thread_pool.c / .h   # Fixed-size thread pool with work queue
│   ├── ratelimit.c / .h     # Per-IP connection rate limiting
│   ├── auth.c / .h          # Basic Authentication (Base64 decode, credential check)
│   ├── livereload.c / .h    # SSE client management, broadcast, watcher glue
│   ├── watcher.c / .h       # File-system watcher (inotify on Linux, poll fallback on macOS)
│   ├── mime.c / .h          # MIME type detection by file extension
│   ├── log.c / .h           # Leveled, timestamped, colored logging
│   ├── types.h              # Shared type definitions (LivereloadMode)
│   └── project_config.h     # Build-time metadata (version, name, URL)
└── LICENSE
```

## Install

```sh
sudo make strip install                   # installs to /usr/local/bin
sudo make strip install PREFIX=~/.local   # installs to ~/.local/bin
```

To uninstall:

```sh
sudo make uninstall
```

## Usage

```sh
live-server [OPTIONS]
```

| Option               | Short | Default     | Description                                                     |
| -------------------- | ----- | ----------- | --------------------------------------------------------------- |
| `--dir`              | `-I`  | `.`         | Directory to serve                                              |
| `--port`             | `-P`  | `8080`      | Port to listen on                                               |
| `--host`             | `-H`  | `localhost` | Host to bind to                                                 |
| `--threads`          | `-t`  | `4`         | Thread pool size (1–256)                                        |
| `--keep-alive`       | `-k`  | `5`         | Keep-alive timeout in seconds (0 = disable)                     |
| `--max-conns`        | `-M`  | `0`         | Max concurrent connections per IP (0 = unlimited)               |
| `--browser`          | `-B`  | —           | Browser to open automatically                                   |
| `--log-level`        | `-L`  | `info`      | Log level: `error` `warn` `info` `debug`                        |
| `--print-request`    | `-R`  | —           | Log full request headers per connection                         |
| `--live-reload`      | `-U`  | —           | Reload browser automatically on file change                     |
| `--live-hard-reload` | `-W`  | —           | Force cache-busting reload on file change                       |
| `--user`             | `-u`  | —           | Enable Basic-Auth with this username (default: `admin`)         |
| `--pass`             | `-p`  | —           | Enable Basic-Auth with this password (default: `admin`)         |
| `--poll`             | `-o`  | —           | Use poll watcher instead of inotify (Linux; macOS always polls) |
| `--ignore`           | `-i`  | —           | Skip hidden files (dot-prefixed) in watcher                     |

> `--live-reload` and `--live-hard-reload` are mutually exclusive.

### Examples

Serve the current directory on the default port:

```sh
live-server
```

With a custom thread pool and keep-alive:

```sh
live-server -t 8 -k 10
```

Cap connections per IP:

```sh
live-server -M 10
```

Serve a build output folder on a custom port:

```sh
live-server -I ./dist -P 3000
```

Enable live reload while developing:

```sh
live-server -I ./site -U
```

Force a hard reload (bypasses browser cache — useful for CSS/JS changes):

```sh
live-server -I ./site -W
```

Open in browser automatically on startup:

```sh
live-server -I ./site -B open       # macOS
live-server -I ./site -B xdg-open  # Linux
```

Expose on the local network:

```sh
live-server -H 0.0.0.0 -P 8080
```

Enable Basic Authentication with default credentials (`admin`/`admin`):

```sh
live-server -p
```

Set custom credentials:

```sh
live-server -u=myuser -p=mypass
```

Skip hidden files in the file watcher:

```sh
live-server -i -U
```

## How Live Reload Works

Every served HTML page is automatically injected with a small `<script>` that holds open a persistent SSE connection to `/livereload`. When any file in the served directory changes, the watcher signals all connected browsers.

No browser extension or manual script tag needed — it's injected at serve time.

Two reload modes are available:

| Flag                        | Behaviour                                                                                  |
| --------------------------- | ------------------------------------------------------------------------------------------ |
| `--live-reload` (`-U`)      | Calls `window.location.reload()` — fast, uses browser cache                                |
| `--live-hard-reload` (`-W`) | Appends a `_lr=<timestamp>` cache-bust param and navigates — forces re-fetch of all assets |

The watcher backend:

- **Linux** — `inotify` by default, `--poll` (`-o`) to use poll fallback (useful over NFS/sshfs)
- **macOS** — poll always (no inotify on macOS)

## Access Log

Every served request is logged with a timestamp, client IP, method, path, and response status:

```
[12:34:56.789012] [INFO ] 127.0.0.1:54321 "GET /index.html HTTP/1.1" 200 - (3012 bytes, text/html)
[12:34:57.123456] [INFO ] 127.0.0.1:54322 "GET /style.css HTTP/1.1" 200 - (812 bytes, text/css)
[12:34:58.000001] [WARN ] 127.0.0.1:54323 "GET /missing.js HTTP/1.1" 404
```

Use `--print-request` (`-R`) to additionally dump the full raw headers of each request.

## Make Targets

| Target                 | Description                     |
| ---------------------- | ------------------------------- |
| `make`                 | Release build with `-O3`        |
| `make debug O_DEBUG=1` | Debug build with `-g3 -DDEBUG`  |
| `make install`         | Install binary                  |
| `make uninstall`       | Remove installed files          |
| `make strip`           | Strip debug symbols from binary |
| `make clean`           | Remove build artifacts          |
| `make help`            | Show all targets                |

## Tests

```sh
bash tests/test_server.sh          # default port 9999
bash tests/test_server.sh ./live-server 3000   # custom binary and port
```

Runs 26 integration tests covering status codes, conditional requests, ranges, keep-alive, authentication, path traversal, and CLI flags.

## License

[MIT](LICENSE)
