use std::io::{Read, Seek};
use std::path::Path;

use crate::file::FilePart;
use crate::multipart::Form;
use crate::transport::Transport;
use crate::{ProgressCallback, Result};

/// Permanent uploads to <https://catbox.moe> (200 MB per file).
///
/// ```no_run
/// use catbox::CatboxClient;
///
/// let client = CatboxClient::new().with_user_hash("YOUR_HASH");
/// let url = client.upload_file("photo.jpg", Some(&mut |sent, total| {
///     eprintln!("{sent}/{total}");
/// }))?;
/// # Ok::<(), catbox::Error>(())
/// ```
#[derive(Clone, Debug)]
pub struct CatboxClient {
    user_hash: Option<String>,
    endpoint: String,
    transport: Transport,
}

impl CatboxClient {
    /// The catbox.moe API URL.
    pub const DEFAULT_ENDPOINT: &'static str = "https://catbox.moe/user/api.php";

    /// An anonymous client for the public catbox.moe API.
    pub fn new() -> Self {
        CatboxClient {
            user_hash: None,
            endpoint: Self::DEFAULT_ENDPOINT.to_owned(),
            transport: Transport::new(),
        }
    }

    /// Associates uploads with a catbox account so they can be managed later.
    /// An empty hash means anonymous uploads.
    pub fn with_user_hash(mut self, user_hash: impl Into<String>) -> Self {
        let user_hash = user_hash.into();
        self.user_hash = (!user_hash.is_empty()).then_some(user_hash);
        self
    }

    /// Sends requests to another URL instead of [`Self::DEFAULT_ENDPOINT`], for example a test server.
    pub fn with_endpoint(mut self, endpoint: impl Into<String>) -> Self {
        self.endpoint = endpoint.into();
        self
    }

    /// Uploads a file from disk, streaming it, and returns its public URL.
    pub fn upload_file(
        &self,
        path: impl AsRef<Path>,
        progress: Option<ProgressCallback<'_>>,
    ) -> Result<String> {
        self.upload(FilePart::open(path.as_ref())?, progress)
    }

    /// Uploads in-memory data under the given file name and returns its public URL.
    pub fn upload_bytes(
        &self,
        data: &[u8],
        filename: &str,
        progress: Option<ProgressCallback<'_>>,
    ) -> Result<String> {
        self.upload(FilePart::from_bytes(data, filename), progress)
    }

    /// Uploads everything from the reader's current position to its end under the given file
    /// name, and returns its public URL. The reader must be seekable so the size is known before
    /// sending.
    pub fn upload_reader(
        &self,
        reader: impl Read + Seek,
        filename: &str,
        progress: Option<ProgressCallback<'_>>,
    ) -> Result<String> {
        self.upload(FilePart::from_reader(reader, filename)?, progress)
    }

    /// Asks catbox to fetch a remote URL and returns the catbox URL of the copy.
    pub fn upload_url(&self, url: &str) -> Result<String> {
        let mut form = self.form("urlupload");
        form.text("url", url);
        self.transport.post(&self.endpoint, form, None)
    }

    fn upload(&self, file: FilePart<'_>, progress: Option<ProgressCallback<'_>>) -> Result<String> {
        let mut form = self.form("fileupload");
        file.add_to(&mut form, "fileToUpload");
        self.transport.post(&self.endpoint, form, progress)
    }

    fn form<'a>(&self, reqtype: &str) -> Form<'a> {
        let mut form = Form::new();
        form.text("reqtype", reqtype);
        if let Some(user_hash) = &self.user_hash {
            form.text("userhash", user_hash);
        }
        form
    }
}

impl Default for CatboxClient {
    fn default() -> Self {
        Self::new()
    }
}
