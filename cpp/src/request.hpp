// A single multipart/form-data POST to a catbox endpoint. Internal to the library.
#pragma once

#include "curl.hpp"

#include <catbox/catbox.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace catbox::detail {

class Request {
public:
    explicit Request(std::string endpoint);

    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
    Request(Request&&) = delete;
    Request& operator=(Request&&) = delete;
    ~Request() = default;

    void add_field(std::string_view name, std::string_view value);

    /// Adds a file part that is streamed from disk while the request is sent. The part's
    /// filename is the path's base name and its Content-Type comes from the extension.
    /// Throws Error{Kind::file} if the path is missing, a directory, or unreadable.
    void add_file(std::string_view name, const std::filesystem::path& path);

    /// Adds a file part from memory. curl copies the bytes.
    void add_data(std::string_view name, std::string_view filename, std::span<const std::byte> data);

    /// Sends the request and returns the URL in the response body.
    std::string send(const ProgressCallback& progress = {});

private:
    curl_mimepart* add_part(std::string_view name);

    std::string endpoint_;
    // Declaration order is destruction order in reverse: the easy handle goes first, then the
    // mime tree it was using, then the file streams the mime tree read from.
    std::vector<std::unique_ptr<std::ifstream>> files_;
    MimeHandle mime_;
    EasyHandle easy_;
};

/// Turns an HTTP status and body into the uploaded file's URL, or throws Error.
std::string parse_response(long status, std::string_view body);

}  // namespace catbox::detail
