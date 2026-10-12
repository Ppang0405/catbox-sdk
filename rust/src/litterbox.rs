use std::fmt;
use std::io::{Read, Seek};
use std::path::Path;
use std::str::FromStr;

use crate::file::FilePart;
use crate::multipart::Form;
use crate::transport::Transport;
use crate::{ProgressCallback, Result};

/// How long litterbox keeps a file before deleting it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum Expiry {
    /// `1h`
    OneHour,
    /// `12h`
    TwelveHours,
    /// `24h`
    OneDay,
    /// `72h`, the default.
    #[default]
    ThreeDays,
}

impl Expiry {
    /// Every expiry, shortest first.
    pub const ALL: [Expiry; 4] = [
        Expiry::OneHour,
        Expiry::TwelveHours,
        Expiry::OneDay,
        Expiry::ThreeDays,
    ];

    /// The value litterbox expects: `1h`, `12h`, `24h` or `72h`.
    pub const fn as_str(self) -> &'static str {
        match self {
            Expiry::OneHour => "1h",
            Expiry::TwelveHours => "12h",
            Expiry::OneDay => "24h",
            Expiry::ThreeDays => "72h",
        }
    }
}

impl fmt::Display for Expiry {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

impl FromStr for Expiry {
    type Err = ParseExpiryError;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        Expiry::ALL
            .into_iter()
            .find(|expiry| expiry.as_str() == value)
            .ok_or(ParseExpiryError)
    }
}

/// Returned when parsing a string other than `1h`, `12h`, `24h` or `72h` as an [`Expiry`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ParseExpiryError;

impl fmt::Display for ParseExpiryError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("expiry must be one of 1h, 12h, 24h or 72h")
    }
}

impl std::error::Error for ParseExpiryError {}

/// Temporary uploads to <https://litterbox.catbox.moe> (1 GB per file).
///
/// ```no_run
/// use catbox::{Expiry, LitterboxClient};
///
/// let url = LitterboxClient::new().upload_file("build.zip", Expiry::OneHour, None)?;
/// # Ok::<(), catbox::Error>(())
/// ```
#[derive(Clone, Debug)]
pub struct LitterboxClient {
    endpoint: String,
    transport: Transport,
}

impl LitterboxClient {
    /// The litterbox API URL.
    pub const DEFAULT_ENDPOINT: &'static str =
        "https://litterbox.catbox.moe/resources/internals/api.php";

    pub fn new() -> Self {
        LitterboxClient {
            endpoint: Self::DEFAULT_ENDPOINT.to_owned(),
            transport: Transport::new(),
        }
    }

    /// Sends requests to another URL instead of [`Self::DEFAULT_ENDPOINT`], for example a test server.
    pub fn with_endpoint(mut self, endpoint: impl Into<String>) -> Self {
        self.endpoint = endpoint.into();
        self
    }

    /// Uploads a file from disk, streaming it, and returns its temporary public URL.
    pub fn upload_file(
        &self,
        path: impl AsRef<Path>,
        expiry: Expiry,
        progress: Option<ProgressCallback<'_>>,
    ) -> Result<String> {
        self.upload(FilePart::open(path.as_ref())?, expiry, progress)
    }

    /// Uploads in-memory data under the given file name and returns its temporary public URL.
    pub fn upload_bytes(
        &self,
        data: &[u8],
        filename: &str,
        expiry: Expiry,
        progress: Option<ProgressCallback<'_>>,
    ) -> Result<String> {
        self.upload(FilePart::from_bytes(data, filename), expiry, progress)
    }

    /// Uploads everything from the reader's current position to its end under the given file
    /// name, and returns its temporary public URL.
    pub fn upload_reader(
        &self,
        reader: impl Read + Seek,
        filename: &str,
        expiry: Expiry,
        progress: Option<ProgressCallback<'_>>,
    ) -> Result<String> {
        self.upload(FilePart::from_reader(reader, filename)?, expiry, progress)
    }

    fn upload(
        &self,
        file: FilePart<'_>,
        expiry: Expiry,
        progress: Option<ProgressCallback<'_>>,
    ) -> Result<String> {
        let mut form = Form::new();
        form.text("reqtype", "fileupload");
        form.text("time", expiry.as_str());
        file.add_to(&mut form, "fileToUpload");
        self.transport.post(&self.endpoint, form, progress)
    }
}

impl Default for LitterboxClient {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn expiry_round_trips_through_strings() {
        for expiry in Expiry::ALL {
            assert_eq!(expiry.to_string().parse::<Expiry>(), Ok(expiry));
        }
        assert_eq!("12h".parse(), Ok(Expiry::TwelveHours));
    }

    #[test]
    fn expiry_rejects_other_values() {
        assert_eq!("5h".parse::<Expiry>(), Err(ParseExpiryError));
        assert_eq!("1H".parse::<Expiry>(), Err(ParseExpiryError));
        assert_eq!("".parse::<Expiry>(), Err(ParseExpiryError));
    }

    #[test]
    fn default_expiry_is_three_days() {
        assert_eq!(Expiry::default(), Expiry::ThreeDays);
        assert_eq!(Expiry::default().as_str(), "72h");
    }
}
