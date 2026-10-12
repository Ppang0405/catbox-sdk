//! The `catbox` command. See conformance/SPEC.md for the behavior shared by every implementation.

mod progress;

use std::fmt::Display;
use std::io::{self, IsTerminal, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use catbox::{CatboxClient, Error, Expiry, LitterboxClient, ProgressCallback};
use clap::builder::{PossibleValuesParser, TypedValueParser};
use clap::{Args, Parser, Subcommand};

use crate::progress::ProgressBar;

#[derive(Parser)]
#[command(
    name = "catbox",
    version,
    about = "Upload files to catbox.moe and litterbox.catbox.moe."
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Upload files to catbox.moe permanently (max 200 MB each)
    Upload {
        #[command(flatten)]
        account: Account,
        #[command(flatten)]
        options: Options,
        /// Files to upload
        #[arg(value_name = "FILE", required = true)]
        files: Vec<PathBuf>,
    },
    /// Have catbox.moe fetch and store files from remote URLs
    UploadUrl {
        #[command(flatten)]
        account: Account,
        #[command(flatten)]
        options: Options,
        /// URLs to copy to catbox
        #[arg(value_name = "URL", required = true)]
        urls: Vec<String>,
    },
    /// Upload files to litterbox.catbox.moe temporarily (max 1 GB each)
    Litter {
        /// How long to keep the files
        #[arg(short, long, default_value_t, value_parser = expiry_parser())]
        time: Expiry,
        #[command(flatten)]
        options: Options,
        /// Files to upload
        #[arg(value_name = "FILE", required = true)]
        files: Vec<PathBuf>,
    },
}

#[derive(Args)]
struct Account {
    /// Your catbox user hash, so uploads show up in your account [default: $CATBOX_USERHASH]
    #[arg(long, value_name = "HASH")]
    user_hash: Option<String>,
}

impl Account {
    fn client(self, endpoint: Option<String>) -> CatboxClient {
        // The environment variable is read by hand rather than with clap's `env` feature, which
        // would print the hash in `--help`.
        let user_hash = self
            .user_hash
            .or_else(|| std::env::var("CATBOX_USERHASH").ok())
            .unwrap_or_default();
        CatboxClient::new()
            .with_user_hash(user_hash)
            .with_endpoint(endpoint.unwrap_or_else(|| CatboxClient::DEFAULT_ENDPOINT.into()))
    }
}

#[derive(Args)]
struct Options {
    /// Don't show upload progress
    #[arg(short, long)]
    quiet: bool,

    /// Send requests to this URL instead of the real API (for testing)
    #[arg(long, hide = true, value_parser = parse_endpoint)]
    endpoint: Option<String>,
}

/// `1h`, `12h`, `24h` or `72h`, listed in `--help` and in the error for anything else.
fn expiry_parser() -> impl TypedValueParser<Value = Expiry> {
    PossibleValuesParser::new(Expiry::ALL.map(Expiry::as_str)).map(|value| {
        value
            .parse::<Expiry>()
            .expect("only possible values get here")
    })
}

fn parse_endpoint(value: &str) -> Result<String, String> {
    if value.starts_with("http://") || value.starts_with("https://") {
        Ok(value.to_owned())
    } else {
        Err("must be an http:// or https:// URL".to_owned())
    }
}

fn main() -> ExitCode {
    // Usage errors exit with status 2 here, before any request is sent.
    let ok = match Cli::parse().command {
        Command::Upload {
            account,
            options,
            files,
        } => {
            let client = account.client(options.endpoint);
            upload_each(&files, options.quiet, |path, progress| {
                client.upload_file(path, progress)
            })
        }
        Command::UploadUrl {
            account,
            options,
            urls,
        } => {
            let client = account.client(options.endpoint);
            let mut ok = true;
            for url in &urls {
                ok &= report(url, client.upload_url(url));
            }
            ok
        }
        Command::Litter {
            time,
            options,
            files,
        } => {
            let client = LitterboxClient::new().with_endpoint(
                options
                    .endpoint
                    .unwrap_or_else(|| LitterboxClient::DEFAULT_ENDPOINT.into()),
            );
            upload_each(&files, options.quiet, |path, progress| {
                client.upload_file(path, time, progress)
            })
        }
    };
    if ok {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}

/// Uploads each file in turn and reports it. Keeps going past failures; returns whether all
/// of them succeeded.
fn upload_each(
    files: &[PathBuf],
    quiet: bool,
    upload: impl Fn(&Path, Option<ProgressCallback<'_>>) -> catbox::Result<String>,
) -> bool {
    let show_progress = !quiet && io::stderr().is_terminal();
    let mut ok = true;
    for path in files {
        let result = if show_progress {
            let mut bar = ProgressBar::new(&label(path));
            let result = upload(path, Some(&mut |sent, total| bar.update(sent, total)));
            bar.finish();
            result
        } else {
            upload(path, None)
        };
        ok &= report(path.display(), result);
    }
    ok
}

/// Prints a URL on stdout, or `catbox: <argument>: <reason>` on stderr. Returns whether it
/// succeeded.
fn report(argument: impl Display, result: catbox::Result<String>) -> bool {
    let reason = match result {
        Ok(url) => match writeln!(io::stdout(), "{url}") {
            Ok(()) => return true,
            Err(error) => format!("uploaded to {url}, but could not print it: {error}"),
        },
        Err(error) => describe(&error),
    };
    // If stderr itself is gone there is nowhere left to complain; the exit status still says it.
    let _ = writeln!(io::stderr(), "catbox: {argument}: {reason}");
    false
}

/// Short, platform-independent wording for the common file errors.
fn describe(error: &Error) -> String {
    match error {
        Error::Io(error) if error.kind() == io::ErrorKind::NotFound => "no such file".into(),
        Error::Io(error) if error.kind() == io::ErrorKind::IsADirectory => "is a directory".into(),
        other => other.to_string(),
    }
}

fn label(path: &Path) -> String {
    path.file_name()
        .unwrap_or(path.as_os_str())
        .to_string_lossy()
        .into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;
    use clap::CommandFactory;

    #[test]
    fn command_definition_is_valid() {
        Cli::command().debug_assert();
    }

    #[test]
    fn litter_time_defaults_to_72h_and_rejects_others() {
        let cli = Cli::try_parse_from(["catbox", "litter", "a.txt"]).unwrap();
        assert!(matches!(
            cli.command,
            Command::Litter {
                time: Expiry::ThreeDays,
                ..
            }
        ));
        let cli = Cli::try_parse_from(["catbox", "litter", "-t", "1h", "a.txt"]).unwrap();
        assert!(matches!(
            cli.command,
            Command::Litter {
                time: Expiry::OneHour,
                ..
            }
        ));
        assert!(Cli::try_parse_from(["catbox", "litter", "-t", "5h", "a.txt"]).is_err());
    }

    #[test]
    fn files_are_required() {
        assert!(Cli::try_parse_from(["catbox", "upload"]).is_err());
        assert!(Cli::try_parse_from(["catbox", "upload-url"]).is_err());
        assert!(Cli::try_parse_from(["catbox", "litter"]).is_err());
    }

    #[test]
    fn endpoint_must_be_http() {
        assert!(Cli::try_parse_from(["catbox", "upload", "--endpoint", "ftp://x", "a"]).is_err());
        assert!(
            Cli::try_parse_from(["catbox", "upload", "--endpoint", "http://127.0.0.1:1/", "a"])
                .is_ok()
        );
    }

    #[test]
    fn file_errors_use_short_wording() {
        let missing = Error::Io(io::Error::from(io::ErrorKind::NotFound));
        assert_eq!(describe(&missing), "no such file");
        let directory = Error::Io(io::Error::from(io::ErrorKind::IsADirectory));
        assert_eq!(describe(&directory), "is a directory");
    }
}
