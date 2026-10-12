use std::path::Path;

/// Extensions and their Content-Types, sorted by extension for binary search.
///
/// A fixed table rather than a crate or the OS registry, so every platform answers the same.
/// It matches the table in the Python implementation.
const MIME_TYPES: &[(&str, &str)] = &[
    ("7z", "application/x-7z-compressed"),
    ("aac", "audio/aac"),
    ("avif", "image/avif"),
    ("bmp", "image/bmp"),
    ("css", "text/css"),
    ("csv", "text/csv"),
    ("flac", "audio/flac"),
    ("gif", "image/gif"),
    ("gz", "application/gzip"),
    ("heic", "image/heic"),
    ("htm", "text/html"),
    ("html", "text/html"),
    ("ico", "image/vnd.microsoft.icon"),
    ("jpeg", "image/jpeg"),
    ("jpg", "image/jpeg"),
    ("js", "text/javascript"),
    ("json", "application/json"),
    ("m4a", "audio/mp4"),
    ("md", "text/markdown"),
    ("mkv", "video/x-matroska"),
    ("mov", "video/quicktime"),
    ("mp3", "audio/mpeg"),
    ("mp4", "video/mp4"),
    ("ogg", "audio/ogg"),
    ("opus", "audio/opus"),
    ("pdf", "application/pdf"),
    ("png", "image/png"),
    ("svg", "image/svg+xml"),
    ("tar", "application/x-tar"),
    ("tif", "image/tiff"),
    ("tiff", "image/tiff"),
    ("txt", "text/plain"),
    ("wav", "audio/wav"),
    ("webm", "video/webm"),
    ("webp", "image/webp"),
    ("xml", "application/xml"),
    ("zip", "application/zip"),
];

/// Guesses a Content-Type from a file name's extension, case-insensitively.
///
/// Unknown or missing extensions give `application/octet-stream`.
///
/// ```
/// assert_eq!(catbox::mime_type("Photo.JPG"), "image/jpeg");
/// assert_eq!(catbox::mime_type("archive.unknown"), "application/octet-stream");
/// ```
pub fn mime_type(filename: &str) -> &'static str {
    Path::new(filename)
        .extension()
        .and_then(|extension| extension.to_str())
        .map(str::to_ascii_lowercase)
        .and_then(|extension| {
            MIME_TYPES
                .binary_search_by(|(known, _)| (*known).cmp(extension.as_str()))
                .ok()
        })
        .map_or("application/octet-stream", |index| MIME_TYPES[index].1)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn table_is_sorted_for_binary_search() {
        assert!(MIME_TYPES.windows(2).all(|pair| pair[0].0 < pair[1].0));
    }

    #[test]
    fn known_extensions() {
        assert_eq!(mime_type("a.jpg"), "image/jpeg");
        assert_eq!(mime_type("a.jpeg"), "image/jpeg");
        assert_eq!(mime_type("a.png"), "image/png");
        assert_eq!(mime_type("a.gif"), "image/gif");
        assert_eq!(mime_type("a.webp"), "image/webp");
        assert_eq!(mime_type("a.txt"), "text/plain");
        assert_eq!(mime_type("a.json"), "application/json");
        assert_eq!(mime_type("a.pdf"), "application/pdf");
        assert_eq!(mime_type("a.zip"), "application/zip");
        assert_eq!(mime_type("a.mp4"), "video/mp4");
        assert_eq!(mime_type("a.7z"), "application/x-7z-compressed");
    }

    #[test]
    fn extension_is_case_insensitive() {
        assert_eq!(mime_type("UPPER.JPG"), "image/jpeg");
        assert_eq!(mime_type("Mixed.Png"), "image/png");
    }

    #[test]
    fn unknown_or_missing_extension_is_octet_stream() {
        assert_eq!(mime_type("data.unknownext"), "application/octet-stream");
        assert_eq!(mime_type("noextension"), "application/octet-stream");
        assert_eq!(mime_type(".hidden"), "application/octet-stream");
        assert_eq!(mime_type("trailing."), "application/octet-stream");
    }

    #[test]
    fn only_the_last_extension_counts() {
        assert_eq!(mime_type("archive.tar.gz"), "application/gzip");
    }
}
