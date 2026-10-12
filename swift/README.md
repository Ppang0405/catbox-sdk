# catbox (Swift)

A Swift port of `catbox_sdk`: a `CatboxKit` library and a `catbox` command-line tool for
[catbox.moe](https://catbox.moe) and [litterbox](https://litterbox.catbox.moe).
Runs on macOS, Linux and Windows.

## Build

Requires Swift 5.9 or newer ([install](https://www.swift.org/install/)).

```bash
cd swift
swift build -c release
# binary: .build/release/catbox (catbox.exe on Windows)
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

Progress is drawn on stderr (hide it with `-q`), so stdout holds only the URLs, which makes it
easy to pipe: `catbox upload *.png | pbcopy`. If any upload fails, the remaining files are still
uploaded and the command exits with status 1.

## Library

```swift
import CatboxKit

let catbox = CatboxClient(userHash: nil)
let url = try await catbox.upload(fileAt: URL(fileURLWithPath: "photo.jpg"))
let copy = try await catbox.upload(remoteURL: URL(string: "https://example.com/a.png")!)
let fromMemory = try await catbox.upload(data: bytes, filename: "notes.txt")

let litterbox = LitterboxClient()
let temp = try await litterbox.upload(fileAt: fileURL, expiry: .oneHour) { sent, total in
    print("\(sent)/\(total)")
}
```

Every method returns the full file URL and throws `CatboxError` when catbox rejects the upload.
File uploads are streamed from disk rather than loaded into memory.

## How it's built

The package splits into a library and a thin executable, so the upload logic can be reused and
tested without the CLI:

```
Package.swift                     products: CatboxKit (library) + catbox (executable)
Sources/CatboxKit/
  CatboxClient.swift              catbox.moe API: file, in-memory data, remote URL
  LitterboxClient.swift           litterbox API with typed expiry
  MultipartForm.swift             multipart/form-data encoder, streams files to a temp file
  Transport.swift                 URLSession upload + progress delegate, response parsing
  MIMEType.swift                  extension table (UniformTypeIdentifiers is Apple-only)
  CatboxError.swift
Sources/catbox/
  CatboxCommand.swift             swift-argument-parser commands: upload, upload-url, litter
  Terminal.swift                  TTY detection and a stderr progress bar
Tests/CatboxKitTests/             encoding, response parsing, MIME lookup (no network)
```

Things that keep it portable across macOS, Linux and Windows:

- **Only Foundation for networking.** On Linux and Windows `URLSession` lives in
  `FoundationNetworking`, hence the `#if canImport(FoundationNetworking)` imports.
- **`swift-argument-parser`** gives subcommands, `--help`, `--version`, validation and shell
  completion from property wrappers.
- **No ANSI escape codes or `stderr` C global.** Progress is redrawn by padding with spaces and
  written through `FileHandle.standardError`, and TTY detection uses `isatty`/`_isatty` per platform.
- **stdout carries only results** (one URL per line), so the tool composes with pipes.

### Release binaries

```bash
# macOS universal (arm64 + x86_64), depends only on system libraries
swift build -c release --arch arm64 --arch x86_64

# Linux, fully static (no Swift runtime needed) via the Static Linux SDK
# https://www.swift.org/documentation/articles/static-linux-getting-started.html
swift build -c release --swift-sdk x86_64-swift-linux-musl
```

The static Linux binary is large (~60 MB stripped, ~23 MB gzipped) because it links all of
Foundation, including ICU. The macOS binary is under 1 MB per architecture.
