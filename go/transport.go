package catbox

import (
	"context"
	"io"
	"net/http"
	"strings"
)

// ProgressFunc is called as a request body is sent, with the number of bytes sent so far and
// the total body size. Calls come one at a time, but may come from a goroutine other than the
// one that started the upload (net/http writes request bodies on its own goroutine).
type ProgressFunc func(sent, total int64)

// maxResponseSize bounds how much of a response is read. Catbox answers with a short line.
const maxResponseSize = 1 << 20

// post sends a multipart form and returns the URL the server answers with.
func post(ctx context.Context, client *http.Client, endpoint string, fields []field, file *filePart, progress ProgressFunc) (string, error) {
	form, err := newForm(fields, file)
	if err != nil {
		return "", err
	}
	total := form.size()
	body := form.reader()
	if progress != nil {
		body = &progressReader{r: body, total: total, progress: progress}
	}

	request, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint, body)
	if err != nil {
		return "", err
	}
	// net/http can't tell the length of a MultiReader and would otherwise send the body chunked.
	request.ContentLength = total
	request.Header.Set("Content-Type", form.contentType)
	request.Header.Set("User-Agent", UserAgent)

	if client == nil {
		client = http.DefaultClient
	}
	response, err := client.Do(request)
	if err != nil {
		return "", err
	}
	defer response.Body.Close()

	data, err := io.ReadAll(io.LimitReader(response.Body, maxResponseSize))
	if err != nil {
		return "", err
	}
	return parseResponse(response.StatusCode, data)
}

// parseResponse turns a response into the uploaded file's URL or an *Error.
func parseResponse(status int, body []byte) (string, error) {
	text := strings.TrimSpace(string(body))
	if status < 200 || status > 299 {
		return "", &Error{StatusCode: status, Body: text}
	}
	// Catbox reports most errors as plain text with status 200.
	if !strings.HasPrefix(text, "https://") && !strings.HasPrefix(text, "http://") {
		return "", &Error{StatusCode: status, Body: text}
	}
	return text, nil
}

// progressReader reports how much of the request body net/http has read so far.
type progressReader struct {
	r        io.Reader
	sent     int64
	total    int64
	progress ProgressFunc
}

func (p *progressReader) Read(b []byte) (int, error) {
	n, err := p.r.Read(b)
	if n > 0 {
		p.sent += int64(n)
		p.progress(p.sent, p.total)
	}
	return n, err
}
