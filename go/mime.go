package catbox

import (
	"path/filepath"
	"strings"
)

// mimeTypes maps lower-case file extensions to Content-Types. It is a fixed table rather than
// the mime package, which consults system files and so answers differently per platform.
// Keep it in sync with _MIME_TYPES in the Python package.
var mimeTypes = map[string]string{
	"7z":   "application/x-7z-compressed",
	"aac":  "audio/aac",
	"avif": "image/avif",
	"bmp":  "image/bmp",
	"css":  "text/css",
	"csv":  "text/csv",
	"flac": "audio/flac",
	"gif":  "image/gif",
	"gz":   "application/gzip",
	"heic": "image/heic",
	"htm":  "text/html",
	"html": "text/html",
	"ico":  "image/vnd.microsoft.icon",
	"jpeg": "image/jpeg",
	"jpg":  "image/jpeg",
	"js":   "text/javascript",
	"json": "application/json",
	"m4a":  "audio/mp4",
	"md":   "text/markdown",
	"mkv":  "video/x-matroska",
	"mov":  "video/quicktime",
	"mp3":  "audio/mpeg",
	"mp4":  "video/mp4",
	"ogg":  "audio/ogg",
	"opus": "audio/opus",
	"pdf":  "application/pdf",
	"png":  "image/png",
	"svg":  "image/svg+xml",
	"tar":  "application/x-tar",
	"tif":  "image/tiff",
	"tiff": "image/tiff",
	"txt":  "text/plain",
	"wav":  "audio/wav",
	"webm": "video/webm",
	"webp": "image/webp",
	"xml":  "application/xml",
	"zip":  "application/zip",
}

// contentType guesses a Content-Type from the file extension, case-insensitively,
// falling back to application/octet-stream. Like Python's os.path.splitext, leading dots
// don't start an extension, so ".png" has none.
func contentType(filename string) string {
	name := strings.TrimLeft(filepath.Base(filename), ".")
	extension := strings.ToLower(strings.TrimPrefix(filepath.Ext(name), "."))
	if t, ok := mimeTypes[extension]; ok {
		return t
	}
	return "application/octet-stream"
}
