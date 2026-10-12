# catbox (TypeScript, Bun)

A TypeScript port of `catbox_sdk` for [Bun](https://bun.sh): an importable library and a `catbox`
command-line tool for [catbox.moe](https://catbox.moe) and [litterbox](https://litterbox.catbox.moe).
Runs on macOS, Linux and Windows, and compiles to a single executable that doesn't need Bun installed.

## Build

Requires Bun 1.4 or newer ([install](https://bun.sh/docs/installation)). There are no runtime
dependencies; `bun install` only fetches TypeScript and the Bun type definitions for type-checking.

```bash
cd ts
bun install
bun src/cli.ts --help        # run straight from source, no build step
bun run build                # single-file executable: dist/catbox (dist/catbox.exe on Windows)
```

## CLI

```bash
# Permanent upload (200 MB max). Prints one URL per line.
catbox upload photo.jpg video.mp4
catbox upload --user-hash YOUR_HASH photo.jpg   # or set CATBOX_USERHASH

# Let catbox fetch a remote file
catbox upload-url https://example.com/image.png

# Temporary upload (1 GB max), kept for 1h, 12h, 24h or 72h (default)
catbox litter -t 1h build.zip
```

Progress is drawn on stderr when it is a terminal (hide it with `-q`), so stdout holds only the
URLs, which makes it easy to pipe: `catbox upload *.png | pbcopy`. If any upload fails, the
remaining files are still uploaded and the command exits with status 1; bad usage exits with 2.
The behavior is shared with the other implementations in this repository; see
[`conformance/SPEC.md`](../conformance/SPEC.md).

## Library

```ts
import { CatboxClient, CatboxError, LitterboxClient } from "catbox-sdk";

const catbox = new CatboxClient(); // or new CatboxClient({ userHash: "..." })
const url = await catbox.uploadFile("photo.jpg");
const copy = await catbox.uploadUrl("https://example.com/image.png");
const fromMemory = await catbox.uploadData(new TextEncoder().encode("hello"), "notes.txt");

const litterbox = new LitterboxClient();
const temp = await litterbox.uploadFile("build.zip", {
  expiry: "1h", // type Expiry = "1h" | "12h" | "24h" | "72h"
  progress: (sent, total) => console.log(`${sent}/${total}`),
});

try {
  await catbox.uploadFile("huge.iso", { signal: AbortSignal.timeout(60_000) });
} catch (error) {
  if (error instanceof CatboxError) console.error("upload failed:", error.message, error.status);
}
```

Every upload method returns the file's full URL and rejects with `CatboxError` when catbox
rejects the upload or can't be reached (`status` holds the HTTP status for non-2xx replies).
`uploadData` takes a `Blob`, `Uint8Array` or `ArrayBuffer`. Files are streamed from disk rather
than read into memory.

The package isn't published to npm. Bun imports the TypeScript sources directly, so another
project can depend on this directory: `bun add ../path/to/catbox-sdk/ts`.

## How it's built

The library and the command are separate entry points of one package (`exports` and `bin` in
`package.json`), and Bun runs the `.ts` files as they are, so there is no compile step for either:

```
package.json         "exports": src/index.ts, "bin": src/cli.ts, no dependencies
tsconfig.json        strict type-checking only (noEmit)
src/
  index.ts           public API
  catbox.ts          CatboxClient: file, in-memory data, remote URL
  litterbox.ts       LitterboxClient and the Expiry type
  multipart.ts       multipart/form-data encoder with a known length; streams file parts
  transport.ts       fetch with Content-Length and progress, response parsing
  mime.ts            extension table, the same as every other implementation
  errors.ts          CatboxError
  version.ts
  cli.ts             the catbox command: node:util parseArgs, upload, upload-url, litter
  progress.ts        the stderr progress bar
test/                bun test against a local Bun.serve (no network)
```

Things worth knowing about the implementation:

- **Only platform APIs.** HTTP is the global `fetch`, argument parsing is `node:util`'s
  `parseArgs`, files are `Bun.file`. There is nothing to install at runtime.
- **Streaming with an exact `Content-Length`.** catbox rejects chunked uploads, but `fetch` sends a
  `ReadableStream` body chunked unless told otherwise. Bun keeps an explicit `Content-Length`
  header for stream bodies, so `MultipartForm` computes the body size up front (header bytes +
  file sizes) and `stream()` yields the parts one chunk at a time, reading files lazily through
  `Bun.file(path).stream()`. A 30 MB upload stays flat in memory. Two simpler-looking options
  don't work in Bun 1.4: `new Blob([header, Bun.file(path), footer])` silently drops the
  file's bytes, and `FormData` with a `Bun.file` reads the whole file into memory first.
- **Upload progress without upload events.** `fetch` has no upload-progress events, so the body
  stream counts bytes as `fetch` pulls them. Against a server throttled to about 10 MB/s, those
  counts ran ahead of the socket by only the OS send buffer (100% was reported ~0.4 s before the
  response), which is plenty for a progress bar.
- **Files are checked, not trusted.** `uploadFile` stats the path first (so a missing file or a
  directory fails without a request), and the stream fails the upload if a file shrank after its
  size went into `Content-Length`, rather than sending a malformed request.
- **No ANSI escape codes.** The progress bar redraws by writing `\r` and padding with spaces,
  which also works in older Windows consoles, and is only drawn when `process.stderr.isTTY`.
  `node:path` handles `\` and `/` in file names on Windows.
- **stdout carries only results** (one URL per line), so the tool composes with pipes.
- **A testable command.** `main(argv, io)` takes its output streams, environment and TTY flag as
  arguments and returns the exit status; the file runs it only when it is the entry point
  (`import.meta.main`).

### Release binaries

`bun build --compile` bundles the code with the Bun runtime into one executable, and can
cross-compile from any host:

```bash
bun run build      # this machine: dist/catbox
bun build src/cli.ts --compile --minify --target=bun-linux-x64    --outfile dist/catbox-linux-x64
bun build src/cli.ts --compile --minify --target=bun-linux-arm64  --outfile dist/catbox-linux-arm64
bun build src/cli.ts --compile --minify --target=bun-darwin-arm64 --outfile dist/catbox-darwin-arm64
bun build src/cli.ts --compile --minify --target=bun-windows-x64  --outfile dist/catbox-windows-x64.exe
```

The binaries are large because they contain the whole runtime (JavaScriptCore and Bun's APIs);
the bundled code itself is 10 KB. Sizes with Bun 1.4.3:

| Target | Size | gzipped |
|---|---|---|
| `bun-darwin-arm64` | 71 MB | 31 MB |
| `bun-linux-x64` | 91 MB | 42 MB |
| `bun-linux-arm64` | 90 MB | 42 MB |
| `bun-windows-x64` | 96 MB | 44 MB |

The Linux binaries link glibc dynamically; for Alpine and other musl systems use the
`bun-linux-x64-musl` and `bun-linux-arm64-musl` targets.

## Development

```bash
cd ts
bun install
bun test
bun run typecheck
python3 ../conformance/run.py -- bun src/cli.ts
bun run build && python3 ../conformance/run.py -- dist/catbox
```
