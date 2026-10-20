QuarkPlusPlus 0.2.0 rebuilds the project as a reusable C++20 library and a command-line client for Quark Drive.

## Highlights

- Terminal QR-code login: run `quarkpp login`, scan with the Quark app, and keep the saved session in your local configuration.
- Browse, search, upload, download, rename, and recycle files through the CLI or public C++ API; the library also supports moving files.
- Stream multipart uploads and resume downloads with HTTP Range requests and 64-bit offsets.
- Parse magnet links, select torrent files, and submit cloud download tasks.
- Serve an authenticated WebDAV endpoint for rclone and filesystem mounting on Windows, Linux, and macOS.
- Build with xmake, cpp-httplib, OpenSSL, and C++20. English and Chinese documentation is included.

## Downloads

| Archive suffix | Platform |
| --- | --- |
| `windows-x64.zip` | Windows x64 |
| `linux-x64.tar.gz` | Linux x64, glibc 2.39+ and GCC 13 libstdc++ (Ubuntu 24.04+) |
| `macos-x64.tar.gz` | macOS 15 or newer, Intel |
| `macos-arm64.tar.gz` | macOS 15 or newer, Apple Silicon |

Each archive contains `bin/quarkpp`, the static core library, public headers, configuration example, and documentation. Windows uses `quarkpp.exe`. Verify downloads against `SHA256SUMS`. OpenSSL is linked into the CLI; building applications against the static core library still requires its documented dependencies and a compatible compiler ABI.

The GitHub Actions matrix builds, runs offline integration tests, checks the installed executable, and packages every platform before this release is published. These are unsigned command-line distributions.

## Verified behavior and limits

Terminal QR login was verified with a real account, including automatic private session storage and successful account/list queries from new CLI processes.

Real-account tests verified a 250,000,000-byte upload/download with an interrupted transfer, resumed download, and matching SHA-256. A separate 4,311,744,513-byte upload and a WebDAV read beyond the 4 GiB boundary passed. The entire 4 GiB file was not downloaded for a full hash comparison. Cloud BT selected-file download and WebDAV operations through rclone were also verified.

Native filesystem mounting depends on rclone and its platform filesystem driver; native mounts have not been exercised. Uploads stream with bounded memory, but restarting the client starts a new upload rather than resuming an upload checkpoint. Quark's unofficial web protocol can change independently of this release.

## Upgrading

The CLI, public API, and configuration have been reorganized. Start with the bundled README and `quarkpp --help`. Existing users can retain a private Cookie configuration or use the new QR login command. Never commit a saved login configuration.
