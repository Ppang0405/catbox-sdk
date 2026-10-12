// The public API: Error, CatboxClient and LitterboxClient, built on detail::Request.
#include "request.hpp"

#include <catbox/catbox.hpp>

#include <utility>

namespace catbox {

Error::Error(Kind kind, const std::string& message, std::optional<int> status, std::string server_message)
    : std::runtime_error(message), kind_(kind), status_(status), server_message_(std::move(server_message)) {}

// catbox.moe

namespace {

void add_catbox_fields(detail::Request& request, const UserHash& user_hash, std::string_view reqtype) {
    request.add_field("reqtype", reqtype);
    if (!user_hash.empty()) {
        request.add_field("userhash", user_hash.value());
    }
}

}  // namespace

CatboxClient::CatboxClient(UserHash user_hash, Endpoint endpoint)
    : user_hash_(std::move(user_hash)), endpoint_(std::move(endpoint)) {}

std::string CatboxClient::upload_file(const std::filesystem::path& file, const ProgressCallback& progress) const {
    detail::Request request(endpoint_.url());
    add_catbox_fields(request, user_hash_, "fileupload");
    request.add_file("fileToUpload", file);
    return request.send(progress);
}

std::string CatboxClient::upload_data(std::span<const std::byte> data, std::string_view filename,
                                      const ProgressCallback& progress) const {
    detail::Request request(endpoint_.url());
    add_catbox_fields(request, user_hash_, "fileupload");
    request.add_data("fileToUpload", filename, data);
    return request.send(progress);
}

std::string CatboxClient::upload_url(std::string_view url) const {
    detail::Request request(endpoint_.url());
    add_catbox_fields(request, user_hash_, "urlupload");
    request.add_field("url", url);
    return request.send();
}

// litterbox.catbox.moe

std::string_view to_string(Expiry expiry) noexcept {
    switch (expiry) {
        case Expiry::one_hour: return "1h";
        case Expiry::twelve_hours: return "12h";
        case Expiry::one_day: return "24h";
        case Expiry::three_days: return "72h";
    }
    return "72h";  // unreachable for valid enumerators; keeps every compiler's return-path check quiet
}

std::optional<Expiry> parse_expiry(std::string_view text) noexcept {
    for (const Expiry expiry : {Expiry::one_hour, Expiry::twelve_hours, Expiry::one_day, Expiry::three_days}) {
        if (to_string(expiry) == text) {
            return expiry;
        }
    }
    return std::nullopt;
}

LitterboxClient::LitterboxClient(Endpoint endpoint) : endpoint_(std::move(endpoint)) {}

std::string LitterboxClient::upload_file(const std::filesystem::path& file, Expiry expiry,
                                         const ProgressCallback& progress) const {
    detail::Request request(endpoint_.url());
    request.add_field("reqtype", "fileupload");
    request.add_field("time", to_string(expiry));
    request.add_file("fileToUpload", file);
    return request.send(progress);
}

std::string LitterboxClient::upload_data(std::span<const std::byte> data, std::string_view filename, Expiry expiry,
                                         const ProgressCallback& progress) const {
    detail::Request request(endpoint_.url());
    request.add_field("reqtype", "fileupload");
    request.add_field("time", to_string(expiry));
    request.add_data("fileToUpload", filename, data);
    return request.send(progress);
}

}  // namespace catbox
