package main

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

// fakeCatbox answers like catbox: a URL for each upload, plain-text errors for names
// starting with "bad", and HTTP 500 for names starting with "http500".
type fakeCatbox struct {
	*httptest.Server
	mu    sync.Mutex
	forms []map[string]string // fields, plus "filename" for the file part
}

func newFakeCatbox(t *testing.T) *fakeCatbox {
	t.Helper()
	f := &fakeCatbox{}
	f.Server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if err := r.ParseMultipartForm(1 << 20); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		form := map[string]string{}
		for name, values := range r.MultipartForm.Value {
			form[name] = values[0]
		}
		if files := r.MultipartForm.File["fileToUpload"]; len(files) > 0 {
			form["filename"] = files[0].Filename
		}
		f.mu.Lock()
		f.forms = append(f.forms, form)
		n := len(f.forms)
		f.mu.Unlock()

		name := form["filename"] + form["url"]
		switch {
		case strings.HasPrefix(form["filename"], "http500"):
			http.Error(w, "Internal Server Error", http.StatusInternalServerError)
		case strings.Contains(name, "bad"):
			io.WriteString(w, "No files given.")
		default:
			fmt.Fprintf(w, "https://files.catbox.moe/f%d%s", n, filepath.Ext(name))
		}
	}))
	t.Cleanup(f.Close)
	return f
}

type result struct {
	code           int
	stdout, stderr string
}

func runCLI(t *testing.T, env map[string]string, terminal bool, args ...string) result {
	t.Helper()
	var stdout, stderr bytes.Buffer
	a := &app{
		stdout:           &stdout,
		stderr:           &stderr,
		getenv:           func(key string) string { return env[key] },
		stderrIsTerminal: terminal,
	}
	code := a.run(args)
	return result{code, stdout.String(), stderr.String()}
}

func tempFiles(t *testing.T, names ...string) []string {
	t.Helper()
	dir := t.TempDir()
	paths := make([]string, len(names))
	for i, name := range names {
		paths[i] = filepath.Join(dir, name)
		if err := os.WriteFile(paths[i], []byte("data for "+name), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return paths
}

func TestUploadPrintsOneURLPerFile(t *testing.T) {
	server := newFakeCatbox(t)
	files := tempFiles(t, "a.png", "b.txt")
	r := runCLI(t, nil, false, append([]string{"upload", "--endpoint", server.URL, "--user-hash", "H"}, files...)...)
	if r.code != 0 || r.stderr != "" {
		t.Fatalf("got %+v", r)
	}
	if r.stdout != "https://files.catbox.moe/f1.png\nhttps://files.catbox.moe/f2.txt\n" {
		t.Errorf("stdout = %q", r.stdout)
	}
	for _, form := range server.forms {
		if form["reqtype"] != "fileupload" || form["userhash"] != "H" {
			t.Errorf("form = %v", form)
		}
	}
}

func TestUploadContinuesAfterFailures(t *testing.T) {
	server := newFakeCatbox(t)
	files := tempFiles(t, "one.txt", "badresp.txt", "http500.txt", "two.txt")
	missing := filepath.Join(t.TempDir(), "missing.png")
	dir := t.TempDir()
	args := []string{"upload", "--endpoint", server.URL, files[0], missing, files[1], dir, files[2], files[3]}

	r := runCLI(t, nil, false, args...)
	if r.code != 1 {
		t.Errorf("exit = %d, want 1", r.code)
	}
	if len(server.forms) != 4 {
		t.Errorf("sent %d requests, want 4 (none for the missing file or directory)", len(server.forms))
	}
	if r.stdout != "https://files.catbox.moe/f1.txt\nhttps://files.catbox.moe/f4.txt\n" {
		t.Errorf("stdout = %q", r.stdout)
	}
	wantErrors := []string{
		"catbox: " + missing + ": no such file",
		"catbox: " + files[1] + ": server error: No files given.",
		"catbox: " + dir + ": is a directory",
		"catbox: " + files[2] + ": server returned HTTP 500: Internal Server Error",
	}
	if got := strings.Split(strings.TrimSuffix(r.stderr, "\n"), "\n"); strings.Join(got, "\n") != strings.Join(wantErrors, "\n") {
		t.Errorf("stderr:\n%s\nwant:\n%s", r.stderr, strings.Join(wantErrors, "\n"))
	}
}

func TestUserHashFromEnvironment(t *testing.T) {
	for _, tc := range []struct {
		args []string
		want string
	}{
		{nil, "ENV"},
		{[]string{"--user-hash", "FLAG"}, "FLAG"},
		{[]string{"--user-hash", ""}, ""}, // an explicit empty hash means anonymous
	} {
		server := newFakeCatbox(t)
		args := append(append([]string{"upload-url", "--endpoint", server.URL}, tc.args...), "https://example.com/a.gif")
		r := runCLI(t, map[string]string{"CATBOX_USERHASH": "ENV"}, false, args...)
		if r.code != 0 {
			t.Fatalf("got %+v", r)
		}
		if got := server.forms[0]["userhash"]; got != tc.want {
			t.Errorf("%v: userhash = %q, want %q", tc.args, got, tc.want)
		}
	}
}

func TestUploadURL(t *testing.T) {
	server := newFakeCatbox(t)
	r := runCLI(t, nil, false, "upload-url", "--endpoint", server.URL, "https://example.com/a.gif", "https://bad.example/b")
	if r.code != 1 || r.stdout != "https://files.catbox.moe/f1.gif\n" {
		t.Errorf("got %+v", r)
	}
	if r.stderr != "catbox: https://bad.example/b: server error: No files given.\n" {
		t.Errorf("stderr = %q", r.stderr)
	}
	if server.forms[0]["reqtype"] != "urlupload" || server.forms[0]["url"] != "https://example.com/a.gif" {
		t.Errorf("form = %v", server.forms[0])
	}
}

func TestLitter(t *testing.T) {
	for _, tc := range []struct {
		args []string
		want string
	}{{nil, "72h"}, {[]string{"-t", "1h"}, "1h"}, {[]string{"--time", "12h"}, "12h"}, {[]string{"--time=24h"}, "24h"}} {
		server := newFakeCatbox(t)
		args := append(append([]string{"litter", "--endpoint", server.URL}, tc.args...), tempFiles(t, "a.zip")...)
		r := runCLI(t, map[string]string{"CATBOX_USERHASH": "IGNORED"}, false, args...)
		if r.code != 0 {
			t.Fatalf("got %+v", r)
		}
		if form := server.forms[0]; form["time"] != tc.want || form["userhash"] != "" {
			t.Errorf("%v: form = %v", tc.args, form)
		}
	}
}

func TestFlagsMayFollowOperands(t *testing.T) {
	server := newFakeCatbox(t)
	files := tempFiles(t, "a.txt", "-dash.txt")
	r := runCLI(t, nil, true, "litter", files[0], "-q", "--endpoint", server.URL, "-t", "1h", "--", files[1])
	if r.code != 0 || r.stderr != "" || strings.Count(r.stdout, "\n") != 2 {
		t.Fatalf("got %+v", r)
	}
	if server.forms[0]["time"] != "1h" {
		t.Errorf("form = %v", server.forms[0])
	}
}

func TestUsageErrorsSendNothing(t *testing.T) {
	server := newFakeCatbox(t)
	file := tempFiles(t, "a.txt")[0]
	for _, args := range [][]string{
		{},
		{"frobnicate", file},
		{"upload", "--endpoint", server.URL},
		{"upload", "--endpoint", server.URL, "--no-such-option", file},
		{"upload-url", "--endpoint", server.URL},
		{"upload", "--endpoint", "not a url", file},
		{"litter", "--endpoint", server.URL, "-t", "5h", file},
		{"litter", "--endpoint", server.URL, "--user-hash", "H", file},
	} {
		r := runCLI(t, nil, false, args...)
		if r.code != 2 || r.stdout != "" || r.stderr == "" {
			t.Errorf("%v: got %+v, want exit 2 with a message on stderr", args, r)
		}
	}
	if len(server.forms) != 0 {
		t.Errorf("sent %d requests, want none", len(server.forms))
	}
}

func TestHelpAndVersion(t *testing.T) {
	r := runCLI(t, nil, false, "--version")
	if r.code != 0 || r.stdout != "catbox 0.1.0\n" {
		t.Errorf("--version: got %+v", r)
	}
	r = runCLI(t, nil, false, "--help")
	if r.code != 0 || !strings.Contains(r.stdout, "upload-url") || !strings.Contains(r.stdout, "litter") {
		t.Errorf("--help: got %+v", r)
	}
	for _, cmd := range commands {
		r := runCLI(t, nil, false, cmd.name, "--help")
		if r.code != 0 || !strings.HasPrefix(r.stdout, "Usage: catbox "+cmd.name+" ") {
			t.Errorf("%s --help: got %+v", cmd.name, r)
		}
		if strings.Contains(r.stdout, "endpoint") {
			t.Errorf("%s --help shows the hidden --endpoint option", cmd.name)
		}
	}
}

func TestProgressOnlyOnTerminal(t *testing.T) {
	server := newFakeCatbox(t)
	file := tempFiles(t, "a.txt")[0]

	r := runCLI(t, nil, true, "upload", "--endpoint", server.URL, file)
	if r.code != 0 || !strings.Contains(r.stderr, "a.txt [") || !strings.HasSuffix(r.stderr, "\r") {
		t.Errorf("terminal: got %+v, want a progress bar that is erased at the end", r)
	}
	if strings.Contains(r.stderr, "\x1b") {
		t.Errorf("progress uses ANSI escapes: %q", r.stderr)
	}
	r = runCLI(t, nil, true, "upload", "-q", "--endpoint", server.URL, file)
	if r.stderr != "" {
		t.Errorf("-q: stderr = %q", r.stderr)
	}
	r = runCLI(t, nil, false, "upload", "--endpoint", server.URL, file)
	if r.stderr != "" {
		t.Errorf("not a terminal: stderr = %q", r.stderr)
	}
}

func TestProgressBar(t *testing.T) {
	var out bytes.Buffer
	bar := newProgressBar(&out, "photo.jpg")
	const total = 2 << 20
	bar.update(0, total)
	bar.update(1, total) // still 0.0%, not redrawn
	bar.update(1048471, total)
	bar.update(1<<20, total) // a shorter line, padded over the longer one
	bar.update(total, total)
	bar.finish()
	bar.update(total, total) // after finish, ignored

	last := "photo.jpg [========================] 100.0% 2.0 MB/2.0 MB"
	want := "\rphoto.jpg [>                       ]   0.0% 0 B/2.0 MB" +
		"\rphoto.jpg [===========>            ]  49.9% 1023.9 KB/2.0 MB" +
		"\rphoto.jpg [============>           ]  50.0% 1.0 MB/2.0 MB   " +
		"\r" + last +
		"\r" + strings.Repeat(" ", len(last)) + "\r"
	if out.String() != want {
		t.Errorf("drew\n%q\nwant\n%q", out.String(), want)
	}
}

func TestFormatBytes(t *testing.T) {
	for count, want := range map[int64]string{0: "0 B", 1023: "1023 B", 1024: "1.0 KB", 5 << 20: "5.0 MB", 3 << 30: "3.0 GB", 2048 << 30: "2048.0 GB"} {
		if got := formatBytes(count); got != want {
			t.Errorf("formatBytes(%d) = %q, want %q", count, got, want)
		}
	}
}
