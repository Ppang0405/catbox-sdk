# catbox CLI specification

Every implementation in this repository ships an importable library and a `catbox` command that
behaves as described here. `conformance/run.py` checks the command against a local mock server.

## Commands

```
catbox upload     [--user-hash HASH] [-q | --quiet] [--endpoint URL] FILE...
catbox upload-url [--user-hash HASH] [--endpoint URL] URL...
catbox litter     [-t | --time 1h|12h|24h|72h] [-q | --quiet] [--endpoint URL] FILE...
catbox --version
catbox --help
```

- `upload` stores files permanently on catbox.moe.
- `upload-url` asks catbox.moe to fetch remote URLs.
- `litter` stores files temporarily on litterbox. `--time` defaults to `72h`.
- `--user-hash` falls back to the `CATBOX_USERHASH` environment variable. `litter` ignores both.
- `--endpoint` overrides the API URL. It exists for testing and may be hidden from `--help`.
- `--version` prints a line containing `0.1.0` and exits 0. `--help` exits 0.

Default endpoints:

| Command | Endpoint |
|---|---|
| `upload`, `upload-url` | `https://catbox.moe/user/api.php` |
| `litter` | `https://litterbox.catbox.moe/resources/internals/api.php` |

## Requests

One `POST` per argument, with a `multipart/form-data` body:

| Command | Fields |
|---|---|
| `upload` | `reqtype=fileupload`, `userhash` (only when non-empty), `fileToUpload` (file) |
| `upload-url` | `reqtype=urlupload`, `userhash` (only when non-empty), `url` |
| `litter` | `reqtype=fileupload`, `time`, `fileToUpload` (file) |

- The file part's `filename` is the file's base name, never the path it was given as.
- The file part's `Content-Type` comes from the extension, case-insensitively, and is
  `application/octet-stream` when unknown. At minimum: `jpg`/`jpeg` → `image/jpeg`,
  `png` → `image/png`, `gif` → `image/gif`, `webp` → `image/webp`, `txt` → `text/plain`,
  `json` → `application/json`, `pdf` → `application/pdf`, `zip` → `application/zip`,
  `mp4` → `video/mp4`.
- Requests carry a `Content-Length` header; chunked request bodies are not allowed.
- The `User-Agent` header starts with `catbox-` (for example `catbox-go/0.1.0`).
- Libraries should stream files from disk rather than loading them into memory.

## Responses

- Success: a 2xx status and a body that, trimmed, starts with `http://` or `https://`.
- Anything else is a failure. Catbox reports most errors as plain text with status 200, so the
  body text is part of the error message.

## Output and exit status

- Arguments are processed in order. After a failure, the remaining arguments are still processed.
- Each success prints the returned URL on its own line on **stdout**. Nothing else goes to stdout.
- Each failure prints one line on **stderr** naming the argument and the reason, for example
  `catbox: missing.png: no such file`. A missing file or a directory fails without a request.
- A progress indicator may be drawn on stderr, but only when stderr is a terminal and `--quiet`
  is not given. When stderr is not a terminal, a fully successful run writes nothing to stderr.
- Exit status: `0` if every argument succeeded, `1` if any failed, and `2` or higher for usage
  errors (unknown option, invalid `--time`, no arguments). Usage errors send no requests.
