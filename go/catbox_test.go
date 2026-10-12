package catbox

import (
	"bytes"
	"context"
	"errors"
	"io"
	"io/fs"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"testing/iotest"
)

// received is what the test server saw of one request.
type received struct {
	path          string
	userAgent     string
	contentLength int64
	chunked       bool
	fields        map[string]string
	files         map[string]receivedFile
}

type receivedFile struct {
	filename, contentType string
	data                  []byte
}

// testServer records requests and answers each with reply (by default, a catbox-style URL).
type testServer struct {
	*httptest.Server
	mu       sync.Mutex
	requests []received
	reply    func(w http.ResponseWriter, r received)
}

func newTestServer(t *testing.T) *testServer {
	t.Helper()
	s := &testServer{reply: func(w http.ResponseWriter, _ received) {
		io.WriteString(w, "https://files.catbox.moe/abc123.png\n")
	}}
	s.Server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		req := received{
			path:          r.URL.Path,
			userAgent:     r.UserAgent(),
			contentLength: r.ContentLength,
			chunked:       len(r.TransferEncoding) > 0,
			fields:        map[string]string{},
			files:         map[string]receivedFile{},
		}
		reader, err := r.MultipartReader()
		if err != nil {
			t.Errorf("not a multipart request: %v", err)
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		for {
			part, err := reader.NextPart()
			if err == io.EOF {
				break
			}
			if err != nil {
				t.Errorf("bad multipart body: %v", err)
				http.Error(w, err.Error(), http.StatusBadRequest)
				return
			}
			data, _ := io.ReadAll(part)
			if part.FileName() != "" {
				req.files[part.FormName()] = receivedFile{part.FileName(), part.Header.Get("Content-Type"), data}
			} else {
				req.fields[part.FormName()] = string(data)
			}
		}
		s.mu.Lock()
		s.requests = append(s.requests, req)
		s.mu.Unlock()
		s.reply(w, req)
	}))
	t.Cleanup(s.Close)
	return s
}

func (s *testServer) only(t *testing.T) received {
	t.Helper()
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.requests) != 1 {
		t.Fatalf("got %d requests, want 1", len(s.requests))
	}
	return s.requests[0]
}

func writeFile(t *testing.T, name string, data []byte) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func checkRequest(t *testing.T, req received, wantFields map[string]string) {
	t.Helper()
	if req.chunked || req.contentLength <= 0 {
		t.Errorf("body was chunked=%v with Content-Length %d, want a fixed length", req.chunked, req.contentLength)
	}
	if req.userAgent != "catbox-go/"+Version {
		t.Errorf("User-Agent = %q", req.userAgent)
	}
	if len(req.fields) != len(wantFields) {
		t.Errorf("fields = %v, want %v", req.fields, wantFields)
	}
	for name, want := range wantFields {
		if got := req.fields[name]; got != want {
			t.Errorf("field %s = %q, want %q", name, got, want)
		}
	}
}

func checkFile(t *testing.T, req received, filename, contentType string, data []byte) {
	t.Helper()
	file, ok := req.files["fileToUpload"]
	if !ok {
		t.Fatalf("no fileToUpload part in %+v", req)
	}
	if file.filename != filename || file.contentType != contentType {
		t.Errorf("file part = %q (%s), want %q (%s)", file.filename, file.contentType, filename, contentType)
	}
	if !bytes.Equal(file.data, data) {
		t.Errorf("uploaded %d bytes that differ from the %d expected", len(file.data), len(data))
	}
}

func TestUploadFile(t *testing.T) {
	server := newTestServer(t)
	data := bytes.Repeat([]byte("catbox"), 100_000)
	path := writeFile(t, "Photo.JPG", data)

	client := &Client{UserHash: "HASH", Endpoint: server.URL + "/user/api.php"}
	url, err := client.UploadFile(context.Background(), path, nil)
	if err != nil {
		t.Fatal(err)
	}
	if url != "https://files.catbox.moe/abc123.png" {
		t.Errorf("url = %q", url)
	}
	req := server.only(t)
	if req.path != "/user/api.php" {
		t.Errorf("posted to %s", req.path)
	}
	checkRequest(t, req, map[string]string{"reqtype": "fileupload", "userhash": "HASH"})
	checkFile(t, req, "Photo.JPG", "image/jpeg", data)
}

func TestUploadFileWithoutUserHash(t *testing.T) {
	server := newTestServer(t)
	path := writeFile(t, "empty.txt", nil)

	client := &Client{Endpoint: server.URL}
	if _, err := client.UploadFile(context.Background(), path, nil); err != nil {
		t.Fatal(err)
	}
	req := server.only(t)
	checkRequest(t, req, map[string]string{"reqtype": "fileupload"})
	checkFile(t, req, "empty.txt", "text/plain", nil)
}

func TestUploadFileRejectsMissingFilesAndDirectories(t *testing.T) {
	server := newTestServer(t)
	client := &Client{Endpoint: server.URL}
	dir := t.TempDir()

	if _, err := client.UploadFile(context.Background(), filepath.Join(dir, "missing.png"), nil); !errors.Is(err, fs.ErrNotExist) {
		t.Errorf("missing file: err = %v, want fs.ErrNotExist", err)
	}
	if _, err := client.UploadFile(context.Background(), dir, nil); !errors.Is(err, ErrIsDirectory) {
		t.Errorf("directory: err = %v, want ErrIsDirectory", err)
	}
	if len(server.requests) != 0 {
		t.Errorf("sent %d requests, want none", len(server.requests))
	}
}

func TestUploadReaders(t *testing.T) {
	data := []byte("some notes\n")
	readers := map[string]func() io.Reader{
		"bytes.Reader":   func() io.Reader { return bytes.NewReader(data) },
		"strings.Reader": func() io.Reader { return strings.NewReader(string(data)) },
		"bytes.Buffer":   func() io.Reader { return bytes.NewBuffer(data) },
		// No Len or Seek, so it is buffered to learn its size.
		"plain io.Reader": func() io.Reader { return iotest.OneByteReader(bytes.NewReader(data)) },
		// A file whose offset is past the start sends only the rest.
		"os.File at offset": func() io.Reader {
			file, err := os.Open(writeFile(t, "f", append([]byte("skip"), data...)))
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { file.Close() })
			file.Seek(4, io.SeekStart)
			return file
		},
	}
	for name, newReader := range readers {
		t.Run(name, func(t *testing.T) {
			server := newTestServer(t)
			client := &Client{Endpoint: server.URL}
			if _, err := client.Upload(context.Background(), `say "hi".md`, newReader(), nil); err != nil {
				t.Fatal(err)
			}
			req := server.only(t)
			checkRequest(t, req, map[string]string{"reqtype": "fileupload"})
			checkFile(t, req, `say "hi".md`, "text/markdown", data)
		})
	}
}

func TestUploadURL(t *testing.T) {
	server := newTestServer(t)
	client := &Client{UserHash: "HASH", Endpoint: server.URL}
	if _, err := client.UploadURL(context.Background(), "https://example.com/a.png"); err != nil {
		t.Fatal(err)
	}
	req := server.only(t)
	checkRequest(t, req, map[string]string{"reqtype": "urlupload", "userhash": "HASH", "url": "https://example.com/a.png"})
	if len(req.files) != 0 {
		t.Errorf("sent file parts %v", req.files)
	}
}

func TestLitterboxUploadFile(t *testing.T) {
	for _, tc := range []struct {
		expiry Expiry
		want   string
	}{{"", "72h"}, {OneHour, "1h"}, {TwelveHours, "12h"}, {OneDay, "24h"}, {ThreeDays, "72h"}} {
		server := newTestServer(t)
		path := writeFile(t, "build.zip", []byte("PK"))
		client := &LitterboxClient{Endpoint: server.URL + "/resources/internals/api.php"}
		if _, err := client.UploadFile(context.Background(), path, tc.expiry, nil); err != nil {
			t.Fatal(err)
		}
		req := server.only(t)
		checkRequest(t, req, map[string]string{"reqtype": "fileupload", "time": tc.want})
		checkFile(t, req, "build.zip", "application/zip", []byte("PK"))
	}
}

func TestLitterboxRejectsInvalidExpiry(t *testing.T) {
	server := newTestServer(t)
	client := &LitterboxClient{Endpoint: server.URL}
	path := writeFile(t, "a.txt", []byte("a"))
	if _, err := client.UploadFile(context.Background(), path, "5h", nil); err == nil {
		t.Error("expected an error for expiry 5h")
	}
	if len(server.requests) != 0 {
		t.Errorf("sent %d requests, want none", len(server.requests))
	}
}

func TestParseExpiry(t *testing.T) {
	for _, s := range []string{"1h", "12h", "24h", "72h"} {
		if e, err := ParseExpiry(s); err != nil || string(e) != s {
			t.Errorf("ParseExpiry(%q) = %q, %v", s, e, err)
		}
	}
	for _, s := range []string{"", "5h", "1H", "72"} {
		if _, err := ParseExpiry(s); err == nil {
			t.Errorf("ParseExpiry(%q) succeeded", s)
		}
	}
}

func TestServerErrors(t *testing.T) {
	for _, tc := range []struct {
		status    int
		body      string
		wantError string
	}{
		{500, "Internal Server Error\n", "server returned HTTP 500: Internal Server Error"},
		{412, "", "server returned HTTP 412"},
		{200, "No files given.", "server error: No files given."},
		{200, "  \n", "server returned an empty response"},
		{301, "https://files.catbox.moe/x.png", "server returned HTTP 301: https://files.catbox.moe/x.png"},
	} {
		server := newTestServer(t)
		server.reply = func(w http.ResponseWriter, _ received) {
			w.WriteHeader(tc.status)
			io.WriteString(w, tc.body)
		}
		client := &Client{Endpoint: server.URL}
		_, err := client.Upload(context.Background(), "a.txt", strings.NewReader("a"), nil)
		var catboxErr *Error
		if !errors.As(err, &catboxErr) {
			t.Fatalf("status %d: err = %v, want *Error", tc.status, err)
		}
		if catboxErr.StatusCode != tc.status || err.Error() != tc.wantError {
			t.Errorf("status %d: got %d %q, want %q", tc.status, catboxErr.StatusCode, err.Error(), tc.wantError)
		}
	}
}

func TestProgress(t *testing.T) {
	server := newTestServer(t)
	path := writeFile(t, "big.bin", make([]byte, 3<<20))

	var calls, lastSent, lastTotal int64
	progress := func(sent, total int64) {
		if sent < lastSent || sent > total {
			t.Errorf("progress went from %d to %d of %d", lastSent, sent, total)
		}
		calls++
		lastSent, lastTotal = sent, total
	}
	client := &Client{Endpoint: server.URL}
	if _, err := client.UploadFile(context.Background(), path, progress); err != nil {
		t.Fatal(err)
	}
	req := server.only(t)
	if calls < 2 || lastSent != lastTotal || lastTotal != req.contentLength {
		t.Errorf("%d calls ending at %d/%d, Content-Length %d", calls, lastSent, lastTotal, req.contentLength)
	}
}

func TestContextCancellation(t *testing.T) {
	server := newTestServer(t)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	client := &Client{Endpoint: server.URL}
	if _, err := client.UploadURL(ctx, "https://example.com/a.png"); !errors.Is(err, context.Canceled) {
		t.Errorf("err = %v, want context.Canceled", err)
	}
}

func TestContentType(t *testing.T) {
	for name, want := range map[string]string{
		"a.jpg": "image/jpeg", "A.JPEG": "image/jpeg", "a.png": "image/png", "a.gif": "image/gif",
		"a.webp": "image/webp", "a.txt": "text/plain", "a.json": "application/json",
		"a.pdf": "application/pdf", "a.zip": "application/zip", "a.mp4": "video/mp4",
		"archive.tar.gz": "application/gzip", "noextension": "application/octet-stream",
		"a.unknownext": "application/octet-stream", ".png": "application/octet-stream",
		"trailingdot.": "application/octet-stream",
	} {
		if got := contentType(name); got != want {
			t.Errorf("contentType(%q) = %q, want %q", name, got, want)
		}
	}
}

func TestFormEscapesHeaderValues(t *testing.T) {
	part := &filePart{name: "fileToUpload", filename: "a\"b\\c\r\nd.txt", r: strings.NewReader("x"), size: 1}
	f, err := newForm(nil, part)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Contains(f.head, []byte(`filename="a\"b\\cd.txt"`)) {
		t.Errorf("header not escaped:\n%s", f.head)
	}
	body, _ := io.ReadAll(f.reader())
	if int64(len(body)) != f.size() {
		t.Errorf("body is %d bytes, size() says %d", len(body), f.size())
	}
}
