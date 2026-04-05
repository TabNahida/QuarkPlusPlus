# QuarkPlusPlus

`QuarkPlusPlus` 是一个基于 `xmake` 和 `C++23` 的夸克网盘第三方 CLI 客户端。

当前实现使用 `libcurl + OpenSSL + nlohmann_json`，不再依赖 `WinHTTP / BCrypt / Crypt32` 这类 Windows 专属库。只要 `xmake` 能拉到对应包，代码本身就是按跨平台方向组织的。

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

项目默认从 `config/quarkpp.local.json` 读取本地配置，这个文件已经被 `.gitignore` 忽略，不会进入仓库。

先复制示例：

```powershell
Copy-Item config\quarkpp.example.json config\quarkpp.local.json
```

然后把你的 cookie 填进去：

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

```powershell
$env:XMAKE_GLOBALDIR=(Join-Path (Get-Location) '.xmake-global')
xmake f -y -m release
xmake build -y
```

Windows 下可执行文件默认位于：

```text
build/windows/x64/release/quarkpp.exe
```

## 命令示例

查看帮助：

```powershell
build\windows\x64\release\quarkpp.exe --help
```

查看账号：

```powershell
build\windows\x64\release\quarkpp.exe account
```

列出根目录：

```powershell
build\windows\x64\release\quarkpp.exe ls
```

按远端路径列目录：

```powershell
build\windows\x64\release\quarkpp.exe ls --path /视频
```

上传文件到根目录：

```powershell
build\windows\x64\release\quarkpp.exe upload .\movie.mkv
```

上传文件到指定目录：

```powershell
build\windows\x64\release\quarkpp.exe upload .\movie.mkv --to-path /视频 --name movie-final.mkv
```

下载整个远端目录：

```powershell
build\windows\x64\release\quarkpp.exe download --path /视频 --out .\downloads
```

创建分享：

```powershell
build\windows\x64\release\quarkpp.exe share-create --path /视频/电影合集 --expire 7d --passcode 1234
```

转存分享：

```powershell
build\windows\x64\release\quarkpp.exe transfer "https://pan.quark.cn/s/xxxx" --passcode 1234 --to-path /收藏
```

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

## 注意

- 这是 CLI 客户端，目前没有 GUI。
- 由于使用了网页接口，夸克后续改接口时可能需要跟进。
- 我没有把真实 cookie 写进仓库文件；请只放到本地忽略配置或环境变量里。
- 当前代码已经去掉 Windows 专属网络与加密实现；如果后续要在 Linux/macOS 上正式发布，主要剩下 CI、打包和实际运行验证。
