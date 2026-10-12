package catbox

import (
	"errors"
	"fmt"
)

// ErrIsDirectory is returned (wrapped in an *os.PathError) when asked to upload a directory.
var ErrIsDirectory = errors.New("is a directory")

// Error is returned when catbox or litterbox answers but rejects the upload.
// Network failures are returned as they come from net/http instead.
type Error struct {
	// StatusCode is the HTTP status of the response. Catbox reports most errors as plain
	// text with status 200, so a 2xx code here means the body was not a URL.
	StatusCode int

	// Body is the response body with surrounding whitespace removed.
	Body string
}

func (e *Error) Error() string {
	if e.StatusCode < 200 || e.StatusCode > 299 {
		if e.Body == "" {
			return fmt.Sprintf("server returned HTTP %d", e.StatusCode)
		}
		return fmt.Sprintf("server returned HTTP %d: %s", e.StatusCode, e.Body)
	}
	if e.Body == "" {
		return "server returned an empty response"
	}
	return "server error: " + e.Body
}
