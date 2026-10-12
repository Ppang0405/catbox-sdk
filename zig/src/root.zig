//! Clients for catbox.moe (permanent uploads) and litterbox.catbox.moe (temporary uploads).
//!
//!     var http: std.http.Client = .{ .allocator = gpa, .io = io };
//!     defer http.deinit();
//!
//!     const client: catbox.Catbox = .{ .http = &http };
//!     const url = try client.uploadFile(gpa, "photo.jpg", .{});
//!     defer gpa.free(url);
//!
//! Uploads stream files from disk with an exact Content-Length. Failures return an error;
//! pass `Options.diagnostics` to get the server's message and HTTP status as well.

const std = @import("std");

pub const version = "0.1.0";
pub const user_agent = "catbox-zig/" ++ version;

pub const Catbox = @import("Catbox.zig");
pub const Litterbox = @import("Litterbox.zig");
pub const Expiry = Litterbox.Expiry;
pub const Diagnostics = @import("Diagnostics.zig");

const transport = @import("transport.zig");
pub const Options = transport.Options;
pub const Progress = transport.Progress;
pub const Error = transport.Error;
pub const parseResponse = transport.parseResponse;

/// The Content-Type sent for a file name: by extension, case-insensitively, with
/// `application/octet-stream` for anything unknown.
pub const mimeType = @import("mime.zig").forFilename;

test {
    std.testing.refAllDecls(@This());
    _ = @import("mime.zig");
    _ = @import("multipart.zig");
    _ = @import("transport.zig");
    _ = @import("tests.zig");
}
