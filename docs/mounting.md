# Mounting

QuarkPlusPlus provides a loopback HTTP WebDAV server. rclone supplies the OS filesystem adapter, using WinFsp on Windows and a FUSE implementation on Linux/macOS. The bridge itself is portable C++20 and does not require OS driver headers to compile.

## Setup

1. Authenticate the CLI with the local config or `QUARKPP_COOKIE`.
2. Set `QUARKPP_WEBDAV_PASSWORD` in the server's environment. The default username is `quarkpp`.
3. Start `quarkpp serve --port 8787`. For inspection-only access, use `--read-only`.
4. In `rclone config`, create `quark` as a WebDAV remote: URL `http://127.0.0.1:8787/`, vendor `other`, username `quarkpp`, and the chosen password.
5. Check `rclone lsf quark:`, then mount using the commands in the README.

Both the bridge and rclone must remain running. For a foreground mount, wait for writes to finish, then stop rclone with Ctrl+C. Linux can also use `fusermount3 -u "$HOME/Quark"`; macOS can use `umount "$HOME/Quark"`. Stop the bridge after unmounting. Do not terminate either process while writes are pending.

Set the password before starting the bridge in that same terminal:

```powershell
# PowerShell
$env:QUARKPP_WEBDAV_PASSWORD = "choose-a-local-password"
quarkpp serve --port 8787
```

```sh
# Linux / macOS shell
export QUARKPP_WEBDAV_PASSWORD='choose-a-local-password'
quarkpp serve --port 8787
```

The bridge accepts `127.0.0.1`, `::1`, and `localhost` only. Authentication is mandatory. If you deliberately expose it to another host, use a separately configured authenticated TLS reverse proxy. Its local password is distinct from the Quark cookie.

## Filesystem behavior

| Operation | Behavior |
| --- | --- |
| Directory listing | PROPFIND Depth 0/1, XML escaping and UTF-8 percent-encoded paths |
| Read / seek | GET/HEAD, 64-bit Content-Length, a single bounded or suffix Range |
| Write | PUT to an isolated temporary file, then normal multipart cloud upload |
| Overwrite | Upload a new object, rename the old object to a backup, finalize the new name, recycle the backup |
| Create/remove | MKCOL and recycle-bin DELETE |
| Rename | MOVE within the same parent; existing destinations are rejected |
| Unsupported | COPY, LOCK/UNLOCK, PROPPATCH, cross-directory MOVE, multi-range GET, arbitrary POSIX metadata |

Use `--vfs-cache-mode full` for applications that seek or rewrite files. Configure rclone's cache directory/size for your workload. rclone cache storage and the bridge's temporary storage are separate: a large write may require two local copies. `quarkpp serve --temp-dir PATH` selects an existing directory for the latter. It is cleaned after requests; abrupt process termination can leave a `quarkpp-*` temporary directory to inspect and remove.

Overwrites are not atomic and should not be used for databases or concurrent writers. If a cloud rename/rollback fails, `.quarkpp-upload-*` or `.quarkpp-backup-*` files may remain for recovery. The adapter serializes its own mutations, but it cannot lock out other clients or the website. Review remaining objects before removing them.

Writes can take longer than local file saves: rclone first flushes the VFS cache, then the bridge hashes and uploads the staged file. For slow links, increase rclone's HTTP timeout, for example `--timeout 1h`. Do not treat a closed editor window as proof of completed cloud upload; check rclone's pending work and the remote listing.

## Troubleshooting

- **401:** the WebDAV password/user differs from the rclone config.
- **502:** Quark rejected an operation or a transport request failed. Check cookie validity, quotas, API availability, and try the corresponding CLI command. WebDAV responses deliberately hide provider bodies and signed URLs.
- **507:** insufficient or inaccessible local temporary storage, or an interrupted PUT body.
- **412 on rename:** the destination exists or a precondition no longer matches.
- **400 on PROPFIND:** ensure the generated cpp-httplib header is used; bypassing the xmake rule loses WebDAV method support.
- **Large files on Windows:** use rclone/WinFsp. The Windows WebClient has its own limits and is not the supported client here.
- **macOS mount unavailable:** check the rclone distribution's FUSE support and macFUSE installation requirements.

The official Windows rclone 1.75.2 client passed `lsf`, `cat`, an eight-byte read beyond 5 GiB, and `copyto` against the local mock WebDAV server. It also passed real-account directory listing, a SHA-256-checked 1 MiB download, a 64-byte range read, a SHA-256-checked 131,089-byte upload/download, same-directory rename and recoverable deletion. A further real WebDAV check read 256 bytes at offset 4,294,968,320 from the uploaded 4,311,744,513-byte file; all bytes and SHA-256 matched the local source. The live bridge was bound to loopback and stopped afterward. Native OS driver installation and actual mounted drives require separate platform verification. See [the verification record](testing.md).
