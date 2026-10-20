<div align="center">

# QuarkPlusPlus

**Your Quark Drive. Your terminal. Your filesystem.**

A reusable C++20 library, a focused command-line client, and a local WebDAV bridge.

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![Build](https://img.shields.io/badge/build-xmake-22A079)
![HTTP](https://img.shields.io/badge/HTTP-cpp--httplib-455A64)
![License](https://img.shields.io/badge/license-GPL--3.0-blue)

English · [简体中文](README.zh-CN.md)

[Quick start](#quick-start) · [Commands](#commands) · [Mount](#mount-on-windows-linux-and-macos) · [C++ library](#use-the-library) · [Testing](docs/testing.md)

</div>

---

## What it does

| Capability | Implementation |
| :--- | :--- |
| Sign in by QR code | Scan in the Quark mobile app; the CLI saves your session locally |
| Browse and manage | Paginated listing, search, file information, folders, rename, recycle-bin deletion |
| Upload large files | Streaming MD5/SHA1, fast-upload check, OSS multipart upload, bounded memory, part retries |
| Download large files | Streaming to disk, 64-bit offsets, validated HTTP Range resume, refreshed links on retry |
| Cloud BT downloads | Parse magnet/HTTP(S) sources, select files, submit to Quark, inspect and wait for tasks |
| Mount your drive | Authenticated local WebDAV for rclone mount on Windows, Linux, and macOS |
| Build on the API | Installable `quarkpp_core` static library with public headers and progress callbacks |

This is an **unofficial client** using Quark's private web APIs. Account privileges, quotas, speed limits, resource availability, and protocol changes still apply. See [protocol notes](docs/protocol.md). Windows/xmake and Linux/GCC builds and local integration tests are verified, along with a real 4.02 GiB upload, a SHA-256-checked 250 MB upload/download with interrupted-download resume, and a completed cloud BT download. See the [verification record](docs/testing.md) for exact sizes and limits. macOS builds and native OS mounts remain unverified; CI is provided for all three platforms.

## Quick start

Use a **64-bit C++20 toolchain**: Visual Studio 2022 or newer on Windows, GCC 13+/Clang 16+ with a C++20 standard library on Linux, or a recent Xcode Command Line Tools on macOS. Install [xmake](https://xmake.io/guide/quick-start), then:

```sh
xmake f -m release -y
xmake
xmake run quarkpp --help
```

xmake resolves pinned cpp-httplib, OpenSSL 3, and nlohmann/json packages. The build generates a local cpp-httplib header with three additional WebDAV method names; it does not modify your global package cache. See [architecture](docs/architecture.md).

Sign in from your terminal:

```sh
xmake run quarkpp login
```

Scan the displayed QR code with the Quark mobile app and confirm sign-in. The CLI saves the session to `config/quarkpp.local.json`, preserving your other settings. This file is ignored by Git. Run `login` again when your session expires. You can also supply an existing Quark web session cookie through `QUARKPP_COOKIE` or [the example config](config/quarkpp.example.json).

```json
{
  "cookie": "YOUR_QUARK_COOKIE",
  "download_dir": "downloads",
  "request_timeout_ms": 120000,
  "upload_retry_count": 3,
  "download_retry_count": 3,
  "task_poll_limit": 30
}
```

```sh
xmake run quarkpp account
xmake run quarkpp ls
xmake run quarkpp ls --path /Documents
```

Use `--config FILE` or `QUARKPP_CONFIG` to choose another config. `QUARKPP_COOKIE` takes precedence over its cookie. Optional `user_agent` and `ca_bundle` configure the normal API browser identity and trusted PEM CA bundle. Download-link requests and file reads use the same desktop-client identity for large-file compatibility. Keep TLS verification enabled. Relative paths, including the default config path, resolve from the working directory.

Install the CLI, library, headers, and docs with `xmake install -o ./dist`. Examples below assume `dist/bin` is on `PATH`; otherwise use `xmake run quarkpp` in place of `quarkpp`.

## Commands

```sh
# Explore and manage
quarkpp login
quarkpp --json ls --path /Documents
quarkpp search "report"
quarkpp info --path /Documents/report.pdf
quarkpp mkdir Archive --parent-path /Documents
quarkpp rename report-final.pdf --path /Documents/report.pdf
quarkpp rm --path /Documents/old-report.pdf

# Transfer: parent folders must already exist
quarkpp upload ./movie.mkv --to-path /Videos --progress
quarkpp upload ./notes.txt --to-fid 0 --name notes-backup.txt
quarkpp download --path /Videos/movie.mkv --out ./downloads --progress
quarkpp download --path /Documents --out ./backup
```

Paths and IDs are interchangeable via `--path` / `--fid`, or `--to-path` / `--to-fid` for upload destinations. Downloading a folder preserves its name, contents, and empty subfolders beneath `--out`. `--json` produces machine-readable listings; other command results already use JSON. Errors go to stderr and return a nonzero exit code.

Add `--progress` to upload or download commands for current-file byte counts on stderr, at most once per second, and a final completion line. JSON results remain on stdout.

### Large transfers

Transfers never load the entire file into RAM. Upload reads bounded buffers, hashes the source, uploads server-sized parts, and retries failed parts. A failed process must restart the upload; upload checkpoints are **not** persisted. Keep the source file unchanged during transfer.

Downloads resume by default. A resumed response must match the requested offset and remote size before bytes are appended. An ignored Range, conflicting total, or oversized local file fails instead of silently corrupting the result. Use `--no-resume` to restart. Existing partial files must belong to the same remote content: size and range checks do not prove byte identity or provide a full-file checksum. Interrupted downloads remain at the destination for the next run. Timeouts apply to individual socket operations, not the total duration of a large transfer.

### BT and cloud downloads

```sh
# Preview first; --select values are provider file_no values in this output
quarkpp offline parse "magnet:?xt=urn:btih:YOUR_INFO_HASH"
quarkpp offline add "magnet:?xt=urn:btih:YOUR_INFO_HASH" --to-path /Downloads --select 0,2
quarkpp offline list
quarkpp offline status TASK_ID --wait

# Once Quark reports completion, download the saved file normally
quarkpp download --path /Downloads/example.iso --out ./downloads
```

Omit `--select` to select every parsed file. Submission reports `submitted`; it does not claim that the content has finished downloading. Quark performs the download in the cloud. This client does not run a local BitTorrent engine, and local `.torrent` ingestion is not implemented. HTTP(S) sources are accepted when Quark can parse them. A timed-out wait does not cancel the cloud task.

## Mount on Windows, Linux, and macOS

The client serves a local WebDAV bridge. [rclone mount](https://rclone.org/commands/rclone_mount/) provides the filesystem integration; install rclone and the platform's mount dependency:

| Platform | Mount dependency | Example mountpoint |
| :--- | :--- | :--- |
| Windows | [WinFsp](https://winfsp.dev/) | `Q:` |
| Linux | FUSE 3 / `fusermount3` | `$HOME/Quark` |
| macOS | [macFUSE](https://osxfuse.github.io/), plus a compatible rclone distribution | `$HOME/Quark` |

Set `QUARKPP_WEBDAV_PASSWORD` locally, then keep this process running:

```sh
quarkpp serve --port 8787
# Add --read-only to expose a read-only drive
```

Run `rclone config`, create a remote named `quark`, choose `webdav`, and enter:

| Setting | Value |
| :--- | :--- |
| URL | `http://127.0.0.1:8787/` |
| Vendor | `other` |
| Username | `quarkpp` |
| Password | The same value as `QUARKPP_WEBDAV_PASSWORD` |

Check it with `rclone lsf quark:`. In another terminal:

```powershell
# Windows
rclone mount quark: Q: --vfs-cache-mode full --timeout 1h
```

```sh
# Linux / macOS
mkdir -p "$HOME/Quark"
rclone mount quark: "$HOME/Quark" --vfs-cache-mode full --timeout 1h
```

Keep both processes running; stop the mount before stopping the bridge. See [mounting details](docs/mounting.md) for unmounting, behavior, and troubleshooting.

WebDAV reads stream directly from Quark. Writes need **temporary disk space for the whole file**, in addition to rclone's VFS cache; `--temp-dir` chooses an existing staging directory. Cloud replacement is not atomic. The bridge supports listing, GET/HEAD with one byte range, PUT, MKCOL, DELETE, and same-directory MOVE. It does not implement COPY, LOCK, PROPPATCH, or POSIX filesystem semantics. rclone is the documented mount client; native Windows WebClient and Finder mounts are not validated.

## Use the library

```cpp
#include <quarkpp/client.hpp>
#include <cstdlib>
#include <iostream>

int main() {
    const char* cookie = std::getenv("QUARKPP_COOKIE");
    if (!cookie) return 1;
    quarkpp::AppConfig config;
    config.cookie = cookie;
    quarkpp::QuarkClient drive(config);

    for (const auto& entry : drive.list_directory("0"))
        std::cout << entry.name << '\n';

    auto uploaded = drive.upload_file("archive.zip", "0", {},
        [](std::uint64_t done, std::uint64_t total) {
            // Update your UI or telemetry here.
        });
    drive.download_to_file(uploaded.fid, "archive-copy.zip");
}
```

Inside this xmake project, use `add_deps("quarkpp_core")`. Installed consumers link `quarkpp` plus OpenSSL and platform socket/thread libraries, and include nlohmann/json. Methods are synchronous, report progress through callbacks, and do not print. Provider and transport failures throw `quarkpp::QuarkException`; filesystem and JSON exceptions may also propagate. Public types and methods are in [client.hpp](include/quarkpp/client.hpp) and [support.hpp](include/quarkpp/support.hpp).

## Development

```sh
xmake f -m debug --tests=y -y
xmake
xmake test -v
```

The tests use loopback mock servers and generated temporary files; they need no account and do not modify a drive. Sparse-file tests exercise offsets beyond 4 GiB without uploading gigabytes. See [testing and live verification](docs/testing.md), [architecture](docs/architecture.md), and [protocol references](docs/protocol.md).

The 0.2 rewrite replaces the old monolithic C++23/libcurl implementation. Growth/check-in and sharing commands were removed; this version focuses on file access, transfers, offline tasks, and mounting. There is no source compatibility promise with the old private headers.

## License

[GNU General Public License v3.0](LICENSE). QuarkPlusPlus is not affiliated with Quark or its operators.
