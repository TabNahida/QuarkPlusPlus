# Architecture

```text
CLI ─────────────┐
                ├── QuarkClient (quarkpp_core) ── HttpClient ── PACE 0.4.0 / cpp-httplib
WebDAV adapter ──┘             │
       ↑                      ├── Quark JSON / wg APIs
  rclone mount                └── OSS multipart / signed download URLs
```

The public library is under `include/quarkpp`. `src/quark_client.cpp` owns provider behavior; `src/support.cpp` adapts PACE's direct `pace::http::fetch` API for buffered requests and cpp-httplib for streaming transfers, together with URL/header handling, hashes, and filesystem conversions. PACE 0.4.0 does not expose the client streaming callbacks required by the transfer and WebDAV adapters. Neither component parses command-line arguments nor writes terminal output. CLI configuration and formatting live in `src/main.cpp`; WebDAV is a separate adapter in `src/webdav.cpp`.

## Contracts

`QrLoginClient` in `include/quarkpp/auth.hpp` handles QR creation, status polling,
ticket exchange, and authenticated drive cookies without terminal output. The
CLI renders QR codes locally and stores the session in its selected config;
embedding applications choose how to display the URL and persist credentials.

- C++20, 64-bit `size_t` and file offsets. Windows paths use UTF-8 conversion at the boundary.
- Synchronous methods, explicit configuration, per-operation HTTP connections, caller-supplied progress callbacks. Callback exceptions abort the operation. The API does not provide a dedicated cancellation token.
- Batch-download callbacks are serialized and report monotonic aggregate bytes over the whole selection, including existing resumed bytes. Worker lifetimes finish before a batch returns or propagates a failure.
- API-issued `__puus` refreshes are retained in a mutex-protected session shared by copies of a client. `config()` remains the original configuration; no refreshed credentials are persisted to disk. File reads use headers obtained after their download URL request.
- The HTTP layer caps buffered API replies at 16 MiB. File bodies use streaming callbacks; upload read buffers are 256 KiB. Directory lists and offline previews are ordinary in-memory metadata.
- TLS verifies chains and hostnames. Cross-origin redirects strip credentials and custom headers; HTTPS-to-HTTP redirects are rejected. No raw credential logging.
- Library errors report transport/provider failure. Downloads validate server ranges and remote size before appending. Recursive extraction rejects traversal, unsafe Windows names, symlinks, and duplicate target paths.
- API endpoints can be overridden in `AppConfig` for an HTTP mock server. Production defaults use HTTPS. Endpoint overrides are trusted configuration and receive the account cookie.

## Download scheduling

`download()` delegates to `download_batch()`. Selections are expanded recursively and all target paths are checked before file transfers begin, preserving directory structure and rejecting collisions. Download links are requested in groups of at most `download_batch_size` (default 20); retries refresh the failed file's link individually.

Files at or below `small_file_threshold` (default 8 MiB) use one streaming connection each, with at most `download_jobs` files active (default 4). Larger files run one at a time and use `download_threads` range workers (default 4). Setting the threshold to zero leaves only empty files in the small-file group.

Large-file ranges are bounded to 1 MiB and collected in waves. Each response must match the exact range, total size and identity content encoding before a contiguous wave is appended to the destination. No sparse destination or persistent segment sidecars are created. After interruption, the destination remains a contiguous prefix that the next attempt can resume. Range data occupies at most `download_threads` MiB per large file, in addition to ordinary stream buffers and metadata. A fresh download can fall back to a single stream when the origin does not support ranges; an existing prefix is never overwritten by an ignored resume request.

`--threads`, `--jobs`, `--batch-size`, and `--small-file-threshold` override the corresponding configuration for a CLI download. Repeated `--fid` and `--path` selections share one batch and progress total.

## Multipart upload

Compute hashes → `/file/upload/pre` → `/file/update/hash` → either finish as a fast upload, or obtain per-request OSS signatures → stream each part → merge ETags → `/file/upload/finish`.

Part sizes come from the provider. Retries re-open the same file range. An upload source must stay unchanged. State is not persisted between processes; the library does not claim resumable upload sessions or parallel part scheduling.

## cpp-httplib compatibility patch

cpp-httplib 0.34.0 rejects unknown HTTP methods before routing. `scripts/httplib.lua` generates a project-local copy of its installed header and extends the method allowlist with `PROPFIND`, `MKCOL`, and `MOVE`. It also suppresses the automatic `Content-Length: 0` header on empty HTTP 204 responses: that header is forbidden for 204 and rejected by PACE's strict parser. This keeps the local WebDAV server and loopback fixtures interoperable with the PACE client.

The pinned version and exact replacements make unexpected upstream changes fail visibly. Every target uses this generated header to keep inline definitions consistent. No global package file is edited.

WebDAV exposes a deliberately limited filesystem contract, described in [mounting.md](mounting.md). Temporary upload storage is owned by the adapter, not the core library.

## Build outputs

`quarkpp_core` produces `quarkpp.lib` on Windows or `libquarkpp.a` on Unix. `quarkpp` produces the CLI. Public headers install below `include/quarkpp`; docs and config examples install below `share/quarkpp`. The CLI links PACE, OpenSSL and c-ares statically. SDK consumers supply PACE 0.4.0 configured with HTTP/1, TLS and DNS enabled and WebSocket disabled, OpenSSL 3.6.1, c-ares 1.34.8, and nlohmann/json 3.12.0.

The PACE static modules are `pace_http1`, `pace_tls`, `pace_dns`, `pace_net` and `pace_runtime`, linked in that order. System dependencies include `ws2_32`, `bcrypt`, `crypt32`, `iphlpapi`, `advapi32` and `user32` on Windows; Unix needs `pthread`, and macOS also needs `resolv` plus the Security and CoreFoundation frameworks. Windows and macOS API requests import native trusted certificates into PACE's immutable TLS configuration; streaming requests use cpp-httplib's corresponding native certificate support. Match the build compiler ABI and runtime (MSVC `/MT` on Windows). Third-party SDK archives are not bundled with the QuarkPlusPlus core library. Tests are opt-in targets discovered by `xmake test`.

The old internal headers, libcurl dependency, C++23 requirement, and unrelated growth/sharing commands have been removed. The GPLv3 license is preserved.
