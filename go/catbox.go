// Package catbox uploads files to catbox.moe (permanent) and litterbox.catbox.moe (temporary).
//
// Every upload method returns the public URL of the file, or an error. When the server rejects
// an upload the error is an [*Error], which carries the HTTP status code and the server's message.
// Files are streamed from disk rather than read into memory.
package catbox

import (
	"context"
	"io"
	"net/http"
	"os"
	"path/filepath"
)

const (
	// Version is the version of this package and of the catbox command.
	Version = "0.1.0"

	// UserAgent is sent with every request.
	UserAgent = "catbox-go/" + Version

	// DefaultEndpoint is the catbox.moe API used when Client.Endpoint is empty.
	DefaultEndpoint = "https://catbox.moe/user/api.php"
)

// Client uploads files permanently to catbox.moe (200 MB per file).
// The zero value is ready to use and uploads anonymously.
type Client struct {
	// UserHash associates uploads with a catbox account so they can be managed later.
	// Uploads are anonymous when it is empty.
	UserHash string

	// Endpoint overrides the API URL. DefaultEndpoint is used when it is empty.
	Endpoint string

	// HTTPClient sends the requests. http.DefaultClient is used when it is nil.
	HTTPClient *http.Client
}

// UploadFile uploads the file at path and returns its public URL.
// The file is sent under its base name and streamed from disk.
func (c *Client) UploadFile(ctx context.Context, path string, progress ProgressFunc) (string, error) {
	return withFile(path, func(name string, file io.Reader) (string, error) {
		return c.Upload(ctx, name, file, progress)
	})
}

// Upload uploads the contents of r under the given filename and returns its public URL.
//
// The request needs an exact Content-Length, so r is streamed when its size can be found
// without reading it (an *os.File on a regular file, *bytes.Reader, *strings.Reader,
// *bytes.Buffer, or any io.Seeker) and read into memory first otherwise.
func (c *Client) Upload(ctx context.Context, filename string, r io.Reader, progress ProgressFunc) (string, error) {
	file, err := newFilePart("fileToUpload", filename, r)
	if err != nil {
		return "", err
	}
	return post(ctx, c.HTTPClient, endpointOr(c.Endpoint, DefaultEndpoint), c.fields("fileupload"), file, progress)
}

// UploadURL asks catbox to fetch a remote URL and returns the catbox URL of the copy.
func (c *Client) UploadURL(ctx context.Context, url string) (string, error) {
	fields := append(c.fields("urlupload"), field{"url", url})
	return post(ctx, c.HTTPClient, endpointOr(c.Endpoint, DefaultEndpoint), fields, nil, nil)
}

func (c *Client) fields(reqtype string) []field {
	fields := []field{{"reqtype", reqtype}}
	if c.UserHash != "" {
		fields = append(fields, field{"userhash", c.UserHash})
	}
	return fields
}

// withFile opens path and passes the file and its base name to upload.
// Directories are rejected before anything is sent.
func withFile(path string, upload func(name string, file io.Reader) (string, error)) (string, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer file.Close()

	info, err := file.Stat()
	if err != nil {
		return "", err
	}
	if info.IsDir() {
		return "", &os.PathError{Op: "upload", Path: path, Err: ErrIsDirectory}
	}
	return upload(filepath.Base(path), file)
}

func endpointOr(endpoint, fallback string) string {
	if endpoint == "" {
		return fallback
	}
	return endpoint
}
