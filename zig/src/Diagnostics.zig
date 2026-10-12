//! What went wrong with a failed upload, beyond the error code.
//!
//! Zig errors are plain values that cannot carry a payload, but catbox's error messages are
//! the useful part ("No files given.", "HTTP 500: ..."). Pass a `*Diagnostics` in the upload
//! options and, when the call returns an error, it holds the server's status and response
//! text. `format` turns it into a one-line message:
//!
//!     var diagnostics: catbox.Diagnostics = .{};
//!     const url = client.uploadFile(gpa, path, .{ .diagnostics = &diagnostics }) catch {
//!         std.log.err("{s}: {f}", .{ path, diagnostics });
//!         return;
//!     };
//!
//! It owns no memory, so there is nothing to free, and it can be reused across calls.

const std = @import("std");
const Writer = std.Io.Writer;

const Diagnostics = @This();

/// The error the upload returned.
err: ?anyerror = null,
/// The HTTP status, when the server answered.
status: ?std.http.Status = null,
/// When `err` is `error.WriteFailed` or `error.ReadFailed`, the network or TLS error behind it.
cause: ?anyerror = null,
body_buffer: [max_body_len]u8 = undefined,
body_len: usize = 0,

/// Longer response bodies are truncated. Catbox's messages are a few dozen bytes.
pub const max_body_len = 1024;

/// The server's response text, trimmed of surrounding whitespace.
pub fn body(d: *const Diagnostics) []const u8 {
    return d.body_buffer[0..d.body_len];
}

pub fn setBody(d: *Diagnostics, text: []const u8) void {
    d.body_len = @min(text.len, max_body_len);
    @memcpy(d.body_buffer[0..d.body_len], text[0..d.body_len]);
}

/// Writes a one-line description such as `server error: No files given.` or
/// `server returned HTTP 500: Internal Server Error` or `no such file`.
pub fn format(d: *const Diagnostics, w: *Writer) Writer.Error!void {
    const err = d.err orelse return w.writeAll("no error");
    switch (err) {
        error.HttpStatus => {
            try w.print("server returned HTTP {d}", .{@backingInt(d.status.?)});
            if (d.body_len != 0) {
                try w.writeAll(": ");
                try writeOneLine(w, d.body());
            }
        },
        error.UnexpectedResponse => if (d.body_len != 0) {
            try w.writeAll("server error: ");
            try writeOneLine(w, d.body());
        } else try w.writeAll("server returned an empty response"),
        error.WriteFailed, error.ReadFailed => if (d.cause) |cause|
            try writeErrorName(w, cause)
        else
            try writeErrorName(w, err),
        else => try writeErrorName(w, err),
    }
}

/// Writes `text` with each run of line breaks (an HTML error page, say) turned into a space.
fn writeOneLine(w: *Writer, text: []const u8) Writer.Error!void {
    var lines = std.mem.tokenizeAny(u8, text, "\r\n");
    var first = true;
    while (lines.next()) |line| : (first = false) {
        if (!first) try w.writeByte(' ');
        try w.writeAll(line);
    }
}

/// Describes common errors in words, and falls back to the error's name split into
/// lowercase words, so `error.ConnectionResetByPeer` reads "connection reset by peer".
fn writeErrorName(w: *Writer, err: anyerror) Writer.Error!void {
    const phrase: ?[]const u8 = switch (err) {
        error.FileNotFound => "no such file",
        error.IsDir => "is a directory",
        error.AccessDenied, error.PermissionDenied => "permission denied",
        error.UnknownHostName => "unknown host name",
        error.InvalidEndpoint => "invalid endpoint URL",
        error.NotAFile => "not a regular file",
        error.FileChanged => "file changed size while it was being uploaded",
        error.TlsInitializationFailed => "TLS handshake failed",
        error.CertificateBundleLoadFailure => "could not load the system's TLS certificates",
        else => null,
    };
    if (phrase) |text| return w.writeAll(text);

    for (@errorName(err), 0..) |c, i| {
        if (std.ascii.isUpper(c)) {
            if (i != 0) try w.writeByte(' ');
            try w.writeByte(std.ascii.toLower(c));
        } else {
            try w.writeByte(c);
        }
    }
}

const testing = std.testing;

fn expectMessage(expected: []const u8, d: Diagnostics) !void {
    var buffer: [256]u8 = undefined;
    var w: Writer = .fixed(&buffer);
    try d.format(&w);
    try testing.expectEqualStrings(expected, w.buffered());
}

test format {
    var d: Diagnostics = .{ .err = error.HttpStatus, .status = .internal_server_error };
    try expectMessage("server returned HTTP 500", d);
    d.setBody("Internal Server Error");
    try expectMessage("server returned HTTP 500: Internal Server Error", d);

    d.setBody("<html>\r\n<body>oops</body>\n</html>");
    try expectMessage("server returned HTTP 500: <html> <body>oops</body> </html>", d);

    d = .{ .err = error.UnexpectedResponse };
    try expectMessage("server returned an empty response", d);
    d.setBody("No files given.");
    try expectMessage("server error: No files given.", d);

    try expectMessage("no such file", .{ .err = error.FileNotFound });
    try expectMessage("is a directory", .{ .err = error.IsDir });
    try expectMessage("connection refused", .{ .err = error.ConnectionRefused });
    try expectMessage("connection reset by peer", .{ .err = error.WriteFailed, .cause = error.ConnectionResetByPeer });
    try expectMessage("write failed", .{ .err = error.WriteFailed });
}

test "long bodies are truncated" {
    var d: Diagnostics = .{};
    d.setBody(&(@as([max_body_len + 10]u8, @splat('x'))));
    try testing.expectEqual(max_body_len, d.body().len);
}
