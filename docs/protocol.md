# Protocol notes

QuarkPlusPlus implements the Quark Drive web protocol. These endpoints are not a
published, supported SDK contract. A successful local test verifies our HTTP
behavior against a controlled server; it does not establish that a particular
account can use an endpoint or that Quark will keep it unchanged.

## Hosts and credentials

The default API is `https://drive.quark.cn/1/clouddrive`, authenticated with a
cookie supplied by the caller. Requests include the Quark origin, referer and a
browser user agent. The API base can be overridden by library callers for tests
and gateways.

The desktop host, `drive-pc.quark.cn`, may require additional native request
signatures. The offline flow uses the web host because the published protocol
research below verified cookie authentication there. A browser session in Codex
is separate from the CLI process; the library does not read browser profiles or
extract cookies automatically.

The client accepts `__puus` refreshes from responses under its configured API
bases and retains repeated `Set-Cookie` fields. Refreshed credentials stay in a
thread-safe in-memory session; the caller's configuration and local files are
not changed. Other response cookies and redirects outside those API bases cannot
replace this credential. Download headers are created after URL issuance so
they include any cookie refreshed by that request.

Download links are short-lived. The library requests a new URL for each retry
and uses the same desktop-client User-Agent for URL issuance and file reads.
This keeps the large-file request identity consistent; the configurable browser
`user_agent` applies to normal API requests. The library
sends the cookie only to HTTPS hosts within `quark.cn`, or to the explicitly
configured API origin. Cross-origin redirects strip credentials in the HTTP
transport. An unfamiliar CDN host fails explicitly instead of receiving account
credentials. `download_url()` and `download_headers()` are intended to be used
together by adapters.

## QR sign-in

`QrLoginClient` creates a login token with Quark's
`/cas/ajax/getTokenForQrcodeLogin` (`client_id=532`), returns the official
`su.quark.cn` URL for local QR encoding, and polls
`/cas/ajax/getServiceTicketByQrcodeToken`. A confirmed ticket is exchanged at
`pan.quark.cn/account/info`; a drive `/config` request completes the session
with `__puus`. Credentials are returned only after the exchange succeeds.

The library keeps a domain/path-aware Cookie jar, retains repeated Set-Cookie
headers, and refuses redirects to another authority during login. It supports
expiry and cancellation without printing credentials or writing files. The CLI
adds a terminal QR renderer, a bounded polling loop, and atomic private config
replacement (owner-only Windows ACL or Unix mode 0600). QR images are generated
locally; no QR service receives the login token. `--qr-svg FILE` is an optional
local display fallback and must use a different path from the config.

The protocol was cross-checked against
[xiaoya-alist](https://github.com/xiaoyaDev/xiaoya-alist/blob/1c8d4dd0d885de580122a4dca5c9a4bd05b0ea38/glue_python/quark_cookie/quark_cookie.py),
[LitePan](https://github.com/Ponphil/LitePan/blob/e0e29c0e4c7fb4bc172d9bb66cb0292911b6693b/drivers/Quark/qrlogin.go),
and [BoxPlayer](https://github.com/gaozhangmin/boxplayer/blob/5aa925cdddb18a97a131e8503dc12b1d14cc7990/src/quark/auth.ts).

## Files and transfers

- Listing uses `/file/sort` with numbered pages. A missing `_total` does not stop
  after the first full page. Repeated pages and incomplete advertised totals
  produce errors. Directory detection accepts `dir`, `file`, and `file_type`
  representations; sizes use 64-bit integers.
- Uploads compute MD5 and SHA-1 with bounded buffers, then call
  `/file/upload/pre` and `/file/update/hash`. A hash response with `finish:true`
  already represents a fast upload; no multipart completion is sent afterward.
- Normal uploads use the server's part size and upload ID. Each part gets fresh
  authorization from `/file/upload/auth`, streams a byte range to OSS, and saves
  its ETag. Transient part failures retry that part. OSS completion sends the
  ordered ETags and Content-MD5, followed by `/file/upload/finish`.
- The sequential uploader omits `parallel_upload` in preflight. A real-account
  test found that declaring parallel mode without its additional hash-context
  protocol makes the second 4 MiB part fail with `NoHashContext`. The sequential
  request shape passed complete 32 MiB and 4,311,744,513-byte normal uploads
  after this correction. OSS
  errors expose only a bounded error-code identifier and local part size/number;
  response bodies, signatures and object keys are not included in diagnostics.
- Transfers retain 64-bit sizes and offsets. Memory does not grow with file
  size. Uploads detect source length or modification-time changes. Empty files
  use a zero-byte final part when the hash endpoint does not finish them.
- Downloads validate Content-Range and expected size before appending to a
  partial file. Ignored ranges fail without replacing an existing partial file.
  Signed URLs are refreshed on retry; resume can continue in a later CLI run.
  Completion verifies byte count, not a cryptographic checksum of the content.
- A process restart currently restarts a normal multipart upload from the
  beginning; uploaded-part checkpoints are not persisted. In-process retries
  preserve successful parts.
- Directory downloads preserve empty directories and validate remote names,
  collisions and existing local symbolic links before writing files. The
  caller's selected output directory is resolved first, allowing system aliases
  such as macOS `/var`; remote-derived components and final file destinations
  still cannot redirect writes through a symbolic link.

The multipart and basic file-operation wire fields were cross-checked against
[OpenList's Quark/UC implementation](https://github.com/OpenListTeam/OpenList/tree/main/drivers/quark_uc).
Quark's account quotas, file-size limits, download restrictions and service
availability still apply.

## Cloud-side BT downloads

`offline_parse()` previews a magnet or HTTP(S) source. `offline_add()` parses the
source, validates the selected provider `file_no` values, and submits it to a
destination folder. An empty selection means all parsed files. A comma-separated
folder `file_no` is expanded into individual numbers.

The implemented sequence is:

1. `POST /offline/download/parse?pr=ucpro&fr=pc&api_ver=2` with `parse_mode:0`,
   `entry:"download"`, and `auto_download:false`.
2. Wait for a bounded number of parsing attempts if `finished` is false.
3. `POST /offline/download/submit` with the parse token, destination folder and
   selected file numbers.
4. Query `/task?task_id=...` to observe the cloud download. Provider states are
   0 queued, 1 running, 2 complete, 3 failed and 4 cancelled.

Submission returns `state:"submitted"` and a task ID. It never claims that the
content has finished downloading. `wait_task()` reports failure/cancellation and
has a bounded wait; callers may query the same task again later. The task list
uses `/task/current/list?task_type=5`; individual task queries are the reliable
way to inspect a known task because the provider may ignore list filters.

Parse and submit use the versioned `wg` request envelope and
`X-U-Content-Encoding: wg`. The body is Base64 of a two-byte big-endian secure
number followed by AES-128-CBC ciphertext with PKCS#7 padding. The version's
16-byte key is also its IV. The defaults are secure number `14100` and protocol
key `e21247a68938d36f`; this is a public protocol constant, not an account secret.
Library configuration can override both when the service changes them.

The known vector is:

```text
wg("hello", 14100) = NxTFGiL/qySwJSpexPNDuS1r
```

Marked responses are decrypted and their version and padding checked. Plaintext
JSON responses are also supported. Unknown versions and business errors fail
explicitly. Source files, tokens and cookies are not logged by the library.

The wire contract and envelope are based on
[qrypt's documented, independently tested offline flow](https://github.com/yinzhenyu-su/qrypt/blob/736ae09adda320ce1b72d472f6f1300ae750f789/openspec/changes/add-offline-download/design.md#appendix-quark-provider-flow-decrypted-reference).
That source reports successful magnet and direct-HTTP submissions through the
web host. Its results are evidence about the protocol, not a live verification
of this implementation with your account. We implement the normal cloud
staging flow; accelerated download sessions, the desktop browser-save path,
and local `.torrent` parsing are outside the current interface.

## Verification scope

The deterministic client tests cover pagination, malformed API success flags,
64-bit metadata, path traversal rejection, multipart offsets/retries/hashes,
zero-byte uploads, fast uploads, resume, encrypted offline requests/responses,
selection validation and task failure. They also reject empty upload completion
acknowledgements, OSS error responses, unrelated download hosts and unsafe
resume transitions after URL lookup failures. Transport tests additionally exercise
streaming, Range validation and large offsets with local fixture files.

The client, transport and WebDAV suites have passed on Windows with MSVC and on
Ubuntu 24.04 under WSL with GCC 13.3 and system OpenSSL 3.0.13. Linux validation
used the same generated cpp-httplib 0.34.0 header and C++20 source, with separate
build output; it did not reconfigure the Windows xmake workspace.

These tests require no account or network access beyond a local loopback HTTP
server. Live account tests must be reported separately, with the actual
operation and size tested. No multi-gigabyte live upload/download, remote BT
completion, or operating-system mount is implied by the automated tests.
