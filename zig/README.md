# catbox (Zig)

A Zig port of `catbox_sdk`: a `catbox` library module and a `catbox` command-line tool for
[catbox.moe](https://catbox.moe) and [litterbox](https://litterbox.catbox.moe).
Runs on macOS, Linux and Windows, and uses nothing but the Zig standard library.

## Build

Requires Zig 0.17.0 ([install](https://ziglang.org/download/)). The standard library changes
between Zig releases, so older and newer versions are not expected to compile this.

```bash
cd zig
zig build                          # binary: zig-out/bin/catbox (catbox.exe on Windows)
zig build test                     # unit tests, no network needed
zig build -Doptimize=ReleaseSmall  # a small, optimized binary
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
remaining files are still uploaded, each failure is reported as `catbox: <file>: <reason>`, and
the command exits with status 1. Usage errors exit with status 2 before anything is sent.

## Library

```zig
const std = @import("std");
const catbox = @import("catbox");

pub fn main(init: std.process.Init) !void {
    const gpa = init.gpa;
    var http: std.http.Client = .{ .allocator = gpa, .io = init.io };
    defer http.deinit();

    const box: catbox.Catbox = .{ .http = &http, .user_hash = null };
    const url = try box.uploadFile(gpa, "photo.jpg", .{});
    defer gpa.free(url);

    const copy = try box.uploadUrl(gpa, "https://example.com/a.png", .{});
    defer gpa.free(copy);
    const from_memory = try box.uploadBytes(gpa, "hello", "notes.txt", .{});
    defer gpa.free(from_memory);

    const litter: catbox.Litterbox = .{ .http = &http };
    const temporary = try litter.uploadFile(gpa, "build.zip", .one_hour, .{});
    defer gpa.free(temporary);
}
```

Every upload returns the full file URL, allocated with the allocator you pass and owned by you.
File uploads are streamed from disk rather than loaded into memory. The clients are small
structs that borrow a `std.http.Client`, so one HTTP client (and its TLS certificate bundle) can
serve any number of uploads.

### Errors and diagnostics

Zig errors are plain values: `error.HttpStatus` can't carry the status code, and
`error.UnexpectedResponse` can't carry catbox's message ("No files given."), which is the part a
user needs to see. Rather than allocating an error object, each upload takes an optional
`Diagnostics` out-parameter, the same pattern as `std.json.Diagnostics` and
`std.zon.parse.Diagnostics`. On failure it holds the error, the HTTP status if the server
answered, the trimmed response text, and the network error behind a generic `WriteFailed` or
`ReadFailed`. It formats itself as a one-line message:

```zig
var diagnostics: catbox.Diagnostics = .{};
const url = box.uploadFile(gpa, path, .{ .diagnostics = &diagnostics }) catch |err| {
    // err == error.HttpStatus, diagnostics.status == .internal_server_error, ...
    std.log.err("{s}: {f}", .{ path, diagnostics }); // "server returned HTTP 500: ..."
    return err;
};
```

`Diagnostics` is a fixed-size struct (the response text is kept up to 1 KiB), so it needs no
allocator and nothing to free, and the error code alone stays usable in `switch` and `try`.
A result union (`union(enum) { ok: []u8, failed: Failure }`) was the alternative, but it gives
up `try` for every caller, including those who only want the error code.

### Progress

Zig has no closures, so a progress callback is a context pointer plus a function.
`Progress.init` builds one from any pointer and a matching method:

```zig
const Counter = struct {
    fn update(self: *@This(), sent: u64, total: u64) void { ... }
};
var counter: Counter = .{};
_ = try box.uploadFile(gpa, path, .{ .progress = .init(&counter, Counter.update) });
```

### Adding it to a project

The package lives in the `zig/` directory of this repository. Zig's package manager fetches a
whole repository and has no way to point at a subdirectory's `build.zig`, so there are two ways
to depend on it.

From a checkout (a git submodule or a vendored copy), as a path dependency, which uses this
package's `build.zig` and its `catbox` module:

```zig
// build.zig.zon
.dependencies = .{
    .catbox = .{ .path = "deps/catbox-sdk/zig" },
},

// build.zig
const catbox = b.dependency("catbox", .{ .target = target, .optimize = optimize });
exe.root_module.addImport("catbox", catbox.module("catbox"));
```

From GitHub, by fetching the repository and making a module from the library's root file (it
has no dependencies of its own, so nothing else is needed):

```bash
zig fetch --save=catbox_sdk git+https://github.com/Ppang0405/catbox-sdk#<commit>
```

```zig
// build.zig
const catbox_sdk = b.dependency("catbox_sdk", .{});
exe.root_module.addImport("catbox", b.createModule(.{
    .root_source_file = catbox_sdk.path("zig/src/root.zig"),
    .target = target,
    .optimize = optimize,
}));
```

## How it's built

`build.zig` declares the library with `b.addModule` (so dependents can import it), builds the
executable from a separate root that imports it, and runs the tests of both under `zig build test`:

```
build.zig              library module "catbox", executable "catbox", test step
build.zig.zon          package manifest (minimum_zig_version = 0.17.0)
src/
  root.zig             the module's public API: Catbox, Litterbox, Expiry, Diagnostics, ...
  Catbox.zig           catbox.moe API: file, in-memory bytes, remote URL
  Litterbox.zig        litterbox API and the Expiry enum
  transport.zig        std.http.Client request, streamed body, response parsing, Progress
  multipart.zig        multipart/form-data encoder that measures its own length
  Diagnostics.zig      error details: status, server message, underlying network error
  mime.zig             extension -> Content-Type table
  tests.zig            end-to-end tests against an in-process HTTP server
  cli/
    main.zig           the command: runs each operand, prints URLs and failures
    args.zig           hand-written argument parser, help and usage text
    ProgressBar.zig    the stderr progress bar
```

How an upload goes out:

- **Exact `Content-Length`, no chunking.** The multipart encoder writes everything before the
  file bytes (`writeHead`) and everything after (`writeTail`) separately, and measures both by
  writing them to a `std.Io.Writer.Discarding`, which counts bytes without storing them. With
  the file size from `stat`, the body length is known before the request starts, so the headers
  carry `Content-Length` and catbox (which rejects chunked uploads) accepts it.
- **Streaming from disk.** The file is read with `readPositional` straight into the HTTP body
  writer's 64 KiB buffer, so memory use is the same for a 1 KB file and a 1 GB one. If the file
  shrinks mid-upload, the upload fails with `error.FileChanged` rather than sending a short body.
- **No surprise requests.** The file is opened and measured before connecting, so a missing file
  or a directory fails without contacting the server.
- **Early answers.** If the server replies and closes while the body is still being sent (a
  413 for an oversized file, say), the client tries to read that reply and reports it instead
  of a bare "broken pipe".

Things that keep it portable across macOS, Linux and Windows:

- **Only the standard library.** `std.http.Client` implements HTTP/1.1 and TLS 1.2/1.3 itself
  and loads the operating system's root certificates, so https works without OpenSSL or libcurl.
  The Linux binaries don't even need libc; they are static and run on any distribution.
- **`std.process.Init` for arguments and environment.** On Windows, arguments arrive as UTF-16
  and are converted to WTF-8 by `Args.toSlice`, and `CATBOX_USERHASH` is read from the
  environment map, which compares names case-insensitively there as Windows does.
- **Base names on both separators.** `std.fs.path.basename` follows the target's rules, so on
  Windows `C:\photos\a.png` and `photos/a.png` both upload as `a.png`.
- **No ANSI escape codes.** The progress bar redraws with `\r` and pads over the previous line
  with spaces, which every console understands, and it is drawn only when
  `File.stderr().isTty()` says a person is watching.
- **A fixed MIME table** instead of the system's, so every platform sends the same Content-Type.
- **stdout carries only results** (one URL per line), written through a streaming writer, so the
  tool composes with pipes and `>>`.
- **`.gitattributes` pins LF line endings**, so a Windows checkout with `core.autocrlf` still
  passes `zig fmt --check`.

### Release binaries

Zig cross-compiles to any supported target from any host, with no extra toolchain or SDK:

```bash
zig build -Doptimize=ReleaseSmall -Dtarget=x86_64-linux-musl    # static Linux
zig build -Doptimize=ReleaseSmall -Dtarget=aarch64-linux-musl
zig build -Doptimize=ReleaseSmall -Dtarget=x86_64-windows       # catbox.exe
zig build -Doptimize=ReleaseSmall -Dtarget=aarch64-windows
zig build -Doptimize=ReleaseSmall -Dtarget=x86_64-macos
zig build -Doptimize=ReleaseSmall -Dtarget=aarch64-macos
# add -p out/<target> to keep each one in its own directory
```

Sizes with Zig 0.17.0, built on macOS (ReleaseSmall leaves out debug info; the Linux builds
are fully static):

| Target | Size |
|---|---|
| `x86_64-linux-musl` | 520 KB |
| `aarch64-linux-musl` | 476 KB |
| `x86_64-windows` | 802 KB |
| `aarch64-windows` | 710 KB |
| `x86_64-macos` | 479 KB |
| `aarch64-macos` | 484 KB |

Most of that is the TLS stack and certificate parsing from the standard library.
