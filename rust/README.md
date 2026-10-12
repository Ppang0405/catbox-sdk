# catbox (Rust)

A Rust port of `catbox_sdk`: a `catbox` library crate and a `catbox` command-line tool for
[catbox.moe](https://catbox.moe) and [litterbox](https://litterbox.catbox.moe).
Runs on macOS, Linux and Windows.

## Build

Requires a stable Rust toolchain ([install](https://rustup.rs)). No system libraries are needed:
TLS is rustls, not OpenSSL.

```bash
cd rust
cargo build --release
# binary: target/release/catbox (catbox.exe on Windows)

cargo test                                          # unit + localhost integration tests
python3 ../conformance/run.py -- target/release/catbox
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
remaining files are still uploaded, each failure is reported as `catbox: <file>: <reason>` on
stderr, and the command exits with status 1. Usage errors exit with status 2 and send nothing.
The full contract is in [`conformance/SPEC.md`](../conformance/SPEC.md).

## Library

```toml
[dependencies]
catbox = { git = "https://github.com/Ppang0405/catbox-sdk" }
```

```rust
use catbox::{CatboxClient, Expiry, LitterboxClient};

let catbox = CatboxClient::new(); // or CatboxClient::new().with_user_hash("...")
let url = catbox.upload_file("photo.jpg", None)?;
let copy = catbox.upload_url("https://example.com/a.png")?;
let from_memory = catbox.upload_bytes(b"hello", "notes.txt", None)?;
let from_reader = catbox.upload_reader(std::fs::File::open("a.bin")?, "a.bin", None)?;

let litterbox = LitterboxClient::new();
let temp = litterbox.upload_file("build.zip", Expiry::OneHour, Some(&mut |sent, total| {
    eprintln!("{sent}/{total}");
}))?;

match catbox.upload_file("huge.iso", None) {
    Ok(url) => println!("{url}"),
    Err(error) => eprintln!("upload failed: {error} (HTTP status {:?})", error.status()),
}
```

Every method blocks until the upload finishes and returns the full file URL, or an `Error`:
`Io` (the file couldn't be read), `Network` (couldn't connect, TLS...), `Http { status, body }`
(non-2xx answer) or `UnexpectedResponse` (a 2xx answer that isn't a URL; catbox reports most
errors that way). Files are streamed from disk rather than loaded into memory. The optional
progress callback gets the bytes sent so far and the total request size.

## How it's built

One crate with a library and a thin binary, so the upload logic can be reused and tested
without the CLI:

```
Cargo.toml                  dependencies (clap, ureq) and the size-optimized release profile
src/
  lib.rs                    public API re-exports, VERSION, ProgressCallback
  catbox.rs                 CatboxClient: file, in-memory bytes, seekable reader, remote URL
  litterbox.rs              LitterboxClient and the Expiry enum (FromStr/Display: 1h ... 72h)
  file.rs                   opens a file part: base name, exact size, rejects directories
  multipart.rs              streaming multipart/form-data encoder with a known total length
  transport.rs              ureq POST, progress-counting reader, response parsing
  mime.rs                   extension -> Content-Type table (same as the Python one)
  error.rs                  Error enum with HTTP status, hand-written Display/Error impls
  bin/catbox/
    main.rs                 clap derive commands: upload, upload-url, litter
    progress.rs             stderr progress bar
tests/client.rs             the library against a one-shot HTTP server on 127.0.0.1
```

Things worth pointing out:

- **Exact `Content-Length`, no chunked body.** `multipart.rs` builds the body as a queue of
  segments: encoded headers (in memory), each file (a reader limited to its size), and the closing
  boundary. Their lengths add up before anything is sent, so `transport.rs` sets
  `Content-Length` and ureq sends a length-delimited body while reading the file in chunks.
  If a file shrinks mid-upload the body reader fails instead of sending a short request.
- **Two dependencies.** `clap` (derive) gives subcommands, `--help`, `--version`, value
  validation (`--time` lists its possible values) and exit status 2 for usage errors. `ureq` is a
  small blocking HTTP client; with `default-features = false, features = ["rustls"]` it uses
  rustls with bundled Mozilla roots (`webpki-roots`), so there is no OpenSSL and nothing to
  install on any OS. The multipart encoder, MIME table, error type and random boundary
  (seeded from std's `RandomState`) are written by hand rather than pulling in more crates.
- **Portable terminal handling.** `std::io::IsTerminal` decides whether to draw progress, and
  the bar redraws with `\r` and space padding instead of ANSI escape codes, which older Windows
  consoles don't support. Errors use fixed wording (`no such file`, `is a directory`) instead of
  the OS message, which differs between platforms.
- **stdout carries only results** (one URL per line), so the tool composes with pipes. A failed
  write to stdout (for example a closed pipe) is reported as a failure rather than a panic.
- **`--endpoint` is hidden** from `--help` and only accepts `http://` or `https://` URLs; it exists
  so the conformance suite can point the CLI at its mock server. `CATBOX_USERHASH` is read by hand
  instead of with clap's `env` feature, which would print the hash in `--help`.

### Release binaries

The release profile in `Cargo.toml` optimizes for size (`opt-level = "z"`, `lto = true`,
`codegen-units = 1`, `strip = true`, `panic = "abort"`). Measured on this crate:

| Build | Size |
|---|---|
| macOS arm64, default `release` profile | 3.3 MB (3,348,896 bytes) |
| macOS arm64, + `strip` | 2.7 MB |
| macOS arm64, + `lto`, `codegen-units = 1` | 2.0 MB |
| macOS arm64, + `opt-level = "z"`, `panic = "abort"` (the committed profile) | **1.3 MB** (1,319,152 bytes) |
| Linux aarch64, static musl, committed profile | **1.6 MB** (1,577,352 bytes) |

Uploading a 256 MB file to the local mock server took the same time (about 0.2 s) with every
profile, so the smaller one costs nothing noticeable: uploads are network-bound and the crypto
in `ring` is assembly either way. The CI workflow prints the size of the Linux, macOS and Windows
binaries it builds.

```bash
# macOS universal binary (arm64 + x86_64); depends only on system libraries
rustup target add aarch64-apple-darwin x86_64-apple-darwin
cargo build --release --target aarch64-apple-darwin
cargo build --release --target x86_64-apple-darwin
lipo -create -output catbox \
  target/aarch64-apple-darwin/release/catbox target/x86_64-apple-darwin/release/catbox

# Linux, fully static, runs on any distribution. On Linux, install musl-tools (for ring's C code):
rustup target add x86_64-unknown-linux-musl
cargo build --release --target x86_64-unknown-linux-musl
# From macOS or anywhere with Docker:
docker run --rm -v "$PWD":/src -w /src rust:alpine \
  sh -c 'apk add musl-dev && cargo build --release --locked'

# Windows (MSVC): a single catbox.exe; +crt-static avoids needing the VC++ runtime installed
RUSTFLAGS="-C target-feature=+crt-static" cargo build --release
```
