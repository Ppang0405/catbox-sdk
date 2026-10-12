# catbox (C++)

A C++20 port of `catbox_sdk`: a `catbox::catbox` library and a `catbox` command-line tool for
[catbox.moe](https://catbox.moe) and [litterbox](https://litterbox.catbox.moe), built on libcurl.
Runs on macOS, Linux and Windows, and builds with Clang, GCC and MSVC.

## Build

Requires CMake 3.20+, a C++20 compiler with `<format>` (GCC 13+, Clang 17+, Apple Clang 15+ or
Visual Studio 2022) and libcurl with its development headers:

| Platform | libcurl |
|---|---|
| macOS | ships with the SDK, nothing to install |
| Debian, Ubuntu | `sudo apt install libcurl4-openssl-dev` |
| Windows | `vcpkg install curl:x64-windows`, then configure with `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake` |

```bash
cd cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build              # unit tests
# binary: build/catbox (build/Release/catbox.exe with Visual Studio)
```

The first configure downloads [CLI11](https://github.com/CLIUtils/CLI11) (pinned to v2.7.2 and
checked by SHA-256) for the command-line parser. The library itself depends only on libcurl.

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
remaining files are still uploaded and the command exits with status 1; usage errors exit with 2.
The behavior is shared with the other implementations in this repository; see
[`conformance/SPEC.md`](../conformance/SPEC.md).

## Library

```cpp
#include <catbox/catbox.hpp>

catbox::CatboxClient client;  // anonymous, or CatboxClient(catbox::UserHash("..."))
std::string url = client.upload_file("photo.jpg");
std::string copy = client.upload_url("https://example.com/a.png");

std::string notes = "hello";
std::string from_memory = client.upload_data(std::as_bytes(std::span(notes)), "notes.txt");

catbox::LitterboxClient litterbox;
std::string temp = litterbox.upload_file("build.zip", catbox::Expiry::one_hour,
                                         [](std::uint64_t sent, std::uint64_t total) {
                                             std::cerr << sent << '/' << total << '\r';
                                         });

try {
    (void)client.upload_file("missing.png");
} catch (const catbox::Error& error) {
    // error.what():           "no such file", "server error: No files given.", ...
    // error.kind():           file, network, http_status or unexpected_response
    // error.status():         std::optional<int>, set when the server answered
    // error.server_message(): the trimmed response body
}
```

Every method blocks until the server answers, returns the full file URL and throws
`catbox::Error` on failure. File uploads are streamed from disk rather than loaded into memory.
`catbox::mime_type("photo.JPG")` exposes the Content-Type table, and `catbox::Endpoint` points a
client at another server (the CLI's hidden `--endpoint` option uses it for the conformance tests).

To use the library from another CMake project, add this directory and link the target. Only the
library is built then; the CLI and tests are skipped.

```cmake
add_subdirectory(catbox-sdk/cpp)    # or FetchContent, below
target_link_libraries(my_app PRIVATE catbox::catbox)
```

```cmake
include(FetchContent)
FetchContent_Declare(catbox
  GIT_REPOSITORY https://github.com/Ppang0405/catbox-sdk.git
  GIT_TAG main
  SOURCE_SUBDIR cpp)
FetchContent_MakeAvailable(catbox)
target_link_libraries(my_app PRIVATE catbox::catbox)
```

### Why exceptions

`catbox::Error` derives from `std::runtime_error` rather than the methods returning a result type:

- `std::expected` is C++23, and this port targets C++20 without extra dependencies.
- An upload is a network round trip, so the cost of throwing is irrelevant, and the common
  caller (the CLI, or a script-like program) handles every failure the same way, in one place.
- Failures inside libcurl callbacks, including an exception thrown by your progress callback,
  are carried across the C boundary as a `std::exception_ptr` and rethrown from the upload
  method, so there is a single error channel.
- The error still carries structure (`kind()`, `status()`, `server_message()`) for callers that
  want to branch on it.

## How it's built

The project splits into a library and a thin executable, so the upload logic can be reused and
tested without the CLI:

```
CMakeLists.txt                  targets: catbox (library, alias catbox::catbox), catbox_cli, catbox_tests
include/catbox/catbox.hpp       the whole public API: clients, Error, Expiry, UserHash, Endpoint
src/
  client.cpp                    CatboxClient, LitterboxClient, Error: form fields per request type
  request.hpp, request.cpp      one multipart POST: curl_mime parts, callbacks, response parsing
  curl.hpp                      unique_ptr deleters for CURL* and curl_mime*, the global-init guard
  mime.cpp                      extension table (constexpr, sorted, binary-searched)
cli/
  main.cpp                      CLI11 subcommands: upload, upload-url, litter
  terminal.hpp, terminal.cpp    TTY detection, error lines and the stderr progress bar
tests/catbox_tests.cpp          MIME lookup, expiry, response parsing, local error paths (no network)
```

How the C++ shows through:

- **RAII for every C resource.** `CURL*` and `curl_mime*` live in `std::unique_ptr`s with
  deleters, and `Request` declares them so that destruction runs in the order libcurl requires
  (easy handle, then the mime tree, then the file streams it read from). `curl_global_init`
  runs once, from a function-local static whose destructor calls `curl_global_cleanup`; C++11
  guarantees that first call is thread-safe. The progress bar erases itself in its destructor,
  so the terminal is clean whether an upload returns or throws.
- **Strong types at the API.** `UserHash` and `Endpoint` are distinct types, so
  `CatboxClient(UserHash, Endpoint)` can't be called with its strings swapped, and litterbox
  lifetimes are an `enum class Expiry` with `to_string`/`parse_expiry`, not strings.
- **`std::filesystem` for files.** The library checks `fs::status` before building the request
  (missing, directory, not a regular file) and reads the size with `fs::file_size`, so those
  failures never reach the network.
- **No exceptions across C.** The libcurl callbacks are `noexcept`; they catch, stash a
  `std::exception_ptr` and return an abort code, and `Request::send` rethrows after
  `curl_easy_perform`.
- **Compile-time checks.** The MIME table is a `constexpr std::array` with a `static_assert` that
  it stays sorted, searched case-insensitively with `std::ranges::lower_bound` and projections.

Things that keep it portable across macOS, Linux and Windows:

- **Streaming with a known length.** File parts use `curl_mime_data_cb` over a `std::ifstream`,
  with the size from `std::filesystem::file_size`. Because every part's size is known, curl sends
  a `Content-Length` header and never a chunked body (the conformance mock rejects chunked
  requests). `curl_mime_filedata` would also stream, but it opens the file with a narrow
  `char*` path, which on Windows can't name files outside the ANSI code page unless libcurl was
  built with Unicode support; `std::ifstream` takes the `std::filesystem::path` natively. The
  part's filename is set explicitly to `path.filename()` as UTF-8.
- **UTF-8 arguments.** CLI11's `ensure_utf8` re-reads the wide command line on Windows, and
  paths are built from `std::u8string`, so non-ASCII file names survive.
- **No ANSI escape codes.** Progress is redrawn with `\r` and padding spaces, and TTY detection
  uses `isatty`/`_isatty` per platform. `std::getenv` is replaced by `_dupenv_s` under MSVC,
  which deprecates it.
- **Usage errors exit 2.** CLI11 reports parse errors with codes above 100; `main` maps them to
  the conventional 2 while keeping its messages, and `--help`/`--version` exit 0. `--endpoint`
  sits in CLI11's empty option group, which hides it from `--help`.
- **Warnings are errors in CI**: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`
  on Clang and GCC, `/W4 /permissive-` on MSVC (`-DCATBOX_WARNINGS_AS_ERRORS=ON`).

### Release binaries

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCATBOX_BUILD_TESTS=OFF
cmake --build build
cmake --install build --prefix ~/.local --strip    # installs bin/catbox

# macOS universal (arm64 + x86_64); the SDK's libcurl has both slices
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
```

The binary links libcurl and the C++ standard library dynamically, so it is small: a stripped
release build is about 475 KB on macOS arm64 (Apple Clang 21; about 1 MB universal) and about
450 KB on Linux aarch64 (GCC 13). Most of that is the CLI11 parser: a program that uses only the
library comes to about 60 KB stripped on macOS. On Linux it needs the distribution's libcurl at run time
(`libcurl4` on Debian and Ubuntu); on Windows, ship `libcurl.dll` and `zlib1.dll` from vcpkg next
to `catbox.exe` (the vcpkg toolchain copies them into the build directory). When targeting macOS
older than 13.3, note that `std::format` of floating-point values needs that deployment target.
