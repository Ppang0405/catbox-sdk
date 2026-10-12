#include "terminal.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <format>
#include <iostream>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace terminal {

bool stderr_is_interactive() noexcept {
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

void report_error(std::string_view item, std::string_view message) {
    std::cerr << "catbox: " << item << ": " << message << '\n' << std::flush;
}

std::string format_bytes(std::uint64_t bytes) {
    constexpr std::array units{"B", "KB", "MB", "GB"};
    auto value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024 && unit + 1 < units.size()) {
        value /= 1024;
        ++unit;
    }
    return unit == 0 ? std::format("{} B", bytes) : std::format("{:.1f} {}", value, units[unit]);
}

ProgressBar::ProgressBar(std::string label) : label_(std::move(label)) {}

ProgressBar::~ProgressBar() { finish(); }

void ProgressBar::update(std::uint64_t sent, std::uint64_t total) {
    if (total == 0) {
        return;
    }
    const auto permille = static_cast<int>(std::min(sent, total) * 1000 / total);
    // Redraw at most once per 0.1% so fast uploads don't flood the terminal.
    if (permille == last_permille_) {
        return;
    }
    last_permille_ = permille;

    constexpr int width = 24;
    const int filled = permille * width / 1000;
    std::string bar(static_cast<std::size_t>(filled), '=');
    if (filled < width) {
        bar += '>';
    }
    bar.resize(width, ' ');

    const std::string line = std::format("{} [{}] {:5.1f}% {}/{}", label_, bar, permille / 10.0,
                                         format_bytes(sent), format_bytes(total));
    // Pad over the previous line instead of using ANSI erase codes, which older Windows consoles lack.
    const std::size_t padding = line_length_ > line.size() ? line_length_ - line.size() : 0;
    std::cerr << '\r' << line << std::string(padding, ' ') << std::flush;
    line_length_ = line.size();
}

void ProgressBar::finish() noexcept {
    if (line_length_ == 0) {
        return;
    }
    try {
        std::cerr << '\r' << std::string(line_length_, ' ') << '\r' << std::flush;
    } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor must not throw over a cosmetic redraw
    }
    line_length_ = 0;
}

}  // namespace terminal
