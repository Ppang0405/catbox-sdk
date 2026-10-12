// stderr helpers for the CLI: TTY detection, error lines and a progress bar.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace terminal {

[[nodiscard]] bool stderr_is_interactive() noexcept;

/// Writes "catbox: <item>: <message>" on its own line to stderr.
void report_error(std::string_view item, std::string_view message);

/// "512 B", "1.5 KB", "24.0 MB", ...
[[nodiscard]] std::string format_bytes(std::uint64_t bytes);

/// A single-line progress bar on stderr, so stdout stays clean for piping URLs.
class ProgressBar {
public:
    explicit ProgressBar(std::string label);
    ProgressBar(const ProgressBar&) = delete;
    ProgressBar& operator=(const ProgressBar&) = delete;
    ~ProgressBar();

    void update(std::uint64_t sent, std::uint64_t total);

private:
    /// Clears the line again, so whatever is printed next starts in column 0. Runs on
    /// destruction, so the bar is erased on success and on failure alike.
    void finish() noexcept;

    std::string label_;
    int last_permille_ = -1;
    std::size_t line_length_ = 0;
};

}  // namespace terminal
