//! Temporary uploads to https://litterbox.catbox.moe (1 GB per file), deleted after the
//! chosen `Expiry`.
//!
//! Every upload returns the file's temporary URL, allocated with the given allocator and
//! owned by the caller. On failure, pass `Options.diagnostics` to learn the server's message.

const std = @import("std");
const Allocator = std.mem.Allocator;

const transport = @import("transport.zig");
const Options = transport.Options;

const Litterbox = @This();

pub const default_endpoint = "https://litterbox.catbox.moe/resources/internals/api.php";

/// How long litterbox keeps a file.
pub const Expiry = enum {
    one_hour,
    twelve_hours,
    one_day,
    three_days,

    pub const default: Expiry = .three_days;

    /// The value litterbox expects in the `time` field.
    pub fn string(e: Expiry) []const u8 {
        return switch (e) {
            .one_hour => "1h",
            .twelve_hours => "12h",
            .one_day => "24h",
            .three_days => "72h",
        };
    }

    /// Parses "1h", "12h", "24h" or "72h".
    pub fn parse(text: []const u8) ?Expiry {
        inline for (@typeInfo(Expiry).@"enum".field_names) |name| {
            const e = @field(Expiry, name);
            if (std.mem.eql(u8, text, e.string())) return e;
        }
        return null;
    }
};

/// Used for every request. It may be shared with other code and other clients.
http: *std.http.Client,
endpoint: []const u8 = default_endpoint,

/// Uploads a file from disk under its base name, streaming it rather than reading it
/// into memory.
pub fn uploadFile(l: Litterbox, gpa: Allocator, path: []const u8, expiry: Expiry, options: Options) ![]u8 {
    const file: transport.FilePart = .{
        .filename = std.fs.path.basename(path),
        .content = .{ .path = path },
    };
    return l.post(gpa, expiry, file, options);
}

/// Uploads in-memory bytes under the given filename.
pub fn uploadBytes(
    l: Litterbox,
    gpa: Allocator,
    bytes: []const u8,
    filename: []const u8,
    expiry: Expiry,
    options: Options,
) ![]u8 {
    return l.post(gpa, expiry, .{ .filename = filename, .content = .{ .bytes = bytes } }, options);
}

fn post(l: Litterbox, gpa: Allocator, expiry: Expiry, file: transport.FilePart, options: Options) ![]u8 {
    const fields: []const transport.Field = &.{
        .{ .name = "reqtype", .value = "fileupload" },
        .{ .name = "time", .value = expiry.string() },
    };
    return transport.post(gpa, l.http, l.endpoint, fields, file, options);
}

test Expiry {
    try std.testing.expectEqual(Expiry.one_hour, Expiry.parse("1h").?);
    try std.testing.expectEqual(Expiry.twelve_hours, Expiry.parse("12h").?);
    try std.testing.expectEqual(Expiry.one_day, Expiry.parse("24h").?);
    try std.testing.expectEqual(Expiry.three_days, Expiry.parse("72h").?);
    try std.testing.expectEqual(null, Expiry.parse("5h"));
    try std.testing.expectEqual(null, Expiry.parse("1H"));
    try std.testing.expectEqualStrings("72h", Expiry.default.string());
}
