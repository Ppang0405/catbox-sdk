package catbox

import (
	"context"
	"fmt"
	"io"
	"net/http"
)

// DefaultLitterboxEndpoint is the litterbox API used when LitterboxClient.Endpoint is empty.
const DefaultLitterboxEndpoint = "https://litterbox.catbox.moe/resources/internals/api.php"

// Expiry is how long litterbox keeps a file before deleting it.
type Expiry string

// The expiry times litterbox accepts.
const (
	OneHour     Expiry = "1h"
	TwelveHours Expiry = "12h"
	OneDay      Expiry = "24h"
	ThreeDays   Expiry = "72h"
)

// ParseExpiry converts "1h", "12h", "24h" or "72h" to an Expiry.
func ParseExpiry(s string) (Expiry, error) {
	switch e := Expiry(s); e {
	case OneHour, TwelveHours, OneDay, ThreeDays:
		return e, nil
	}
	return "", fmt.Errorf("invalid expiry %q: must be 1h, 12h, 24h or 72h", s)
}

// LitterboxClient uploads files temporarily to litterbox.catbox.moe (1 GB per file).
// The zero value is ready to use.
type LitterboxClient struct {
	// Endpoint overrides the API URL. DefaultLitterboxEndpoint is used when it is empty.
	Endpoint string

	// HTTPClient sends the requests. http.DefaultClient is used when it is nil.
	HTTPClient *http.Client
}

// UploadFile uploads the file at path, kept for the given time, and returns its temporary URL.
// An empty expiry means ThreeDays. The file is sent under its base name and streamed from disk.
func (c *LitterboxClient) UploadFile(ctx context.Context, path string, expiry Expiry, progress ProgressFunc) (string, error) {
	if _, err := expiryOrDefault(expiry); err != nil {
		return "", err
	}
	return withFile(path, func(name string, file io.Reader) (string, error) {
		return c.Upload(ctx, name, file, expiry, progress)
	})
}

// Upload uploads the contents of r under the given filename, kept for the given time, and
// returns its temporary URL. An empty expiry means ThreeDays. See Client.Upload for how r is read.
func (c *LitterboxClient) Upload(ctx context.Context, filename string, r io.Reader, expiry Expiry, progress ProgressFunc) (string, error) {
	expiry, err := expiryOrDefault(expiry)
	if err != nil {
		return "", err
	}
	file, err := newFilePart("fileToUpload", filename, r)
	if err != nil {
		return "", err
	}
	fields := []field{{"reqtype", "fileupload"}, {"time", string(expiry)}}
	return post(ctx, c.HTTPClient, endpointOr(c.Endpoint, DefaultLitterboxEndpoint), fields, file, progress)
}

func expiryOrDefault(expiry Expiry) (Expiry, error) {
	if expiry == "" {
		return ThreeDays, nil
	}
	return ParseExpiry(string(expiry))
}
