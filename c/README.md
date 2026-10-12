# catbox (C)

A C11 port of `catbox_sdk`: a `libcatbox` library and a `catbox` command-line tool for
[catbox.moe](https://catbox.moe) and [litterbox](https://litterbox.catbox.moe), built on
[libcurl](https://curl.se/libcurl/). Builds with clang, gcc and MSVC on macOS, Linux and Windows.

## Build

Requires CMake 3.20 or newer, a C11 compiler and libcurl with its headers
(`libcurl4-openssl-dev` on Debian/Ubuntu, nothing extra on macOS, `vcpkg install curl` on Windows).

```bash
cd c
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build            # unit tests
# binary: build/bin/catbox

# Windows (Visual Studio generator, libcurl from vcpkg)
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_INSTALLATION_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build -C Release
# binary: build\bin\Release\catbox.exe, with the curl DLLs copied next to it
```

`cmake --install build --prefix /usr/local` installs the CLI, `catbox.h`, `libcatbox`, a CMake
package (`find_package(catbox)` gives `catbox::catbox`) and a `catbox.pc` for pkg-config.
Pass `-DBUILD_SHARED_LIBS=ON` for a shared library.

The conformance suite from the repository root:

```bash
python3 conformance/run.py -- c/build/bin/catbox
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
URLs, which makes it easy to pipe: `catbox upload *.png | pbcopy`. Failures are reported as
`catbox: <file>: <reason>` on stderr; the remaining files are still uploaded and the command exits
with status 1. Usage errors exit with status 2 before anything is sent.

## Library

```c
#include <catbox.h>
#include <stdio.h>
#include <stdlib.h>

static int on_progress(void *user_data, int64_t sent, int64_t total)
{
    (void)user_data;
    fprintf(stderr, "%lld/%lld\n", (long long)sent, (long long)total);
    return 0; /* non-zero cancels the upload */
}

int main(void)
{
    catbox_error err;
    catbox_options options = {0};   /* or pass NULL for all defaults */
    char *url;

    catbox_global_init();

    url = catbox_upload_file(NULL, "photo.jpg", &err);
    if (url) {
        puts(url);
        catbox_free(url);
    } else {
        fprintf(stderr, "upload failed: %s (HTTP %ld)\n", err.message, err.http_status);
    }

    options.user_hash = getenv("CATBOX_USERHASH"); /* NULL or "" means anonymous */
    url = catbox_upload_url(&options, "https://example.com/a.png", &err);
    catbox_free(url);
    url = catbox_upload_buffer(&options, "hello\n", 6, "notes.txt", &err);
    catbox_free(url);

    options.progress = on_progress;
    options.progress_data = NULL;
    url = catbox_litter_upload_file(&options, "build.zip", CATBOX_EXPIRY_1H, &err);
    catbox_free(url);

    catbox_global_cleanup();
    return 0;
}
```

Every upload function blocks, returns a newly allocated URL that the caller releases with
`catbox_free()`, and returns `NULL` on failure with the error struct filled in: a
`catbox_code` (file, network, HTTP status, server text, ...), the HTTP status (0 if none) and a
message that includes the server's text, since catbox reports most errors as plain text with
status 200. The message lives inside the struct, so there is nothing else to free. File uploads
are streamed from disk rather than loaded into memory. [`include/catbox.h`](include/catbox.h)
documents the ownership and threading rules in full.

With CMake:

```cmake
find_package(catbox 0.1 REQUIRED)        # or add_subdirectory(c) for a vendored copy
target_link_libraries(app PRIVATE catbox::catbox)
```

## How it's built

The library knows nothing about the command line, and the CLI only talks to the library through
the public header, so either can be reused or tested on its own:

```
CMakeLists.txt         library, CLI, tests, install rules; the version is read from catbox.h
include/catbox.h       the whole public API, with ownership rules
src/catbox.c           curl_mime form building, curl easy transfer, response parsing, MIME table
src/catbox_internal.h  internals the unit tests reach (response parsing)
src/main.c             the catbox command: runs each upload, prints URLs and errors
src/cli.c, cli.h       hand-written argument parser and help text
src/progress.c, .h     TTY detection and a stderr progress bar
tests/test_catbox.c    MIME lookup, base names, response parsing, argument parsing (no network)
cmake/                 package config and pkg-config templates for `cmake --install`
```

How the request is made:

- **`curl_mime` for the form.** `curl_mime_filedata` streams the file and stats it for its
  size, so curl sends a `Content-Length` and never a chunked body. In-memory uploads use
  `curl_mime_data_cb` over the caller's buffer instead of `curl_mime_data`, which would copy it.
- **The part's filename is set explicitly** to the base name with `curl_mime_filename`, rather
  than left to curl, whose choice depends on the platform's `basename()`. The library's own
  `catbox_path_basename` splits on `/`, and on Windows also on `\` and after a `C:` drive prefix.
- **The file is checked first** with `stat` (`_stat64` on Windows): a missing file, a directory or
  a FIFO (whose unknown size would force a chunked body) fails before any request is made.
- **Progress** comes from `CURLOPT_XFERINFOFUNCTION`; the callback gets bytes sent and the
  total form size, and can return non-zero to cancel.
- **The endpoint is limited to http and https**, since it can come from the command line.
- Each call uses its own easy handle and no signals (`CURLOPT_NOSIGNAL`), so uploads may run on
  several threads once `catbox_global_init()` has run.

Things that keep it portable across macOS, Linux and Windows:

- **Plain C11 and libcurl only.** No getopt (MSVC lacks it): `cli.c` parses `--name value`,
  `--name=value`, `-t value`, `-tvalue` and `--`, with options before or after the files.
- **A few `#ifdef _WIN32` spots, all small:** `_stat64` and `_S_IFDIR` instead of `stat` and
  `S_ISDIR`, `_isatty(_fileno(stderr))` instead of `isatty(STDERR_FILENO)`, `\` as a path
  separator, and `__declspec(dllexport)` for shared builds. `_CRT_SECURE_NO_WARNINGS` keeps
  MSVC from suggesting its non-portable `_s` functions.
- **No ANSI escape codes.** The progress bar redraws with `\r` and pads over the previous line
  with spaces, so it works in old Windows consoles too.
- **A fixed MIME table** (the same as the Python version's) instead of the OS's MIME database,
  so every platform sends the same `Content-Type`.
- **`catbox_free()` instead of `free()`** for returned URLs, because a DLL and the program using it
  may each have their own C runtime heap on Windows.
- **Warnings are errors in CI:** `-Wall -Wextra -Wpedantic` on clang and gcc, `/W4` on MSVC
  (`-DCATBOX_WARNINGS_AS_ERRORS=ON`).
- **stdout carries only results** (one URL per line, flushed as each upload finishes), so the
  tool composes with pipes.

Known limits: on Windows, paths and arguments go through the ANSI code page (`fopen`, `main`'s
`argv`), so file names outside it may not open; the progress bar counts one column per code
point, so wide CJK names make it misjudge how much to pad.

### Release binaries

```bash
# Smallest dynamic build: depends on the system libcurl
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=MinSizeRel -DCATBOX_BUILD_TESTS=OFF
cmake --build build-release && strip build-release/bin/catbox

# macOS universal (arm64 + x86_64); the system libcurl is universal already
cmake -S . -B build-universal -DCMAKE_BUILD_TYPE=MinSizeRel "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"

# Linux, fully static, e.g. on Alpine (musl):
#   apk add build-base cmake pkgconf curl-dev curl-static openssl-libs-static nghttp2-static \
#     zlib-static brotli-static zstd-static libidn2-static libpsl-static libunistring-static
cmake -S . -B build-static -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DCATBOX_STATIC_CURL=ON -DCMAKE_EXE_LINKER_FLAGS=-static
cmake --build build-static && strip build-static/bin/catbox
```

`CATBOX_STATIC_CURL` asks pkg-config for libcurl's `--static` link line, because a static libcurl
also needs its TLS library, zlib, nghttp2 and the rest. On Windows the equivalent would be vcpkg's
`x64-windows-static` triplet, which is untested here.

Measured sizes of the `catbox` binary:

| Build | Size |
|---|---|
| macOS arm64, Release, against the system libcurl | 36 KB stripped |
| macOS universal (arm64 + x86_64), MinSizeRel | 69 KB stripped |
| Linux aarch64 (Ubuntu 24.04, gcc 13), Release, dynamic libcurl | 67 KB stripped |
| Linux aarch64 (Alpine, musl), fully static with libcurl + OpenSSL | 7.2 MB stripped, 3.4 MB gzipped |

The dynamic builds are tiny because libcurl does the HTTP and TLS work and ships with the OS
(or, on Windows, as DLLs next to the executable). Nearly all of the static binary is OpenSSL
and libcurl.
