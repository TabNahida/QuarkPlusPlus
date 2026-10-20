# Testing and verification

## Verification record — 2026-10-20

- Terminal QR login: the user scanned the locally generated QR code and confirmed in the Quark app. The CLI exchanged the ticket, obtained drive credentials, and saved a private local config. New CLI processes successfully queried the account and listed the root using only that saved QR session. No manually copied Cookie was used for this check. Regression tests cover the account endpoint's actual string `code:"OK"`, legacy numeric zero, and rejection of invalid status types. Sanitized evidence is retained locally in `build/live-verification/qr-login.json`.

- Windows x64: xmake 3.1.1, MSVC C++20, cpp-httplib 0.34.0, OpenSSL 3.6.1. Release CLI/library build, all three test suites, CLI help/version, and staged installation passed.
- Linux x64: Ubuntu 24.04 under WSL, GCC 13.3, OpenSSL 3.0.13, the same cpp-httplib header. Separate compiler/linker builds of CLI and all three test suites passed. This was a direct compiler check, not a Linux xmake package-resolution run.
- After the live multipart fix and optional CLI progress output were added, Windows/xmake and Linux/GCC builds and all three offline suites passed again. The client regression now rejects an accidental parallel preflight declaration and verifies that a part-2 `NoHashContext` error includes useful context without sensitive OSS response fields.
- Download-link issuance and file reads now consistently use the desktop-client User-Agent. Regression checks cover URL requests, actual byte GETs, adapter download headers, and preservation of a configured browser identity for normal APIs. This corrects the previous large-file fallback, which changed only the URL request identity.
- Session-refresh regressions verify repeated `Set-Cookie` fields, refreshed `__puus` propagation to subsequent API requests and byte downloads, preservation of unrelated credentials, invalid-value rejection, and refusal to update from redirects outside the configured API bases. Refreshes remain in memory and do not modify the caller's config.
- After both download authentication fixes, Windows/xmake and Linux/GCC builds and all three suites passed. A real 250,000,000-byte normal upload and interrupted-download resume passed complete SHA-256 verification. The resumed body averaged approximately 18.679 MiB/s; details and measurement boundaries appear below.
- Windows rclone 1.75.2: the official client passed `lsf`, `cat`, a range read at offset 5,368,709,129, and `copyto` with a matching post-upload size against the mock WebDAV server. No native driver or mount was involved.
- Real Quark account: account query, directory listing, a 67-byte browser-uploaded file downloaded by the C++ CLI, empty-file upload/download, and a 1,048,609-byte random-file upload/download passed. The random upload reported `fast_upload:false`; SHA-256 matched after download.
- Real cloud BT: parsed the official WebTorrent Sintel magnet, selected English subtitles (`file_no:1`), observed task status `2`, and downloaded the resulting 1,514-byte file. Its SHA-256 matched the official WebSeed. Details are below.
- Real WebDAV with Windows rclone 1.75.2: directory listing, a 1,048,609-byte download with SHA-256 comparison, a 64-byte read at offset 4,096, a 131,089-byte upload/download with SHA-256 comparison, same-directory rename, and recoverable deletion passed. The generated WebDAV file was removed and the loopback server stopped. This was an rclone client test, not a native filesystem mount.
- The opt-in `scripts/live-smoke.ps1` runner passed a real-account run with an empty file and `small-测试.bin` (1,048,609 bytes), including metadata validation, full-download hashes and resume from a known half-file prefix. Its dedicated nested directory was recycled and confirmed absent from its parent afterward.
- Real large upload: a generated 4,311,744,513-byte file (4 GiB + 16 MiB + 1) completed normal multipart upload (`fast_upload:false`) in 1,523.82 seconds. The sampled peak process working set was 18,440,192 bytes (17.6 MiB). A separate real WebDAV read returned 256 matching bytes at offset 4,294,968,320, beyond 4 GiB.
- The 4.02 GiB whole-file download was stopped at the user's request after changing the round-trip fixture to 250 MB. It actually resumed from 90,271,744 bytes and reached 129,146,880 bytes, but did not complete full-file hashing. The large upload and high-offset WebDAV results above remain verified; a full 4.02 GiB download is not claimed.
- macOS compilation and native OS mounts remain unverified. See the live acceptance procedure below.

## Offline regression suite

```sh
xmake f -m release --tests=y -y
xmake
xmake test -v
```

The suite binds to ephemeral loopback ports, uses generated temporary files, and never reads a real cookie. A sandbox must allow loopback sockets. It has five executables:

| Suite | Coverage |
| --- | --- |
| `quarkpp_transport_tests` | Streaming, interrupted downloads, invalid/ignored/mismatched ranges, 416 handling, redirected credential stripping, response caps, hashes, sparse offsets beyond 4 GiB |
| `quarkpp_client_tests` | Pagination, strict provider errors, directory formats, multipart retries and offsets, fast/empty uploads, recursive paths, wg offline payloads, selected files and task failures |
| `quarkpp_webdav_tests` | Authentication, PROPFIND, escaped names, traversal, GET/HEAD/ranges, writes and overwrites, MKCOL/MOVE/DELETE, preconditions, read-only mode |
| `quarkpp_auth_tests` | QR creation, pending/expired/cancelled states, ticket exchange, cookie scope and refresh, redirect restrictions, retries and credential-free errors |
| `quarkpp_login_tests` | Local QR rendering, preserving config fields, atomic replacement, private file permissions, malformed input and link rejection |

The offline >4 GiB regression tests use sparse file offsets and small payloads. They verify 64-bit arithmetic and streaming behavior, **not** a real multi-gigabyte transfer, sustained throughput, or cloud quotas. The GitHub Actions matrix builds and tests Windows, Linux, and macOS; adding a workflow does not mean those remote runs have already passed.

For external WebDAV-client checks, run `quarkpp_webdav_tests --serve STOP_FILE`. It prints its temporary URL and serves a mock drive for up to five minutes, stopping when the specified file appears. Use username `tester` and password `test-password`. The mock includes `a & b.txt` (`hello world`) and a virtual `big.bin`; request only a small range beyond 4 GiB from the latter. Uploads use the mock fast-upload path. Choose a stop-file path that does not already exist. This helper uses no real account.

## Live acceptance procedure

Real-account testing is intentionally separate from offline CI. Supply a session cookie through the ignored config or environment; never commit it or paste it into test output. Use your own data and a dedicated disposable directory.

1. Run `account` and `--json ls`. Confirm a multi-page directory and a UTF-8 filename.
2. Create a uniquely named test directory with `mkdir`. Upload an empty file and a small generated file, then download them and compare SHA-256 hashes.
3. Upload a generated file larger than 4 GiB. Observe memory use, actual remote size, and several multipart requests. Interrupt a download, rerun it, and compare the final SHA-256 hash. Upload restart after process termination is not a persisted resume.
4. Parse a magnet for freely redistributable content you choose. Select its actual `file_no` values, submit, poll until success/failure, then download the resulting file and verify its hash. Submission alone is not a pass.
5. Start WebDAV and check `rclone lsf`, `rclone copy`, and a ranged read. On each target OS, mount it and verify listing, seek, write, rename, remove, and unmount. Confirm there is enough staging/cache space.
6. Remove only the dedicated test objects when finished, using recoverable deletion.

`scripts/live-smoke.ps1` provides an opt-in Windows acceptance runner. Run it with `-DryRun` to inspect the plan without using credentials, with `-Large -TransferTimeoutSeconds 3600` to include a generated multi-gigabyte file, and with `-Cleanup` to remove its own remote objects after verification. Large mode defaults to one hour per transfer; increase this explicitly on slower links. It records byte counts, SHA-256 values and process memory in an ignored output directory. Unavailable memory samples are `null`. Explicit `-Config` and `QUARKPP_CONFIG` paths resolve from the caller's working directory; the default config is repository-relative. Its prefix-resume check uses a known local prefix; it must not be described as an interrupted live transfer unless a transfer was actually stopped and restarted separately.

## Real-account evidence

The user explicitly authorized these tests and supplied a cookie in a local file. Credentials were moved into the ignored `config/quarkpp.local.json`; the tracked example contains no cookie. Generated fixtures and raw local test results stay under ignored directories: `build/live-verification/`, `build/live-smoke/`, and `build/live-webdav-*`.

| Fixture | Bytes | SHA-256 after download |
| --- | ---: | --- |
| Browser smoke text | 67 | `bf96b35f720c46145b9a7549fb02aadf1cbed9bfb4e106d3644a4e7081f0aede` |
| Random normal upload | 1,048,609 | `fc79ca1b41e36d9e0d2d66ef2fb503b72787faf30c0b847258bc3fa02ba35611` |
| Sequential multipart upload | 33,554,432 | `f35533996495669decaf81128939323ac323d8627d72f746803a9d4baa511af5` |
| 250 MB normal upload and actual interrupted-download resume | 250,000,000 | `5fa8cd6e4d0f800bbccc681968d8c35be645c988577282cd1140b76baf8ba8a4` |
| Sintel English subtitles, cloud BT | 1,514 | `4ed7e1f1bc5ff69fe33606960e4c048ba6766237bf33b132fef76e1a92cb64b2` |

### 250 MB interrupted round trip

The final fixture was 250,000,000 bytes (decimal MB). It uploaded normally
(`fast_upload:false`) in 194.613 seconds, with a sampled peak process working set
of 17,776,640 bytes (16.95 MiB). Remote size and parent directory were checked.

The test stopped its own download process after 8,855,552 bytes had actually
arrived. A new CLI process resumed that file and completed in 16.217 seconds,
including 3.905 seconds before the first additional bytes were observed. The
remaining 241,144,448 bytes averaged about 18.679 MiB/s (19.6 MB/s) after that
first-byte sample, or 14.181 MiB/s including request setup. These are observed
single-run averages, not guaranteed service speeds. The resumed process's sampled
peak working set was 17,100,800 bytes (16.31 MiB). The final size and complete
SHA-256 matched the source.

The earlier 4.02 GiB download ran at approximately 128 KiB/s. An account query
confirmed active SUPER_VIP membership. The client had omitted server-issued
`__puus` updates and used inconsistent User-Agent values on its large-file
fallback. Both authentication defects are now corrected and covered by offline
regressions. The successful faster transfer above used the user's replacement
250 MB fixture; it does not establish the speed of every file or a completed
4.02 GiB round trip.

Local evidence is in `build/live-verification/250mb-metrics.json` and its sibling
source/upload/progress records. Raw credential-bearing files remain ignored.

After verification, the seven known children and BT subtitle in the dedicated
`QuarkPlusPlus-live-20261020` directory were checked before the directory was
recycled. The asynchronous recycle task completed and absence from the root
listing was confirmed. No recycle bin was emptied. Four exact generated local
large-file paths were removed, freeing 4,940,891,393 bytes; metrics, logs and the
website screenshot were retained.

### Cloud BT verification

The BT infohash `08ada5a7a6183aae1e09d831df6748d566095a10` was computed from [WebTorrent's official torrent](https://webtorrent.io/torrents/sintel.torrent). The downloaded subtitle was compared using SHA-256 with its declared [official WebSeed](https://webtorrent.io/torrents/Sintel/Sintel.en.srt). The task created a `Sintel` subdirectory inside the dedicated test directory. This verifies cloud staging and retrieval of selected BT content; it does not establish availability of every magnet.

The website displayed an account-restriction toast during loading, but listing, upload, download and the selected BT task succeeded. Results here describe the actual operations, not an inference from that toast.
