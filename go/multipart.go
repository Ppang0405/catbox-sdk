package catbox

import (
	"bytes"
	"fmt"
	"io"
	"mime/multipart"
	"net/textproto"
	"strings"
)

type field struct {
	name, value string
}

// filePart is a file to send in a form, with its size known up front.
type filePart struct {
	name     string // form field name
	filename string
	r        io.Reader
	size     int64
}

// newFilePart works out how many bytes r holds. Readers that can report their size are
// streamed; anything else is read into memory, since the request needs an exact Content-Length.
func newFilePart(name, filename string, r io.Reader) (*filePart, error) {
	part := &filePart{name: name, filename: filename, r: r}
	switch v := r.(type) {
	case interface{ Len() int }: // *bytes.Reader, *bytes.Buffer, *strings.Reader
		part.size = int64(v.Len())
		return part, nil
	case io.Seeker: // *os.File on a regular file; pipes and consoles fail to seek
		if size, ok := remaining(v); ok {
			part.size = size
			return part, nil
		}
	}
	data, err := io.ReadAll(r)
	if err != nil {
		return nil, err
	}
	part.r, part.size = bytes.NewReader(data), int64(len(data))
	return part, nil
}

// remaining returns the number of bytes between the current offset and the end of s,
// leaving the offset where it was.
func remaining(s io.Seeker) (int64, bool) {
	current, err := s.Seek(0, io.SeekCurrent)
	if err != nil {
		return 0, false
	}
	end, err := s.Seek(0, io.SeekEnd)
	if err != nil {
		return 0, false
	}
	if _, err := s.Seek(current, io.SeekStart); err != nil {
		return 0, false
	}
	return end - current, true
}

// form is a multipart/form-data body. Only the file can be large, so the body is the encoded
// fields and file header, then the file itself, then the closing boundary. The two ends are
// small and built in memory, which makes the total length known before anything is sent.
type form struct {
	contentType string
	head, tail  []byte
	file        *filePart // nil when the form has no file
}

func newForm(fields []field, file *filePart) (*form, error) {
	var buf bytes.Buffer
	w := multipart.NewWriter(&buf)
	for _, f := range fields {
		if err := w.WriteField(f.name, f.value); err != nil {
			return nil, err
		}
	}
	if file != nil {
		header := make(textproto.MIMEHeader)
		header.Set("Content-Disposition", fmt.Sprintf(`form-data; name="%s"; filename="%s"`,
			escapeQuotes(file.name), escapeQuotes(file.filename)))
		header.Set("Content-Type", contentType(file.filename))
		if _, err := w.CreatePart(header); err != nil {
			return nil, err
		}
	}
	head := bytes.Clone(buf.Bytes())

	// Closing the writer emits the final boundary, which goes after the file contents.
	buf.Reset()
	if err := w.Close(); err != nil {
		return nil, err
	}
	return &form{contentType: w.FormDataContentType(), head: head, tail: buf.Bytes(), file: file}, nil
}

// size is the exact number of bytes reader produces.
func (f *form) size() int64 {
	n := int64(len(f.head) + len(f.tail))
	if f.file != nil {
		n += f.file.size
	}
	return n
}

func (f *form) reader() io.Reader {
	if f.file == nil {
		return io.MultiReader(bytes.NewReader(f.head), bytes.NewReader(f.tail))
	}
	// LimitReader keeps the body at the announced length if the file grows mid-upload.
	// If it shrinks, net/http reports the mismatch as an error.
	return io.MultiReader(bytes.NewReader(f.head), io.LimitReader(f.file.r, f.file.size), bytes.NewReader(f.tail))
}

// escapeQuotes makes a value safe inside a quoted header parameter: quotes and backslashes
// are escaped, and line breaks, which would end the header, are dropped.
var escapeQuotes = strings.NewReplacer(`\`, `\\`, `"`, `\"`, "\r", "", "\n", "").Replace
