[简体中文](README.md)

# QuarkPlusPlus

`QuarkPlusPlus` is a Quark Drive CLI client for terminal workflows and automation. It is built with `C++23`, uses `xmake` for a straightforward build/run/install flow, and relies on `libcurl + OpenSSL + nlohmann_json` for cross-platform networking, crypto, and data handling.

## Features

- Account info
- Daily growth/sign-in info and sign-in
- Directory listing
- File search
- File/folder metadata
- Create folder
- Rename
- Delete
- Share creation
- Share listing
- Share transfer/save
- Local file upload
- Large file multipart upload
- Fast upload hash check
- Remote file/folder download
- Large file resume download

## Configuration

By default the project reads local config from `config/quarkpp.local.json`.
Create it from [config/quarkpp.example.json](config/quarkpp.example.json) and fill in your cookie:

```json
{
  "cookie": "your quark cookie",
  "download_dir": "downloads",
  "user_agent": "Mozilla/5.0 ...",
  "request_timeout_ms": 120000,
  "upload_retry_count": 3
}
```

Environment overrides are also supported:

- `QUARKPP_CONFIG`
- `QUARKPP_COOKIE`
- `NO_COLOR`
- `QUARKPP_NO_COLOR`
- `QUARKPP_NO_ICONS`

## Build

```sh
xmake f -m release
xmake
```

## Run

The recommended cross-platform way to run the client is `xmake run`:

```sh
xmake run quarkpp --help
xmake run quarkpp account
xmake run quarkpp ls
xmake run quarkpp ls --path /Videos
xmake run quarkpp upload ./movie.mkv --to-path /Videos --name movie-final.mkv
xmake run quarkpp download --path /Videos --out ./downloads
xmake run quarkpp share-create --path /Videos/Collection --expire 7d --passcode 1234
xmake run quarkpp transfer "https://pan.quark.cn/s/xxxx" --passcode 1234 --to-path /Saved
```

## Install

`xmake install` is supported:

```sh
xmake install -o ./dist
```

The install output includes:

- The executable
- [config/quarkpp.example.json](config/quarkpp.example.json)
- Chinese and English README files
- `LICENSE`

These files are installed under `./dist/bin` and `./dist/share/quarkpp`.

## Large File Handling

Upload flow includes:

- Pre-upload
- MD5/SHA1 fast-upload check
- OSS multipart PUT upload
- Multipart completion
- Quark-side finish confirmation
- Per-part retry

Download flow includes:

- Server-side download URL lookup
- Streaming writes to disk
- Resume support for existing files
- Recursive directory download

## License

This project is open source under the GPLv3 license.
