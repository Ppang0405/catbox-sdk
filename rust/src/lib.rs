//! Clients for [catbox.moe](https://catbox.moe) and [litterbox](https://litterbox.catbox.moe).
//!
//! ```no_run
//! use catbox::{CatboxClient, Expiry, LitterboxClient};
//!
//! # fn main() -> Result<(), catbox::Error> {
//! let catbox = CatboxClient::new();
//! let url = catbox.upload_file("photo.jpg", None)?;
//! let copy = catbox.upload_url("https://example.com/image.png")?;
//!
//! let litterbox = LitterboxClient::new();
//! let temporary = litterbox.upload_file("build.zip", Expiry::OneHour, None)?;
//! # Ok(())
//! # }
//! ```
//!
//! Every upload is a blocking call that returns the file's public URL, or an [`Error`] when
//! catbox rejects the upload or cannot be reached. Files are streamed from disk with an exact
//! `Content-Length`, so they never have to fit in memory.

mod catbox;
mod error;
mod file;
mod litterbox;
mod mime;
mod multipart;
mod transport;

pub use crate::catbox::CatboxClient;
pub use crate::error::{Error, Result};
pub use crate::litterbox::{Expiry, LitterboxClient, ParseExpiryError};
pub use crate::mime::mime_type;

/// The library version, also sent in the `User-Agent` header.
pub const VERSION: &str = env!("CARGO_PKG_VERSION");

/// Called while a request body is sent, with the bytes sent so far and the total body size.
///
/// Pass one as `Some(&mut |sent, total| ...)`, or `None` to skip progress reporting.
pub type ProgressCallback<'a> = &'a mut dyn FnMut(u64, u64);
