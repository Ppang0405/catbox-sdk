//! Content-Type lookup by file extension.
//!
//! A fixed table rather than asking the operating system, so every platform sends the same
//! Content-Type for the same file. It matches the table in the Python implementation.

const std = @import("std");

pub const fallback = "application/octet-stream";

const types: std.StaticStringMap([]const u8) = .initComptime(.{
    .{ "7z", "application/x-7z-compressed" },
    .{ "aac", "audio/aac" },
    .{ "avif", "image/avif" },
    .{ "bmp", "image/bmp" },
    .{ "css", "text/css" },
    .{ "csv", "text/csv" },
    .{ "flac", "audio/flac" },
    .{ "gif", "image/gif" },
    .{ "gz", "application/gzip" },
    .{ "heic", "image/heic" },
    .{ "htm", "text/html" },
    .{ "html", "text/html" },
    .{ "ico", "image/vnd.microsoft.icon" },
    .{ "jpeg", "image/jpeg" },
    .{ "jpg", "image/jpeg" },
    .{ "js", "text/javascript" },
    .{ "json", "application/json" },
    .{ "m4a", "audio/mp4" },
    .{ "md", "text/markdown" },
    .{ "mkv", "video/x-matroska" },
    .{ "mov", "video/quicktime" },
    .{ "mp3", "audio/mpeg" },
    .{ "mp4", "video/mp4" },
    .{ "ogg", "audio/ogg" },
    .{ "opus", "audio/opus" },
    .{ "pdf", "application/pdf" },
    .{ "png", "image/png" },
    .{ "svg", "image/svg+xml" },
    .{ "tar", "application/x-tar" },
    .{ "tif", "image/tiff" },
    .{ "tiff", "image/tiff" },
    .{ "txt", "text/plain" },
    .{ "wav", "audio/wav" },
    .{ "webm", "video/webm" },
    .{ "webp", "image/webp" },
    .{ "xml", "application/xml" },
    .{ "zip", "application/zip" },
});

/// Returns the Content-Type for the extension of `filename` (a base name, not a path),
/// compared case-insensitively, or `application/octet-stream` when it is missing or unknown.
pub fn forFilename(filename: []const u8) []const u8 {
    const dot = std.mem.findScalarLast(u8, filename, '.') orelse return fallback;
    if (dot == 0) return fallback; // a dotfile such as ".txt" has no extension
    const extension = filename[dot + 1 ..];
    var lower: [8]u8 = undefined;
    if (extension.len > lower.len) return fallback;
    return types.get(std.ascii.lowerString(&lower, extension)) orelse fallback;
}

test forFilename {
    const expectEqualStrings = std.testing.expectEqualStrings;
    try expectEqualStrings("image/jpeg", forFilename("photo.jpg"));
    try expectEqualStrings("image/jpeg", forFilename("UPPER.JPG"));
    try expectEqualStrings("image/jpeg", forFilename("a.JpEg"));
    try expectEqualStrings("image/png", forFilename("archive.tar.png"));
    try expectEqualStrings("application/x-7z-compressed", forFilename("backup.7z"));
    try expectEqualStrings(fallback, forFilename(".txt"));
    try expectEqualStrings(fallback, forFilename("noextension"));
    try expectEqualStrings(fallback, forFilename("trailing."));
    try expectEqualStrings(fallback, forFilename("data.unknownext"));
    try expectEqualStrings(fallback, forFilename("data.averyveryverylongextension"));
}
