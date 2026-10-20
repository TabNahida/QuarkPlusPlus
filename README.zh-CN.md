<div align="center">

# QuarkPlusPlus

**让夸克网盘走进终端与文件系统。**

可复用的 C++20 接口库 · 命令行客户端 · 本地 WebDAV 挂载桥接

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![Build](https://img.shields.io/badge/build-xmake-22A079)
[![CI](https://github.com/TabNahida/QuarkPlusPlus/actions/workflows/build.yml/badge.svg)](https://github.com/TabNahida/QuarkPlusPlus/actions/workflows/build.yml)
![HTTP](https://img.shields.io/badge/HTTP-cpp--httplib-455A64)
![License](https://img.shields.io/badge/license-GPL--3.0-blue)

[English](README.md) · 简体中文

[快速开始](#快速开始) · [常用命令](#常用命令) · [挂载网盘](#在-windowslinuxmacos-挂载) · [接入接口库](#接入-c-接口库)

</div>

---

## 功能

| 能力 | 实现方式 |
| :--- | :--- |
| 扫码登录 | 终端显示二维码，夸克手机 App 扫码确认后自动保存会话 |
| 浏览与管理 | 自动分页、搜索、文件信息、新建目录、重命名、移入回收站 |
| 大文件上传 | 流式 MD5/SHA1、秒传检查、OSS 分片上传、固定缓冲区、分片重试 |
| 大文件下载 | 流式写盘、64 位偏移、校验 Range 的断点续传、重试时刷新下载链接 |
| BT 离线下载 | 解析磁力/HTTP(S) 链接、选择文件、提交夸克云端任务、查询和等待结果 |
| 跨平台挂载 | 带认证的本地 WebDAV，配合 rclone mount 挂载到三大桌面平台 |
| 二次开发 | 独立的 `quarkpp_core` 静态库、公开头文件与进度回调 |

本项目是使用夸克私有网页接口的**非官方客户端**。账号权限、配额、速度限制、资源可用性及上游协议变更仍然生效，详见[协议说明](docs/protocol.md)。已验证 Windows/xmake、Linux/GCC 编译、本地集成测试、真实账号的 4.02 GiB 上传、通过 SHA-256 校验的 250 MB 上传下载与实际中断续传，以及已完成的 BT 云下载。具体大小与边界见[验证记录](docs/testing.md)。GitHub Actions 会构建并测试 Windows x64、Linux x64、macOS Intel 和 Apple Silicon，四项全部成功后才会发布版本。各系统实际挂载尚未验证。

## 快速开始

可从 [Releases](https://github.com/TabNahida/QuarkPlusPlus/releases) 下载 Windows x64、Linux x64、macOS Intel 或 Apple Silicon 的预编译客户端和 SDK。解压后将 `bin` 加入 `PATH`，运行 `quarkpp login`；每个版本附有校验和及系统要求。从源码构建请继续：

准备 **64 位 C++20 编译器**：Windows 使用 Visual Studio 2022 或更新版本；Linux 使用 GCC 13+/Clang 16+，并配备支持 C++20 的标准库；macOS 使用较新的 Xcode Command Line Tools。安装 [xmake](https://xmake.io/guide/quick-start)，然后运行：

```sh
xmake f -m release -y
xmake
xmake run quarkpp --help
```

xmake 管理固定版本的 cpp-httplib、OpenSSL 3 和 nlohmann/json。构建会在本项目的生成目录中扩展 cpp-httplib 的 WebDAV 方法列表，不修改全局依赖缓存。

直接在终端登录：

```sh
xmake run quarkpp login
```

使用夸克手机 App 扫描二维码并确认登录。CLI 会把会话保存到 `config/quarkpp.local.json`，保留其他配置项；该文件已加入 Git 忽略列表。会话过期后重新执行 `login` 即可。也兼容通过 `QUARKPP_COOKIE` 或[配置示例](config/quarkpp.example.json)手动提供已有 Cookie。

若终端显示不清晰，可添加 `--qr-svg login.svg` 并打开本地图片扫码。二维码有效期为五分钟；`--timeout SECONDS` 可缩短等待时间，`--ca-bundle FILE` 可为登录配置自定义 PEM 证书包。

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

使用 `--config FILE` 或 `QUARKPP_CONFIG` 指定配置文件；`QUARKPP_COOKIE` 优先于配置中的 Cookie。可选的 `user_agent` 和 `ca_bundle` 分别设置普通 API 请求的浏览器标识与可信 PEM CA 证书包。获取下载链接和读取文件使用一致的桌面客户端标识，以兼容大文件下载。TLS 始终验证证书。相对路径以当前工作目录为基准。

`xmake install -o ./dist` 安装客户端、静态库、头文件和文档。以下示例假设 `dist/bin` 已加入 `PATH`，也可将 `quarkpp` 替换成 `xmake run quarkpp`。

## 常用命令

```sh
quarkpp login
quarkpp --json ls --path /Documents
quarkpp search "report"
quarkpp info --path /Documents/report.pdf
quarkpp mkdir Archive --parent-path /Documents
quarkpp rename report-final.pdf --path /Documents/report.pdf
quarkpp rm --path /Documents/old-report.pdf

# 上传的目标目录需要预先存在
quarkpp upload ./movie.mkv --to-path /Videos --progress
quarkpp upload ./notes.txt --to-fid 0 --name notes-backup.txt
quarkpp download --path /Videos/movie.mkv --out ./downloads --progress
quarkpp download --path /Documents --out ./backup
```

`--fid`/`--path` 指定文件或目录，`--to-fid`/`--to-path` 指定上传位置。下载目录时，会在 `--out` 下保留目录名称、内容和空子目录。`--json` 将列表转换成便于脚本解析的 JSON，其他命令的结果默认使用 JSON。错误写入 stderr，并返回非零退出码。

上传、下载命令加 `--progress` 后，会向 stderr 最多每秒输出一次当前文件的传输字节数，并在成功后输出完成行；stdout 仍保留 JSON 结果。

### 大文件传输

上传和下载都不会将整个文件放进内存。上传先流式计算哈希，再按服务端要求分片上传，失败的分片会重试。**暂不保存跨进程上传检查点**，退出后重新执行上传会重新开始任务。上传期间请勿修改源文件。

下载默认断点续传。只有服务端返回的起始偏移和总大小正确，才会追加数据；忽略 Range、远端大小冲突或本地文件过大都会明确报错。使用 `--no-resume` 从头下载。已有的部分文件必须属于同一远端内容：长度和 Range 检查不能代替完整文件校验。下载中断后保留目标文件，方便下次继续。超时针对单次连接/读写操作，不限制整个大文件的传输时间。

### BT 与云端离线任务

```sh
# 先查看解析结果；--select 使用结果中的 file_no 编号
quarkpp offline parse "magnet:?xt=urn:btih:YOUR_INFO_HASH"
quarkpp offline add "magnet:?xt=urn:btih:YOUR_INFO_HASH" --to-path /Downloads --select 0,2
quarkpp offline list
quarkpp offline status TASK_ID --wait

# 等夸克完成后，再下载已经保存的文件
quarkpp download --path /Downloads/example.iso --out ./downloads
```

省略 `--select` 则选择全部解析文件。提交成功显示 `submitted`，并不代表资源已经下载完成。任务由夸克云端执行，本项目不内置本地 BT 引擎，也未实现读取本地 `.torrent` 文件。HTTP(S) 链接是否可用取决于夸克能否解析。等待超时不会取消云端任务。

## 在 Windows、Linux、macOS 挂载

客户端提供本地 WebDAV，文件系统挂载由 [rclone mount](https://rclone.org/commands/rclone_mount/) 完成。

| 系统 | 挂载依赖 | 挂载位置示例 |
| :--- | :--- | :--- |
| Windows | rclone + [WinFsp](https://winfsp.dev/) | `Q:` |
| Linux | rclone + FUSE 3 / `fusermount3` | `$HOME/Quark` |
| macOS | 支持挂载的 rclone 发行版 + [macFUSE](https://osxfuse.github.io/) | `$HOME/Quark` |

在本机设置 `QUARKPP_WEBDAV_PASSWORD`，然后运行并保持进程开启：

```sh
quarkpp serve --port 8787
# 添加 --read-only 可提供只读挂载
```

使用 `rclone config` 创建名为 `quark` 的远端，类型选择 `webdav`：

| 配置项 | 值 |
| :--- | :--- |
| URL | `http://127.0.0.1:8787/` |
| Vendor | `other` |
| Username | `quarkpp` |
| Password | 与 `QUARKPP_WEBDAV_PASSWORD` 相同 |

先用 `rclone lsf quark:` 检查目录，再在另一个终端执行：

```powershell
# Windows
rclone mount quark: Q: --vfs-cache-mode full --timeout 1h
```

```sh
# Linux / macOS
mkdir -p "$HOME/Quark"
rclone mount quark: "$HOME/Quark" --vfs-cache-mode full --timeout 1h
```

挂载进程和 WebDAV 进程都需要保持运行，退出时先卸载再停止桥接。详细说明见[挂载文档](docs/mounting.md)。

读取会直接从夸克流式传输。写入需先保存完整文件到临时磁盘，**临时磁盘空间需要容纳整个文件**，此外还有 rclone VFS 缓存；可用 `--temp-dir` 指定已有目录。覆盖远端文件不是原子操作。桥接支持目录列表、GET/HEAD 单段 Range、PUT、MKCOL、DELETE、同目录 MOVE；不支持 COPY、LOCK、PROPPATCH，也不具备完整 POSIX 语义。文档以 rclone 为挂载客户端，Windows 原生 WebClient 和 Finder 挂载未验证。

## 接入 C++ 接口库

[auth.hpp](include/quarkpp/auth.hpp) 提供独立于 CLI 的认证 API：`QrLoginClient::start()` 返回官方二维码 URL，`poll()` 返回登录状态，并在确认后返回会话 Cookie。调用方自行选择显示和保存方式。

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

    auto result = drive.upload_file("archive.zip", "0");
    drive.download_to_file(result.fid, "archive-copy.zip");
}
```

项目内的 xmake target 使用 `add_deps("quarkpp_core")`。独立使用已安装的库时，需要链接 `quarkpp`、OpenSSL、平台 socket/thread 库，并提供 nlohmann/json 头文件。接口同步执行，通过回调报告进度，不向终端打印。服务端和传输错误使用 `quarkpp::QuarkException`，文件系统和 JSON 异常也可能向调用方传播。公开接口见 [client.hpp](include/quarkpp/client.hpp) 和 [support.hpp](include/quarkpp/support.hpp)。

## 开发与测试

```sh
xmake f -m debug --tests=y -y
xmake
xmake test -v
```

测试使用本地模拟服务器和临时文件，不需要网盘账号，也不会修改网盘。稀疏文件测试验证超过 4 GiB 的偏移，无需真的上传数 GiB 数据。详见[测试与实测步骤](docs/testing.md)、[架构](docs/architecture.md)和[协议来源](docs/protocol.md)。

0.2 版本替换了原来的 C++23/libcurl 单体实现，移除了成长签到、分享等旧命令，聚焦文件管理、传输、离线任务和挂载。旧的私有头文件不提供源码兼容保证。

## 许可证

[GNU General Public License v3.0](LICENSE)。QuarkPlusPlus 与夸克及其运营方无隶属关系。
