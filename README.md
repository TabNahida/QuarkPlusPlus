[English](README.en.md)

# QuarkPlusPlus

`QuarkPlusPlus` 是一个夸克网盘命令行客户端，面向终端使用、脚本编排和自动化任务，使用 `C++23` 与 `xmake` 构建。

## 配置

项目默认读取 `config/quarkpp.local.json`。这个文件已加入忽略规则，建议参照 [config/quarkpp.example.json](config/quarkpp.example.json) 新建本地配置，然后填入你的 cookie，例如：

```json
{
  "cookie": "your quark cookie",
  "download_dir": "downloads",
  "user_agent": "Mozilla/5.0 ...",
  "request_timeout_ms": 120000,
  "upload_retry_count": 3
}
```

也可以通过环境变量覆盖：

- `QUARKPP_CONFIG`
- `QUARKPP_COOKIE`
- `NO_COLOR`
- `QUARKPP_NO_COLOR`
- `QUARKPP_NO_ICONS`

## 构建

```sh
xmake f -m release
xmake
```

## 运行

推荐直接用 `xmake run`：

```sh
xmake run quarkpp --help
xmake run quarkpp account
xmake run quarkpp growth-info
xmake run quarkpp ls
xmake run quarkpp ls --path /视频
xmake run quarkpp info --path /视频/电影合集
xmake run quarkpp upload ./movie.mkv --to-path /视频 --name movie-final.mkv
xmake run quarkpp download --path /视频 --out ./downloads
xmake run quarkpp share-create --path /视频/电影合集 --expire 7d --passcode 1234
xmake run quarkpp transfer "https://pan.quark.cn/s/xxxx" --passcode 1234 --to-path /收藏
```

## 安装

```sh
xmake install -o ./dist
```

安装后可执行文件位于 `./dist/bin`，示例配置和文档位于 `./dist/share/quarkpp`。

## 许可证

本项目使用 GPLv3 许可证。
