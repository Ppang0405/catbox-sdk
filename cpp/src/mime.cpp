#include <catbox/catbox.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace catbox {
namespace {

using Entry = std::pair<std::string_view, std::string_view>;

// Sorted by extension for binary search; the static_assert below keeps it that way.
// Same table as the Python package, so every implementation sends the same Content-Type.
constexpr std::array mime_types{
    Entry{"7z", "application/x-7z-compressed"},
    Entry{"aac", "audio/aac"},
    Entry{"avif", "image/avif"},
    Entry{"bmp", "image/bmp"},
    Entry{"css", "text/css"},
    Entry{"csv", "text/csv"},
    Entry{"flac", "audio/flac"},
    Entry{"gif", "image/gif"},
    Entry{"gz", "application/gzip"},
    Entry{"heic", "image/heic"},
    Entry{"htm", "text/html"},
    Entry{"html", "text/html"},
    Entry{"ico", "image/vnd.microsoft.icon"},
    Entry{"jpeg", "image/jpeg"},
    Entry{"jpg", "image/jpeg"},
    Entry{"js", "text/javascript"},
    Entry{"json", "application/json"},
    Entry{"m4a", "audio/mp4"},
    Entry{"md", "text/markdown"},
    Entry{"mkv", "video/x-matroska"},
    Entry{"mov", "video/quicktime"},
    Entry{"mp3", "audio/mpeg"},
    Entry{"mp4", "video/mp4"},
    Entry{"ogg", "audio/ogg"},
    Entry{"opus", "audio/opus"},
    Entry{"pdf", "application/pdf"},
    Entry{"png", "image/png"},
    Entry{"svg", "image/svg+xml"},
    Entry{"tar", "application/x-tar"},
    Entry{"tif", "image/tiff"},
    Entry{"tiff", "image/tiff"},
    Entry{"txt", "text/plain"},
    Entry{"wav", "audio/wav"},
    Entry{"webm", "video/webm"},
    Entry{"webp", "image/webp"},
    Entry{"xml", "application/xml"},
    Entry{"zip", "application/zip"},
};
static_assert(std::ranges::is_sorted(mime_types, {}, &Entry::first), "keep mime_types sorted by extension");

constexpr std::string_view fallback = "application/octet-stream";

constexpr char ascii_lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// The table's keys are lowercase, so comparing lowered characters finds any capitalization
// without copying the extension.
constexpr bool less_ignoring_case(std::string_view a, std::string_view b) noexcept {
    return std::ranges::lexicographical_compare(a, b, {}, ascii_lower, ascii_lower);
}

}  // namespace

std::string_view mime_type(std::string_view filename) noexcept {
    // Like Python's os.path.splitext: the extension follows the last dot of the base name,
    // and leading dots (".bashrc") don't start one.
    if (const auto slash = filename.find_last_of("/\\"); slash != std::string_view::npos) {
        filename.remove_prefix(slash + 1);
    }
    const auto first = filename.find_first_not_of('.');
    const auto dot = filename.rfind('.');
    if (first == std::string_view::npos || dot == std::string_view::npos || dot < first) {
        return fallback;
    }
    const std::string_view extension = filename.substr(dot + 1);
    const auto entry = std::ranges::lower_bound(mime_types, extension, less_ignoring_case, &Entry::first);
    if (entry == mime_types.end() || less_ignoring_case(extension, entry->first)) {
        return fallback;
    }
    return entry->second;
}

}  // namespace catbox
