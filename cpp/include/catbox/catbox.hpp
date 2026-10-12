// catbox: clients for https://catbox.moe and https://litterbox.catbox.moe.
//
// Every upload method blocks until the server answers, returns the public URL of the stored
// file, and throws catbox::Error when the upload fails. Clients are cheap value types that can
// be copied freely and used from several threads at once.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace catbox {

inline constexpr std::string_view version = "0.1.0";

inline constexpr std::string_view catbox_endpoint = "https://catbox.moe/user/api.php";
inline constexpr std::string_view litterbox_endpoint = "https://litterbox.catbox.moe/resources/internals/api.php";

/// Called while a request body is sent, with the bytes sent so far and the total body size.
/// It runs on the calling thread. An exception thrown from it aborts the upload and is
/// rethrown from the upload method.
using ProgressCallback = std::function<void(std::uint64_t sent, std::uint64_t total)>;

/// Raised when an upload fails: the file can't be read, the server can't be reached, or the
/// server rejects the upload.
class Error : public std::runtime_error {
public:
    enum class Kind {
        file,                 ///< the local file is missing, a directory, or unreadable
        network,              ///< no HTTP response (DNS, connection, TLS, timeout, ...)
        http_status,          ///< the server answered with a non-2xx status
        unexpected_response,  ///< 2xx, but the body is not a URL (catbox reports most errors this way)
    };

    Error(Kind kind, const std::string& message, std::optional<int> status = std::nullopt,
          std::string server_message = {});

    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    /// The HTTP status code, when the server answered at all.
    [[nodiscard]] std::optional<int> status() const noexcept { return status_; }
    /// The trimmed response body, e.g. "No files given.". Empty when there was no response.
    [[nodiscard]] const std::string& server_message() const noexcept { return server_message_; }

private:
    Kind kind_;
    std::optional<int> status_;
    std::string server_message_;
};

/// Guesses a Content-Type from a file name's extension, case-insensitively, falling back to
/// "application/octet-stream". A fixed table, so the answer is the same on every platform.
[[nodiscard]] std::string_view mime_type(std::string_view filename) noexcept;

/// A catbox account's user hash. Uploads made with it show up in that account, so they can be
/// managed later. Empty means anonymous. A distinct type, so it can't be swapped with an Endpoint.
class UserHash {
public:
    UserHash() = default;
    explicit UserHash(std::string_view value) : value_(value) {}
    [[nodiscard]] const std::string& value() const noexcept { return value_; }
    [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

private:
    std::string value_;
};

/// The API URL a client posts to. Override it to test against a local server.
class Endpoint {
public:
    explicit Endpoint(std::string_view url) : url_(url) {}
    [[nodiscard]] const std::string& url() const noexcept { return url_; }

private:
    std::string url_;
};

/// Permanent uploads to https://catbox.moe (200 MB per file).
class CatboxClient {
public:
    explicit CatboxClient(UserHash user_hash = {}, Endpoint endpoint = Endpoint(catbox_endpoint));
    explicit CatboxClient(Endpoint endpoint) : CatboxClient(UserHash(), std::move(endpoint)) {}

    /// Uploads a file from disk, streaming it, under its base name.
    [[nodiscard]] std::string upload_file(const std::filesystem::path& file,
                                          const ProgressCallback& progress = {}) const;

    /// Uploads in-memory bytes under the given filename.
    [[nodiscard]] std::string upload_data(std::span<const std::byte> data, std::string_view filename,
                                          const ProgressCallback& progress = {}) const;

    /// Asks catbox to fetch a remote URL and returns the catbox URL of the copy.
    [[nodiscard]] std::string upload_url(std::string_view url) const;

    [[nodiscard]] const UserHash& user_hash() const noexcept { return user_hash_; }
    [[nodiscard]] const Endpoint& endpoint() const noexcept { return endpoint_; }

private:
    UserHash user_hash_;
    Endpoint endpoint_;
};

/// How long litterbox keeps a file before deleting it.
enum class Expiry { one_hour, twelve_hours, one_day, three_days };

/// "1h", "12h", "24h" or "72h", as the litterbox API expects.
[[nodiscard]] std::string_view to_string(Expiry expiry) noexcept;
/// The inverse of to_string; std::nullopt for anything else.
[[nodiscard]] std::optional<Expiry> parse_expiry(std::string_view text) noexcept;

/// Temporary uploads to https://litterbox.catbox.moe (1 GB per file).
class LitterboxClient {
public:
    explicit LitterboxClient(Endpoint endpoint = Endpoint(litterbox_endpoint));

    /// Uploads a file from disk, streaming it, under its base name.
    [[nodiscard]] std::string upload_file(const std::filesystem::path& file, Expiry expiry = Expiry::three_days,
                                          const ProgressCallback& progress = {}) const;

    /// Uploads in-memory bytes under the given filename.
    [[nodiscard]] std::string upload_data(std::span<const std::byte> data, std::string_view filename,
                                          Expiry expiry = Expiry::three_days,
                                          const ProgressCallback& progress = {}) const;

    [[nodiscard]] const Endpoint& endpoint() const noexcept { return endpoint_; }

private:
    Endpoint endpoint_;
};

}  // namespace catbox
