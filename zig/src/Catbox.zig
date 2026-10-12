//! Permanent uploads to https://catbox.moe (200 MB per file).
//!
//! Every upload returns the file's public URL, allocated with the given allocator and owned
//! by the caller. On failure, pass `Options.diagnostics` to learn the server's message.

const std = @import("std");
const Allocator = std.mem.Allocator;

const transport = @import("transport.zig");
const Options = transport.Options;

const Catbox = @This();

pub const default_endpoint = "https://catbox.moe/user/api.php";

/// Used for every request. It may be shared with other code and other clients.
http: *std.http.Client,
/// Associates uploads with a catbox account so they can be managed later.
/// Null or empty uploads anonymously.
user_hash: ?[]const u8 = null,
endpoint: []const u8 = default_endpoint,

/// Uploads a file from disk under its base name, streaming it rather than reading it
/// into memory.
pub fn uploadFile(c: Catbox, gpa: Allocator, path: []const u8, options: Options) ![]u8 {
    const file: transport.FilePart = .{
        .filename = std.fs.path.basename(path),
        .content = .{ .path = path },
    };
    return c.post(gpa, "fileupload", null, file, options);
}

/// Uploads in-memory bytes under the given filename.
pub fn uploadBytes(c: Catbox, gpa: Allocator, bytes: []const u8, filename: []const u8, options: Options) ![]u8 {
    const file: transport.FilePart = .{ .filename = filename, .content = .{ .bytes = bytes } };
    return c.post(gpa, "fileupload", null, file, options);
}

/// Asks catbox to fetch a remote URL and returns the URL of catbox's copy.
pub fn uploadUrl(c: Catbox, gpa: Allocator, url: []const u8, options: Options) ![]u8 {
    return c.post(gpa, "urlupload", .{ .name = "url", .value = url }, null, options);
}

fn post(
    c: Catbox,
    gpa: Allocator,
    reqtype: []const u8,
    extra_field: ?transport.Field,
    file: ?transport.FilePart,
    options: Options,
) ![]u8 {
    var buffer: [3]transport.Field = undefined;
    var fields: std.ArrayList(transport.Field) = .initBuffer(&buffer);
    fields.appendAssumeCapacity(.{ .name = "reqtype", .value = reqtype });
    if (c.user_hash) |hash| if (hash.len != 0) {
        fields.appendAssumeCapacity(.{ .name = "userhash", .value = hash });
    };
    if (extra_field) |field| fields.appendAssumeCapacity(field);
    return transport.post(gpa, c.http, c.endpoint, fields.items, file, options);
}
