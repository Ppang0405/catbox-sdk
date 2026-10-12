//! The contents of an uploaded file, from disk, memory or any seekable reader.

use std::fs::{self, File};
use std::io::{self, Read, Seek, SeekFrom};
use std::path::Path;

use crate::Result;
use crate::mime::mime_type;
use crate::multipart::Form;

/// A file to upload: its name, a reader positioned at its contents, and their exact size.
pub(crate) struct FilePart<'a> {
    filename: String,
    reader: Box<dyn Read + 'a>,
    len: u64,
}

impl<'a> FilePart<'a> {
    /// Opens a file on disk. Fails without touching the network when the path is missing, a
    /// directory, or not a regular file.
    pub(crate) fn open(path: &Path) -> Result<FilePart<'static>> {
        let metadata = fs::metadata(path)?;
        if metadata.is_dir() {
            return Err(io::Error::new(io::ErrorKind::IsADirectory, "is a directory").into());
        }
        if !metadata.is_file() {
            // Pipes and devices have no size up front, and the request needs a Content-Length.
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "not a regular file").into());
        }
        let file = File::open(path)?;
        // Measure the open file rather than reusing `metadata`, in case it changed in between.
        let len = file.metadata()?.len();
        Ok(FilePart {
            filename: base_name(path),
            reader: Box::new(file),
            len,
        })
    }

    pub(crate) fn from_bytes(data: &'a [u8], filename: &str) -> Self {
        FilePart {
            filename: filename.to_owned(),
            reader: Box::new(data),
            len: data.len() as u64,
        }
    }

    /// Uses everything from the reader's current position to its end.
    pub(crate) fn from_reader(mut reader: impl Read + Seek + 'a, filename: &str) -> Result<Self> {
        let start = reader.stream_position()?;
        let end = reader.seek(SeekFrom::End(0))?;
        reader.seek(SeekFrom::Start(start))?;
        Ok(FilePart {
            filename: filename.to_owned(),
            reader: Box::new(reader),
            len: end.saturating_sub(start),
        })
    }

    pub(crate) fn add_to(self, form: &mut Form<'a>, field: &str) {
        let content_type = mime_type(&self.filename);
        form.file(field, &self.filename, content_type, self.reader, self.len);
    }
}

/// The last path component, so the server never sees the directories it was given with.
fn base_name(path: &Path) -> String {
    path.file_name()
        .unwrap_or(path.as_os_str())
        .to_string_lossy()
        .into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    #[test]
    fn base_name_drops_directories() {
        assert_eq!(
            base_name(Path::new("nested/dir/my photo.png")),
            "my photo.png"
        );
        assert_eq!(base_name(Path::new("plain.txt")), "plain.txt");
    }

    #[cfg(windows)]
    #[test]
    fn base_name_understands_windows_separators() {
        assert_eq!(base_name(Path::new(r"C:\Users\me\a.jpg")), "a.jpg");
    }

    #[test]
    fn reader_size_counts_from_the_current_position() {
        let mut cursor = Cursor::new(b"0123456789".to_vec());
        cursor.set_position(4);
        let part = FilePart::from_reader(cursor, "x.bin").unwrap();
        assert_eq!(part.len, 6);
    }

    #[test]
    fn directories_are_rejected() {
        let error = FilePart::open(&std::env::temp_dir()).err().unwrap();
        assert!(matches!(error, crate::Error::Io(e) if e.kind() == io::ErrorKind::IsADirectory));
    }

    #[test]
    fn missing_files_are_not_found() {
        let error = FilePart::open(Path::new("definitely/not/here.png"))
            .err()
            .unwrap();
        assert!(matches!(error, crate::Error::Io(e) if e.kind() == io::ErrorKind::NotFound));
    }
}
