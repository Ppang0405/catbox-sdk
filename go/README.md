# catbox (Go)

A Go port of `catbox_sdk`: a `catbox` package and a `catbox` command-line tool for
[catbox.moe](https://catbox.moe) and [litterbox](https://litterbox.catbox.moe).
Runs on macOS, Linux and Windows, and uses only the standard library.

## Build

Requires Go 1.22 or newer ([install](https://go.dev/dl/)).

```bash
cd go
go build -o bin/ ./cmd/catbox
# binary: bin/catbox (bin/catbox.exe on Windows)

# or install it into $(go env GOPATH)/bin
go install github.com/Ppang0405/catbox-sdk/go/cmd/catbox@latest
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
uploaded and the command exits with status 1. Usage errors exit with status 2 and send nothing.
Options may come before or after the files; everything after `--` is treated as a file.

## Library

```bash
go get github.com/Ppang0405/catbox-sdk/go
```

```go
import catbox "github.com/Ppang0405/catbox-sdk/go"

ctx := context.Background()

client := &catbox.Client{UserHash: os.Getenv("CATBOX_USERHASH")} // zero value: anonymous
url, err := client.UploadFile(ctx, "photo.jpg", nil)
copied, err := client.UploadURL(ctx, "https://example.com/a.png")
fromMemory, err := client.Upload(ctx, "notes.txt", strings.NewReader("hello"), nil)

litterbox := &catbox.LitterboxClient{}
temp, err := litterbox.UploadFile(ctx, "build.zip", catbox.OneHour, func(sent, total int64) {
	fmt.Printf("%d/%d\n", sent, total)
})

var apiErr *catbox.Error
if errors.As(err, &apiErr) {
	fmt.Println(apiErr.StatusCode, apiErr.Body) // the server rejected the upload
}
```

Every method returns the full file URL. When catbox answers but rejects the upload the error is a
`*catbox.Error` carrying the HTTP status and the server's message; network errors come straight
from `net/http`, and `context` cancellation works as usual. Files are streamed from disk rather
than loaded into memory. `Upload` streams any reader whose size it can find without reading it
(`*os.File`, `*bytes.Reader`, `*strings.Reader`, any `io.Seeker`) and buffers other readers,
because the request needs an exact `Content-Length`. Point `Endpoint` at a test server or set
`HTTPClient` to control timeouts and proxies.

The types are `catbox.Client` and `catbox.LitterboxClient` rather than `CatboxClient`, because Go
style avoids repeating the package name (`catbox.CatboxClient`) in exported identifiers.

## How it's built

The module splits into a library package at the root and a thin command, so the upload logic can
be reused and tested without the CLI:

```
go.mod                  module github.com/Ppang0405/catbox-sdk/go, no dependencies
catbox.go               Client: catbox.moe API for a path, an io.Reader, or a remote URL
litterbox.go            LitterboxClient and the Expiry type (1h, 12h, 24h, 72h)
multipart.go            multipart/form-data body: head + file + tail with a known length
transport.go            the POST, progress reporting, response parsing
mime.go                 extension table (same as the Python package)
errors.go               Error (status code + body) and ErrIsDirectory
catbox_test.go          requests, streaming, errors, progress, MIME lookup (httptest only)
cmd/catbox/
  main.go               flag-based subcommands: upload, upload-url, litter
  terminal.go           TTY detection and a stderr progress bar
  main_test.go          the CLI run in-process against an httptest server
```

How the request body is streamed: `mime/multipart` writes the form fields and the file part's
headers into a small buffer, then the buffer is cut and `Close` writes the closing boundary into a
second one. The body is `io.MultiReader(head, file, tail)`, and since the file size comes from
`Stat`/`Seek`, `request.ContentLength` is set exactly, so the upload is never sent chunked (catbox
and the conformance mock server require a `Content-Length`). Progress is a reader wrapped around
that body counting what `net/http` has consumed.

Things that keep it portable across macOS, Linux and Windows:

- **Standard library only.** `net/http`, `mime/multipart` and `flag` cover everything, so there
  is nothing to vendor and `CGO_ENABLED=0` builds need no C toolchain.
- **`flag` with one `FlagSet` per subcommand.** A small loop lets options follow file names
  (plain `flag` stops at the first one), `-q`/`--quiet` and `-t`/`--time` are pairs of flags bound
  to the same variable, and `-t` uses a `flag.Value` that rejects anything but the four expiry
  times. `--endpoint` is registered but left out of the hand-written help text.
- **TTY detection without syscalls.** `os.Stderr.Stat()` reports `os.ModeCharDevice` for
  terminals on Unix and for consoles on Windows, so no `isatty`, cgo or `golang.org/x/term`.
- **No ANSI escape codes.** Progress is redrawn with `\r` and padded with spaces, which works in
  every terminal, including old Windows consoles.
- **`filepath.Base` for the uploaded name**, which understands both `/` and `\` on Windows, and
  a fixed MIME table instead of `mime.TypeByExtension`, which reads system files and the Windows
  registry and so answers differently per machine.
- **stdout carries only results** (one URL per line), so the tool composes with pipes.

### Release binaries

Go cross-compiles from any machine to any other by setting `GOOS` and `GOARCH`. With cgo off the
Linux binaries are fully static, and the macOS and Windows ones need only the system libraries
every install has.

```bash
cd go
export CGO_ENABLED=0   # pure Go: static, no C toolchain needed for any target
# -trimpath drops local paths from the binary; -s -w drop the symbol and debug tables.
GOOS=darwin  GOARCH=arm64 go build -trimpath -ldflags="-s -w" -o dist/catbox-darwin-arm64      ./cmd/catbox
GOOS=darwin  GOARCH=amd64 go build -trimpath -ldflags="-s -w" -o dist/catbox-darwin-amd64      ./cmd/catbox
GOOS=linux   GOARCH=amd64 go build -trimpath -ldflags="-s -w" -o dist/catbox-linux-amd64       ./cmd/catbox
GOOS=linux   GOARCH=arm64 go build -trimpath -ldflags="-s -w" -o dist/catbox-linux-arm64       ./cmd/catbox
GOOS=windows GOARCH=amd64 go build -trimpath -ldflags="-s -w" -o dist/catbox-windows-amd64.exe ./cmd/catbox

# macOS universal binary (on a Mac)
lipo -create -output dist/catbox-darwin dist/catbox-darwin-arm64 dist/catbox-darwin-amd64
```

Measured with Go 1.27.2: the stripped binaries are 5.7 MB (darwin/arm64), 6.1 MB (darwin/amd64),
6.0 MB (linux/amd64, statically linked), 5.6 MB (linux/arm64) and 6.2 MB (windows/amd64), about
2.4 MB gzipped; the universal macOS binary is 11.9 MB. A plain `go build` without `-s -w` is
8.5 MB (darwin/arm64). Most of that is the Go runtime and
`net/http` with its TLS stack, which every binary carries in full.
