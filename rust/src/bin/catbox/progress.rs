use std::io::{self, Stderr, Write};

/// A single-line progress bar on stderr, so stdout stays clean for piping URLs.
///
/// It redraws by returning to the start of the line with `\r` and padding over the previous
/// text with spaces, instead of ANSI erase codes, which older Windows consoles don't understand.
pub struct ProgressBar<W: Write = Stderr> {
    label: String,
    out: W,
    last_rendered: Option<u64>,
    line_length: usize,
}

const WIDTH: u64 = 24;

impl ProgressBar {
    pub fn new(label: &str) -> Self {
        Self::with_writer(label, io::stderr())
    }
}

impl<W: Write> ProgressBar<W> {
    pub fn with_writer(label: &str, out: W) -> Self {
        ProgressBar {
            label: label.to_owned(),
            out,
            last_rendered: None,
            line_length: 0,
        }
    }

    pub fn update(&mut self, sent: u64, total: u64) {
        if total == 0 {
            return;
        }
        let permille = sent.min(total) * 1000 / total;
        // Redraw at most once per 0.1% so fast uploads don't flood the terminal.
        if self.last_rendered == Some(permille) {
            return;
        }
        self.last_rendered = Some(permille);

        let filled = (permille * WIDTH / 1000) as usize;
        let mut bar = "=".repeat(filled);
        if filled < WIDTH as usize {
            bar.push('>');
        }
        let line = format!(
            "{} [{bar:<width$}] {:5.1}% {}/{}",
            self.label,
            permille as f64 / 10.0,
            format_bytes(sent),
            format_bytes(total),
            width = WIDTH as usize,
        );
        let length = line.chars().count();
        let padding = self.line_length.saturating_sub(length);
        // A progress bar that can't be drawn is not worth failing the upload over.
        let _ = write!(self.out, "\r{line}{:padding$}", "");
        let _ = self.out.flush();
        self.line_length = length;
    }

    /// Erases the bar so the next message starts on a clean line.
    pub fn finish(&mut self) {
        if self.line_length > 0 {
            let _ = write!(self.out, "\r{:width$}\r", "", width = self.line_length);
            let _ = self.out.flush();
            self.line_length = 0;
        }
    }
}

fn format_bytes(count: u64) -> String {
    const UNITS: [&str; 4] = ["B", "KB", "MB", "GB"];
    let mut value = count as f64;
    let mut unit = 0;
    while value >= 1024.0 && unit < UNITS.len() - 1 {
        value /= 1024.0;
        unit += 1;
    }
    if unit == 0 {
        format!("{count} B")
    } else {
        format!("{value:.1} {}", UNITS[unit])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn output(bar: &ProgressBar<Vec<u8>>) -> &str {
        std::str::from_utf8(&bar.out).unwrap()
    }

    #[test]
    fn draws_label_bar_percentage_and_sizes() {
        let mut bar = ProgressBar::with_writer("a.png", Vec::new());
        bar.update(512 * 1024, 1024 * 1024);
        assert_eq!(
            output(&bar),
            "\ra.png [============>           ]  50.0% 512.0 KB/1.0 MB"
        );
    }

    #[test]
    fn full_bar_has_no_arrow() {
        let mut bar = ProgressBar::with_writer("f", Vec::new());
        bar.update(10, 10);
        assert_eq!(
            output(&bar),
            "\rf [========================] 100.0% 10 B/10 B"
        );
    }

    #[test]
    fn skips_redraws_within_the_same_permille() {
        let mut bar = ProgressBar::with_writer("f", Vec::new());
        bar.update(1, 1_000_000);
        bar.update(2, 1_000_000);
        assert_eq!(output(&bar).matches('\r').count(), 1);
        bar.update(1, 0);
        assert_eq!(output(&bar).matches('\r').count(), 1);
    }

    #[test]
    fn pads_over_a_longer_previous_line_and_erases_on_finish() {
        let mut bar = ProgressBar::with_writer("f", Vec::new());
        let total = 2 * 1024 * 1024;
        bar.update(1023 * 1024 + 1000, total); // "... 1024.0 KB/2.0 MB"
        let first = output(&bar).len() - 1;
        bar.update(total, total); // "... 2.0 MB/2.0 MB", 3 characters shorter
        let second = output(&bar)[first + 1..]
            .trim_start_matches('\r')
            .to_owned();
        assert_eq!(
            second.len(),
            first,
            "shorter line is padded to cover the old one"
        );
        assert!(second.ends_with(' '));

        // The padding is already blank, so erasing the visible text is enough.
        bar.finish();
        assert!(output(&bar).ends_with(&format!("\r{}\r", " ".repeat(first - 3))));
    }

    #[test]
    fn finish_without_updates_writes_nothing() {
        let mut bar = ProgressBar::with_writer("f", Vec::new());
        bar.finish();
        assert!(bar.out.is_empty());
    }

    #[test]
    fn byte_sizes() {
        assert_eq!(format_bytes(0), "0 B");
        assert_eq!(format_bytes(1023), "1023 B");
        assert_eq!(format_bytes(1536), "1.5 KB");
        assert_eq!(format_bytes(24 * 1024 * 1024 + 7), "24.0 MB");
        assert_eq!(format_bytes(3 * 1024 * 1024 * 1024), "3.0 GB");
    }
}
