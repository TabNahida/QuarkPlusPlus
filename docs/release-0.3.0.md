QuarkPlusPlus 0.3.0 adds parallel file downloads and small-file batches, and uses PACE 0.4.0's direct HTTP API for ordinary provider requests.

## Highlights

- Replace buffered HTTP request plumbing with `pace::http::fetch`. Streaming file transfers and WebDAV continue to use cpp-httplib with bounded memory.
- Preserve native Windows/macOS certificate trust and explicit CA validation, and correct empty HTTP 204 response framing for PACE interoperability.
- Download large files with multiple validated byte ranges. Completed contiguous waves are appended to disk, so an interrupted download remains resumable without segment sidecars.
- Download small files concurrently, request their links in batches, and report aggregate progress. Repeat `--fid` or `--path` to download several selections together; recursive directory downloads use the same scheduler.
- Add the public `QuarkClient::download_batch()` method while retaining the existing single-selection methods.
- Record PACE and c-ares versions and licenses in release archives, and reject packaging when the installed CLI version differs from the project version.

## Usage

```sh
quarkpp download --path /Videos/movie.mkv --out ./downloads --threads 8 --progress
quarkpp download --path /Documents/a.txt --path /Documents/b.txt --out ./backup --jobs 4
quarkpp download --path /Documents --out ./backup --batch-size 20 --progress
```

New configuration defaults are `download_threads: 4`, `download_jobs: 4`, `download_batch_size: 20`, and `small_file_threshold: 8388608` (8 MiB). The matching command-line overrides are `--threads`, `--jobs`, `--batch-size`, and `--small-file-threshold`. Set `--threads 1` for single-stream large-file downloads. Existing private login configuration remains usable.

Files at or below the threshold download concurrently as single streams. Larger files download one at a time with parallel ranges; the origin must support valid ranges to use that path. Existing partial files must still belong to the same remote content: size and range checks do not prove byte identity.

## Downloads and SDK

| Archive suffix | Platform |
| --- | --- |
| `windows-x64.zip` | Windows x64 |
| `linux-x64.tar.gz` | Linux x64, glibc 2.39+ and GCC 13 libstdc++ (Ubuntu 24.04+) |
| `macos-x64.tar.gz` | macOS 15 or newer, Intel |
| `macos-arm64.tar.gz` | macOS 15 or newer, Apple Silicon |

Each archive contains the CLI, QuarkPlusPlus static core library, public headers, example configuration, documentation and dependency notices. Verify archives against `SHA256SUMS`. PACE, OpenSSL and c-ares are linked statically into the CLI.

SDK applications supply PACE 0.4.0 with HTTP/1, TLS and DNS enabled and WebSocket disabled, OpenSSL 3.6.1, c-ares 1.34.8 and nlohmann/json 3.12.0. Follow the static-link and ABI requirements in [architecture.md](architecture.md).

## Verification and limits

Publication is gated on builds, offline tests, installed-CLI checks and packaging for all four platforms. Offline tests use loopback fixtures; they do not establish real-account throughput or native filesystem mount compatibility. Earlier real-account transfer results are historical and separately dated in [testing.md](testing.md).

The Quark protocol is unofficial and account speed limits still apply. Upload checkpoints are not persisted across processes. Native filesystem mounting continues to depend on rclone and its platform driver.
