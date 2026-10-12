use std::fmt;
use std::io;

/// A `Result` whose error is [`enum@Error`].
pub type Result<T, E = Error> = std::result::Result<T, E>;

/// Why an upload failed.
#[derive(Debug)]
#[non_exhaustive]
pub enum Error {
    /// The file could not be opened or read (missing, a directory, no permission...).
    Io(io::Error),
    /// The request could not be sent or the response not received (DNS, TLS, refused connection...).
    Network(Box<dyn std::error::Error + Send + Sync>),
    /// The server answered with a non-2xx status code.
    Http {
        /// The HTTP status code.
        status: u16,
        /// The response body, trimmed.
        body: String,
    },
    /// The server answered 2xx but the body was not a URL. Catbox reports most errors this way,
    /// for example `No files given.`
    UnexpectedResponse(String),
}

impl Error {
    /// The HTTP status code, when the server answered with a non-2xx status.
    pub fn status(&self) -> Option<u16> {
        match self {
            Error::Http { status, .. } => Some(*status),
            _ => None,
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Io(error) => error.fmt(f),
            Error::Network(error) => error.fmt(f),
            Error::Http { status, body } if body.is_empty() => {
                write!(f, "server returned HTTP {status}")
            }
            Error::Http { status, body } => write!(f, "server returned HTTP {status}: {body}"),
            Error::UnexpectedResponse(body) if body.is_empty() => {
                f.write_str("server returned an empty response")
            }
            Error::UnexpectedResponse(body) => write!(f, "server error: {body}"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Error::Io(error) => Some(error),
            Error::Network(error) => Some(error.as_ref()),
            Error::Http { .. } | Error::UnexpectedResponse(_) => None,
        }
    }
}

impl From<io::Error> for Error {
    fn from(error: io::Error) -> Self {
        Error::Io(error)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn messages_include_status_and_body() {
        let error = Error::Http {
            status: 500,
            body: "Internal Server Error".into(),
        };
        assert_eq!(
            error.to_string(),
            "server returned HTTP 500: Internal Server Error"
        );
        assert_eq!(error.status(), Some(500));

        let error = Error::Http {
            status: 502,
            body: String::new(),
        };
        assert_eq!(error.to_string(), "server returned HTTP 502");
    }

    #[test]
    fn messages_for_plain_text_errors() {
        let error = Error::UnexpectedResponse("No files given.".into());
        assert_eq!(error.to_string(), "server error: No files given.");
        assert_eq!(error.status(), None);
        assert_eq!(
            Error::UnexpectedResponse(String::new()).to_string(),
            "server returned an empty response"
        );
    }
}
