# Architecture

```text
CLI ─────────────┐
                ├── QuarkClient (quarkpp_core) ── HttpClient ── cpp-httplib + OpenSSL
WebDAV adapter ──┘             │
       ↑                      ├── Quark JSON / wg APIs
  rclone mount                └── OSS multipart / signed download URLs
```

The public library is under `include/quarkpp`. `src/quark_client.cpp` owns provider behavior; `src/support.cpp` owns HTTP streaming, URL/header handling, hashes, and filesystem conversions. Neither component parses command-line arguments nor writes terminal output. CLI configuration and formatting live in `src/main.cpp`; WebDAV is a separate adapter in `src/webdav.cpp`.

## Contracts

`QrLoginClient` in `include/quarkpp/auth.hpp` handles QR creation, status polling,
ticket exchange, and authenticated drive cookies without terminal output. The
CLI renders QR codes locally and stores the session in its selected config;
embedding applications choose how to display the URL and persist credentials.

- C++20, 64-bit `size_t` and file offsets. Windows paths use UTF-8 conversion at the boundary.
- Synchronous methods, explicit configuration, per-operation HTTP connections, caller-supplied progress callbacks. Callback exceptions abort the operation. The API does not provide a dedicated cancellation token.
- API-issued `__puus` refreshes are retained in a mutex-protected session shared by copies of a client. `config()` remains the original configuration; no refreshed credentials are persisted to disk. File reads use headers obtained after their download URL request.
- The HTTP layer caps buffered API replies at 16 MiB. File bodies use streaming callbacks; upload read buffers are 256 KiB. Directory lists and offline previews are ordinary in-memory metadata.
- TLS verifies chains and hostnames. Cross-origin redirects strip credentials and custom headers; HTTPS-to-HTTP redirects are rejected. No raw credential logging.
- Library errors report transport/provider failure. Downloads validate server ranges and remote size before appending. Recursive extraction rejects traversal, unsafe Windows names, symlinks, and duplicate target paths.
- API endpoints can be overridden in `AppConfig` for an HTTP mock server. Production defaults use HTTPS. Endpoint overrides are trusted configuration and receive the account cookie.

## Multipart upload

Compute hashes → `/file/upload/pre` → `/file/update/hash` → either finish as a fast upload, or obtain per-request OSS signatures → stream each part → merge ETags → `/file/upload/finish`.

Part sizes come from the provider. Retries re-open the same file range. An upload source must stay unchanged. State is not persisted between processes; the library does not claim resumable upload sessions or parallel part scheduling.

## WebDAV compatibility patch

cpp-httplib 0.34.0 rejects unknown HTTP methods before routing. `scripts/httplib.lua` generates a project-local copy of its installed header and extends the method allowlist with `PROPFIND`, `MKCOL`, and `MOVE`. The pinned version and exact replacement make unexpected upstream changes fail visibly. Every target uses this generated header to keep inline definitions consistent. No global package file is edited.

WebDAV exposes a deliberately limited filesystem contract, described in [mounting.md](mounting.md). Temporary upload storage is owned by the adapter, not the core library.

## Build outputs

`quarkpp_core` produces `quarkpp.lib` on Windows or `libquarkpp.a` on Unix. `quarkpp` produces the CLI. Public headers install below `include/quarkpp`; docs and config examples install below `share/quarkpp`. OpenSSL and nlohmann/json remain consumer dependencies. Tests are opt-in targets discovered by `xmake test`.

The old internal headers, libcurl dependency, C++23 requirement, and unrelated growth/sharing commands have been removed. The GPLv3 license is preserved.
