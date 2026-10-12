//! Sends a form with `ureq` and turns catbox's answer into a URL or an [`Error`].

use std::io::{self, Read};
use std::time::Duration;

use ureq::{Agent, SendBody};

use crate::multipart::Form;
use crate::{Error, ProgressCallback, Result, VERSION};

/// A connection pool plus the request settings shared by every upload.
#[derive(Clone, Debug)]
pub(crate) struct Transport {
    agent: Agent,
}

impl Transport {
    pub(crate) fn new() -> Self {
        let config = Agent::config_builder()
            .user_agent(format!("catbox-rust/{VERSION}"))
            // Non-2xx answers are read like any other, so their body can go into the error.
            .http_status_as_error(false)
            // No overall timeout: a large upload on a slow link can legitimately take hours.
            .timeout_connect(Some(Duration::from_secs(30)))
            .build();
        Transport {
            agent: Agent::new_with_config(config),
        }
    }

    /// Posts a form and returns the URL catbox answers with.
    pub(crate) fn post(
        &self,
        endpoint: &str,
        form: Form<'_>,
        progress: Option<ProgressCallback<'_>>,
    ) -> Result<String> {
        let content_type = form.content_type();
        let mut body = form.finish();
        let total = body.len();
        let mut reader = ProgressReader {
            inner: &mut body,
            sent: 0,
            total,
            progress,
        };

        // Setting Content-Length explicitly makes ureq send a length-delimited body rather than
        // a chunked one, which catbox (and the conformance mock server) require.
        let response = self
            .agent
            .post(endpoint)
            .header("Content-Type", content_type)
            .header("Content-Length", total.to_string())
            .send(SendBody::from_reader(&mut reader))
            .map_err(network_error)?;

        let status = response.status().as_u16();
        let text = response
            .into_body()
            .read_to_string()
            .map_err(network_error)?;
        parse_response(status, &text)
    }
}

fn network_error(error: ureq::Error) -> Error {
    match error {
        // Unwrapped so the message reads "Connection refused (os error 61)" rather than "io: ...".
        ureq::Error::Io(error) => Error::Network(Box::new(error)),
        other => Error::Network(Box::new(other)),
    }
}

/// Accepts a 2xx answer whose trimmed body is an http(s) URL; anything else is an error.
pub(crate) fn parse_response(status: u16, body: &str) -> Result<String> {
    let text = body.trim();
    if !(200..300).contains(&status) {
        return Err(Error::Http {
            status,
            body: text.to_owned(),
        });
    }
    // Catbox reports most errors as plain text with status 200.
    if text.starts_with("https://") || text.starts_with("http://") {
        Ok(text.to_owned())
    } else {
        Err(Error::UnexpectedResponse(text.to_owned()))
    }
}

/// Reports how much of the body has been handed to the HTTP client.
struct ProgressReader<'a, R> {
    inner: R,
    sent: u64,
    total: u64,
    progress: Option<ProgressCallback<'a>>,
}

impl<R: Read> Read for ProgressReader<'_, R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let read = self.inner.read(buf)?;
        if read > 0 {
            self.sent += read as u64;
            if let Some(progress) = self.progress.as_mut() {
                progress(self.sent, self.total);
            }
        }
        Ok(read)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepts_urls_with_surrounding_whitespace() {
        assert_eq!(
            parse_response(200, " https://files.catbox.moe/abc.png\n").unwrap(),
            "https://files.catbox.moe/abc.png"
        );
        assert_eq!(
            parse_response(201, "http://example.test/x").unwrap(),
            "http://example.test/x"
        );
    }

    #[test]
    fn plain_text_with_status_200_is_an_error() {
        match parse_response(200, "No files given.") {
            Err(Error::UnexpectedResponse(body)) => assert_eq!(body, "No files given."),
            other => panic!("unexpected {other:?}"),
        }
        assert!(matches!(
            parse_response(200, "  "),
            Err(Error::UnexpectedResponse(body)) if body.is_empty()
        ));
    }

    #[test]
    fn non_2xx_status_is_an_error_even_with_a_url_body() {
        let error = parse_response(500, "https://files.catbox.moe/abc.png").unwrap_err();
        assert_eq!(error.status(), Some(500));
        let error = parse_response(302, "").unwrap_err();
        assert_eq!(error.to_string(), "server returned HTTP 302");
    }

    #[test]
    fn progress_reader_counts_bytes() {
        let mut calls = Vec::new();
        let mut callback = |sent, total| calls.push((sent, total));
        let mut reader = ProgressReader {
            inner: &b"abcdefgh"[..],
            sent: 0,
            total: 8,
            progress: Some(&mut callback),
        };
        let mut buf = [0u8; 3];
        while reader.read(&mut buf).unwrap() > 0 {}
        assert_eq!(calls, [(3, 8), (6, 8), (8, 8)]);
    }
}
