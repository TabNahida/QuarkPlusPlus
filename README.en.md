[简体中文](README.md)

# QuarkPlusPlus

`QuarkPlusPlus` is a Quark Drive command-line client built for terminal workflows, scripting, and automation, using `C++23` and `xmake`.

## Configuration

The default local config path is `config/quarkpp.local.json`. This file is ignored by git. Create it from [config/quarkpp.example.json](config/quarkpp.example.json) and fill in your cookie:

```json
{
  "cookie": "your quark cookie",
  "download_dir": "downloads",
  "user_agent": "Mozilla/5.0 ...",
  "request_timeout_ms": 120000,
  "upload_retry_count": 3
}
```

Environment overrides:

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

The recommended way to run the client is `xmake run`:

```sh
xmake run quarkpp --help
xmake run quarkpp account
xmake run quarkpp growth-info
xmake run quarkpp ls
xmake run quarkpp ls --path /Videos
xmake run quarkpp info --path /Videos/Collection
xmake run quarkpp upload ./movie.mkv --to-path /Videos --name movie-final.mkv
xmake run quarkpp download --path /Videos --out ./downloads
xmake run quarkpp share-create --path /Videos/Collection --expire 7d --passcode 1234
xmake run quarkpp transfer "https://pan.quark.cn/s/xxxx" --passcode 1234 --to-path /Saved
```

## Install

```sh
xmake install -o ./dist
```

The executable is installed to `./dist/bin`, and the example config and documentation are installed to `./dist/share/quarkpp`.

## License

This project is licensed under GPLv3.
