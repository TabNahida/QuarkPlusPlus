[English](README.en.md)

# QuarkPlusPlus

`QuarkPlusPlus` 是一个基于 `xmake` 和 `C++23` 的夸克网盘第三方 CLI 客户端。

当前实现使用 `libcurl + OpenSSL + nlohmann_json`，不依赖 `WinHTTP / BCrypt / Crypt32` 这类 Windows 专属库。项目默认文档为中文，英文文档见上方链接。

## 已实现能力

- 账号信息读取
- 容量签到信息查询与签到
- 网盘目录列出
- 文件搜索
- 文件/文件夹详情
- 新建文件夹
- 重命名
- 删除
- 分享创建
- 分享列表查询
- 分享转存
- 本地文件上传
- 大文件分片上传
- 秒传检查
- 远端文件/目录下载
- 大文件断点续传下载

## 配置

项目默认从 `config/quarkpp.local.json` 读取本地配置。这个文件已被 `.gitignore` 忽略，不会进入仓库。

建议直接根据 [config/quarkpp.example.json](config/quarkpp.example.json) 新建本地配置文件，并填入你的 cookie：

```json
{
  "cookie": "your quark cookie",
  "download_dir": "downloads",
  "user_agent": "Mozilla/5.0 ...",
  "request_timeout_ms": 120000,
  "upload_retry_count": 3
}
```

也支持环境变量覆盖：

- `QUARKPP_CONFIG`
- `QUARKPP_COOKIE`

## 构建

```sh
xmake f -m release
xmake
```

## 运行

推荐直接使用 `xmake run`，命令写法跨平台一致：

```sh
xmake run quarkpp -- --help
xmake run quarkpp -- account
xmake run quarkpp -- ls
xmake run quarkpp -- ls --path /视频
xmake run quarkpp -- upload ./movie.mkv --to-path /视频 --name movie-final.mkv
xmake run quarkpp -- download --path /视频 --out ./downloads
xmake run quarkpp -- share-create --path /视频/电影合集 --expire 7d --passcode 1234
xmake run quarkpp -- transfer "https://pan.quark.cn/s/xxxx" --passcode 1234 --to-path /收藏
```

## 安装

已经支持 `xmake install`。示例：

```sh
xmake install -o ./dist
```

安装后默认会包含：

- 可执行文件
- [config/quarkpp.example.json](config/quarkpp.example.json)
- 中文/英文 README
- `LICENSE`

这些文件会被安装到 `./dist/bin` 和 `./dist/share/quarkpp` 下。

## 大文件说明

上传流程实现了：

- 预上传
- MD5/SHA1 秒传检查
- OSS 分片 PUT 上传
- 分片合并提交
- 夸克侧完成确认
- 单分片失败重试

下载流程实现了：

- 服务端下载链接获取
- 本地流式写盘
- 已存在文件的断点续传
- 目录递归下载

## 编码说明

已经补了两层 UTF-8 处理：

- 构建时显式使用 UTF-8 源码/执行字符集编译
- Windows 下命令行参数改为宽字符读取后转 UTF-8，减少中文路径和中文输出乱码问题

如果你后面要在 Linux/macOS 上继续验证，重点就是再做一轮不同终端与不同 locale 下的实机测试。
