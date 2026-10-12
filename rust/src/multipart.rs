//! A streaming `multipart/form-data` encoder.
//!
//! The body is a reader that chains the encoded headers, each file's contents and the closing
//! boundary. Its total length is known before anything is sent, so the request carries an exact
//! `Content-Length` instead of a chunked body, and files are never loaded into memory.

use std::collections::VecDeque;
use std::hash::{BuildHasher, RandomState};
use std::io::{self, Cursor, Read};

/// A form under construction.
pub(crate) struct Form<'a> {
    boundary: String,
    /// Encoded bytes that have not yet been moved into a segment.
    pending: Vec<u8>,
    segments: VecDeque<Segment<'a>>,
    len: u64,
}

impl<'a> Form<'a> {
    pub(crate) fn new() -> Self {
        Self::with_boundary(random_boundary())
    }

    pub(crate) fn with_boundary(boundary: String) -> Self {
        Form {
            boundary,
            pending: Vec::new(),
            segments: VecDeque::new(),
            len: 0,
        }
    }

    pub(crate) fn content_type(&self) -> String {
        format!("multipart/form-data; boundary={}", self.boundary)
    }

    /// Adds a text field.
    pub(crate) fn text(&mut self, name: &str, value: &str) {
        let header = format!(
            "--{}\r\nContent-Disposition: form-data; name=\"{}\"\r\n\r\n{value}\r\n",
            self.boundary,
            escape(name),
        );
        self.push_bytes(header.as_bytes());
    }

    /// Adds a file part whose contents are the next `len` bytes of `reader`.
    pub(crate) fn file(
        &mut self,
        name: &str,
        filename: &str,
        content_type: &str,
        reader: impl Read + 'a,
        len: u64,
    ) {
        let header = format!(
            "--{}\r\nContent-Disposition: form-data; name=\"{}\"; filename=\"{}\"\r\n\
             Content-Type: {content_type}\r\n\r\n",
            self.boundary,
            escape(name),
            escape(filename),
        );
        self.push_bytes(header.as_bytes());
        self.flush_pending();
        self.segments.push_back(Segment::Stream {
            reader: Box::new(reader),
            remaining: len,
        });
        self.len += len;
        self.push_bytes(b"\r\n");
    }

    /// Closes the form and returns its body.
    pub(crate) fn finish(mut self) -> Body<'a> {
        let closing = format!("--{}--\r\n", self.boundary);
        self.push_bytes(closing.as_bytes());
        self.flush_pending();
        Body {
            segments: self.segments,
            len: self.len,
        }
    }

    fn push_bytes(&mut self, bytes: &[u8]) {
        self.pending.extend_from_slice(bytes);
        self.len += bytes.len() as u64;
    }

    fn flush_pending(&mut self) {
        if !self.pending.is_empty() {
            let bytes = std::mem::take(&mut self.pending);
            self.segments.push_back(Segment::Bytes(Cursor::new(bytes)));
        }
    }
}

enum Segment<'a> {
    Bytes(Cursor<Vec<u8>>),
    Stream {
        reader: Box<dyn Read + 'a>,
        remaining: u64,
    },
}

/// An encoded form body: a reader of exactly [`Body::len`] bytes.
pub(crate) struct Body<'a> {
    segments: VecDeque<Segment<'a>>,
    len: u64,
}

impl Body<'_> {
    /// The total size of the body, for the `Content-Length` header.
    pub(crate) fn len(&self) -> u64 {
        self.len
    }
}

impl Read for Body<'_> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        if buf.is_empty() {
            return Ok(0);
        }
        while let Some(segment) = self.segments.front_mut() {
            let read = match segment {
                Segment::Bytes(cursor) => cursor.read(buf)?,
                Segment::Stream { remaining: 0, .. } => 0,
                Segment::Stream { reader, remaining } => {
                    let limit = buf
                        .len()
                        .min(usize::try_from(*remaining).unwrap_or(usize::MAX));
                    let read = reader.read(&mut buf[..limit])?;
                    if read == 0 {
                        // Content-Length is already promised, so a short file can't be papered over.
                        return Err(io::Error::new(
                            io::ErrorKind::UnexpectedEof,
                            "file ended before its expected size (was it modified during upload?)",
                        ));
                    }
                    *remaining -= read as u64;
                    read
                }
            };
            if read > 0 {
                return Ok(read);
            }
            self.segments.pop_front();
        }
        Ok(0)
    }
}

/// A boundary that won't plausibly occur in the uploaded data: 128 random bits.
///
/// `RandomState` is seeded randomly per process, which avoids a dependency on a `rand` crate.
fn random_boundary() -> String {
    let high = RandomState::new().hash_one(0u8);
    let low = RandomState::new().hash_one(1u8);
    format!("catbox-{high:016x}{low:016x}")
}

/// Quotes and line breaks would break out of the header, so escape or drop them.
fn escape(value: &str) -> String {
    let mut escaped = String::with_capacity(value.len());
    for c in value.chars() {
        match c {
            '\r' | '\n' => {}
            '\\' | '"' => {
                escaped.push('\\');
                escaped.push(c);
            }
            _ => escaped.push(c),
        }
    }
    escaped
}

#[cfg(test)]
mod tests {
    use super::*;

    fn encode(form: Form<'_>) -> (u64, Vec<u8>) {
        let mut body = form.finish();
        let len = body.len();
        let mut bytes = Vec::new();
        body.read_to_end(&mut bytes).unwrap();
        (len, bytes)
    }

    #[test]
    fn encodes_fields_and_files() {
        let mut form = Form::with_boundary("XYZ".into());
        form.text("reqtype", "fileupload");
        form.file("fileToUpload", "a.txt", "text/plain", &b"hello"[..], 5);
        let (len, bytes) = encode(form);

        let expected = "--XYZ\r\n\
            Content-Disposition: form-data; name=\"reqtype\"\r\n\r\n\
            fileupload\r\n\
            --XYZ\r\n\
            Content-Disposition: form-data; name=\"fileToUpload\"; filename=\"a.txt\"\r\n\
            Content-Type: text/plain\r\n\r\n\
            hello\r\n\
            --XYZ--\r\n";
        assert_eq!(String::from_utf8(bytes).unwrap(), expected);
        assert_eq!(len, expected.len() as u64);
    }

    #[test]
    fn length_matches_for_empty_and_large_files() {
        for size in [0usize, 1, 8191, 8192, 1 << 20] {
            let data = vec![7u8; size];
            let mut form = Form::new();
            form.text("time", "1h");
            form.file(
                "fileToUpload",
                "big.bin",
                "x/y",
                data.as_slice(),
                size as u64,
            );
            let (len, bytes) = encode(form);
            assert_eq!(len, bytes.len() as u64, "size {size}");
        }
    }

    #[test]
    fn small_reads_still_produce_the_whole_body() {
        let mut form = Form::with_boundary("B".into());
        form.file("f", "n", "t", &b"0123456789"[..], 10);
        let mut body = form.finish();
        let mut collected = Vec::new();
        let mut buf = [0u8; 3];
        loop {
            let read = body.read(&mut buf).unwrap();
            if read == 0 {
                break;
            }
            collected.extend_from_slice(&buf[..read]);
        }
        assert_eq!(collected.len() as u64, body.len());
        assert!(
            String::from_utf8(collected)
                .unwrap()
                .contains("\r\n0123456789\r\n")
        );
    }

    #[test]
    fn short_file_is_an_error_not_a_truncated_body() {
        let mut form = Form::new();
        form.file("f", "n", "t", &b"abc"[..], 10);
        let mut body = form.finish();
        let error = body.read_to_end(&mut Vec::new()).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::UnexpectedEof);
    }

    #[test]
    fn longer_file_is_cut_at_the_declared_length() {
        let mut form = Form::with_boundary("B".into());
        form.file("f", "n", "t", &b"abcdef"[..], 3);
        let (len, bytes) = encode(form);
        assert_eq!(len, bytes.len() as u64);
        assert!(
            String::from_utf8(bytes)
                .unwrap()
                .contains("\r\nabc\r\n--B--")
        );
    }

    #[test]
    fn escapes_quotes_and_drops_line_breaks() {
        assert_eq!(escape(r#"my "photo".png"#), r#"my \"photo\".png"#);
        assert_eq!(escape("a\\b"), "a\\\\b");
        assert_eq!(escape("evil\r\nname"), "evilname");
    }

    #[test]
    fn boundaries_are_random() {
        assert_ne!(random_boundary(), random_boundary());
        assert!(
            Form::new()
                .content_type()
                .starts_with("multipart/form-data; boundary=catbox-")
        );
    }
}
