//! Sends a multipart form with `std.http.Client` and interprets catbox's answer.

const std = @import("std");
const Io = std.Io;
const Allocator = std.mem.Allocator;
const http = std.http;

const Diagnostics = @import("Diagnostics.zig");
const multipart = @import("multipart.zig");
const root = @import("root.zig");

/// Receives upload progress: bytes of the request body sent so far, and its total size.
///
/// Zig has no closures, so this pairs a context pointer with a function. `init` builds one
/// from any pointer and a function taking that pointer:
///
///     var bar: ProgressBar = ...;
///     .progress = .init(&bar, ProgressBar.update),
pub const Progress = struct {
    context: *anyopaque,
    callback: *const fn (context: *anyopaque, sent: u64, total: u64) void,

    pub fn init(
        context: anytype,
        comptime callback: fn (@TypeOf(context), sent: u64, total: u64) void,
    ) Progress {
        const Context = @TypeOf(context);
        const erased = struct {
            fn call(pointer: *anyopaque, sent: u64, total: u64) void {
                callback(@ptrCast(@alignCast(pointer)), sent, total);
            }
        };
        comptime std.debug.assert(@typeInfo(Context) == .pointer);
        return .{ .context = @ptrCast(@constCast(context)), .callback = erased.call };
    }

    pub fn report(p: Progress, sent: u64, total: u64) void {
        p.callback(p.context, sent, total);
    }
};

pub const Options = struct {
    /// Called as the request body is sent.
    progress: ?Progress = null,
    /// Filled in when the upload fails. See `Diagnostics`.
    diagnostics: ?*Diagnostics = null,
};

pub const Field = multipart.Field;

/// Where the file part's bytes come from.
pub const Content = union(enum) {
    /// Streamed from disk, never loaded into memory as a whole.
    path: []const u8,
    bytes: []const u8,
};

pub const FilePart = struct {
    filename: []const u8,
    content: Content,
};

/// Errors specific to catbox. The upload functions can also return file system errors
/// (`error.FileNotFound`, `error.IsDir`, ...) and network errors from `std.http.Client`.
pub const Error = error{
    /// The server answered with a status outside 200-299. `Diagnostics` has the status and body.
    HttpStatus,
    /// The server answered 2xx but not with a URL; catbox reports most errors this way.
    /// `Diagnostics` has the body.
    UnexpectedResponse,
    /// The endpoint is not an http:// or https:// URL.
    InvalidEndpoint,
    /// The path names something other than a regular file or directory, such as a pipe.
    NotAFile,
    /// The file got shorter between measuring it and sending it.
    FileChanged,
};

/// Posts `fields` plus an optional file to `endpoint` and returns the URL catbox answers with,
/// allocated with `gpa`.
pub fn post(
    gpa: Allocator,
    client: *http.Client,
    endpoint: []const u8,
    fields: []const Field,
    file: ?FilePart,
    options: Options,
) ![]u8 {
    if (options.diagnostics) |d| d.* = .{};
    return send(gpa, client, endpoint, fields, file, options) catch |err| {
        if (options.diagnostics) |d| d.err = err;
        return err;
    };
}

/// Bytes read from the response. Anything longer is not a catbox URL; the rest is discarded.
const max_response_len = 16 * 1024;

/// The size of the buffer the request body is staged in before going to the socket.
const body_buffer_len = 64 * 1024;

fn send(
    gpa: Allocator,
    client: *http.Client,
    endpoint: []const u8,
    fields: []const Field,
    file_part: ?FilePart,
    options: Options,
) ![]u8 {
    const io = client.io;
    const uri = std.Uri.parse(endpoint) catch return error.InvalidEndpoint;
    if (http.Client.Protocol.fromUri(uri) == null) return error.InvalidEndpoint;

    // Open and measure the file first, so a missing file or a directory never sends a request.
    const payload: Payload = if (file_part) |part| try .open(io, part.content) else .none;
    defer payload.close(io);

    const form: multipart.Form = .{
        .boundary = multipart.randomBoundary(io),
        .fields = fields,
        .file = if (file_part) |part| .{
            .name = "fileToUpload",
            .filename = part.filename,
            .content_type = root.mimeType(part.filename),
        } else null,
    };
    var content_type: multipart.Form.ContentTypeBuffer = undefined;

    var request = try client.request(.POST, uri, .{
        // A file body can't be replayed, so a redirect is reported rather than followed.
        .redirect_behavior = .unhandled,
        // A fresh connection per upload: a pooled one that the server has meanwhile closed
        // would fail after the body was sent, with no way to send it again.
        .keep_alive = false,
        .headers = .{
            .user_agent = .{ .override = root.user_agent },
            .content_type = .{ .override = form.contentType(&content_type) },
            .accept_encoding = .omit,
        },
    });
    defer request.deinit();
    // Without an Accept-Encoding header, a compressed answer would be a protocol error.
    request.accept_encoding = @splat(false);
    request.accept_encoding[@backingInt(http.ContentEncoding.identity)] = true;
    // An exact Content-Length, known before the first byte is sent. Catbox does not accept
    // chunked request bodies.
    request.transfer_encoding = .{ .content_length = form.headLength() + payload.len() + form.tailLength() };

    const diagnostics = options.diagnostics;
    var body_buffer: [body_buffer_len]u8 = undefined;
    sendBody(&request, &body_buffer, &form, payload, options.progress) catch |err| switch (err) {
        error.WriteFailed => {
            if (diagnostics) |d| d.cause = request.connection.?.stream_writer.err;
            // A server may answer early (413 Payload Too Large, say) and close the connection
            // while the body is still being sent. Report its answer if there is one.
            var response = request.receiveHead(&.{}) catch return error.WriteFailed;
            return readResponse(gpa, &response, diagnostics);
        },
        else => |e| return e,
    };

    var response = request.receiveHead(&.{}) catch |err| {
        if (err == error.ReadFailed) {
            if (diagnostics) |d| d.cause = request.connection.?.getReadError();
        }
        return err;
    };
    return readResponse(gpa, &response, diagnostics);
}

/// The file part's contents, ready to send.
const Payload = union(enum) {
    none,
    bytes: []const u8,
    file: struct { handle: Io.File, size: u64 },

    fn open(io: Io, content: Content) !Payload {
        switch (content) {
            .bytes => |bytes| return .{ .bytes = bytes },
            .path => |path| {
                const file = try Io.Dir.cwd().openFile(io, path, .{ .allow_directory = false });
                errdefer file.close(io);
                const stat = try file.stat(io);
                // Only a regular file has a size known up front (not a pipe or a device).
                if (stat.kind != .file) return error.NotAFile;
                return .{ .file = .{ .handle = file, .size = stat.size } };
            },
        }
    }

    fn close(p: Payload, io: Io) void {
        if (p == .file) p.file.handle.close(io);
    }

    fn len(p: Payload) u64 {
        return switch (p) {
            .none => 0,
            .bytes => |bytes| bytes.len,
            .file => |file| file.size,
        };
    }
};

fn sendBody(
    request: *http.Client.Request,
    buffer: []u8,
    form: *const multipart.Form,
    payload: Payload,
    progress: ?Progress,
) !void {
    const total = request.transfer_encoding.content_length;
    var body = try request.sendBodyUnflushed(buffer);
    const w = &body.writer;

    try form.writeHead(w);
    var sent = form.headLength();
    if (progress) |p| p.report(sent, total);

    switch (payload) {
        .none => {},
        .bytes => |bytes| {
            var rest = bytes;
            while (rest.len > 0) {
                const n = @min(rest.len, buffer.len);
                try w.writeAll(rest[0..n]);
                rest = rest[n..];
                sent += n;
                if (progress) |p| p.report(sent, total);
            }
        },
        .file => |file| {
            // Read straight into the body writer's buffer: no extra copy, and memory use
            // stays at one buffer whatever the file size.
            var offset: u64 = 0;
            while (offset < file.size) {
                const dest = try w.writableSliceGreedy(1);
                const want: usize = @intCast(@min(dest.len, file.size - offset));
                const n = try file.handle.readPositional(request.client.io, &.{dest[0..want]}, offset);
                if (n == 0) return error.FileChanged;
                w.advance(n);
                offset += n;
                sent += n;
                if (progress) |p| p.report(sent, total);
            }
        },
    }

    try form.writeTail(w);
    try body.end();
    try request.connection.?.flush();
    if (progress) |p| p.report(total, total);
}

fn readResponse(gpa: Allocator, response: *http.Client.Response, diagnostics: ?*Diagnostics) ![]u8 {
    const status = response.head.status;
    if (diagnostics) |d| d.status = status;

    var transfer_buffer: [64]u8 = undefined;
    const reader = response.reader(&transfer_buffer);
    var text_buffer: [max_response_len]u8 = undefined;
    const len = reader.readSliceShort(&text_buffer) catch |err| {
        if (diagnostics) |d| d.cause = response.bodyErr();
        return err;
    };
    _ = reader.discardRemaining() catch {}; // the connection is closed afterwards anyway

    const url = try parseResponse(status, text_buffer[0..len], diagnostics);
    return gpa.dupe(u8, url);
}

/// Returns the URL in a catbox response, or an error with the response text in `diagnostics`.
pub fn parseResponse(status: http.Status, body: []const u8, diagnostics: ?*Diagnostics) Error![]const u8 {
    const text = std.mem.trim(u8, body, &std.ascii.whitespace);
    errdefer if (diagnostics) |d| d.setBody(text);
    if (status.class() != .success) return error.HttpStatus;
    if (!std.mem.startsWith(u8, text, "https://") and !std.mem.startsWith(u8, text, "http://"))
        return error.UnexpectedResponse;
    return text;
}

const testing = std.testing;

test parseResponse {
    var d: Diagnostics = .{};
    try testing.expectEqualStrings(
        "https://files.catbox.moe/abc123.png",
        try parseResponse(.ok, "  https://files.catbox.moe/abc123.png\n", &d),
    );
    try testing.expectEqualStrings("http://localhost/x", try parseResponse(.ok, "http://localhost/x", null));

    try testing.expectError(error.UnexpectedResponse, parseResponse(.ok, "No files given.\n", &d));
    try testing.expectEqualStrings("No files given.", d.body());

    try testing.expectError(error.UnexpectedResponse, parseResponse(.ok, "", &d));
    try testing.expectEqualStrings("", d.body());

    try testing.expectError(error.HttpStatus, parseResponse(.internal_server_error, "Internal Server Error", &d));
    try testing.expectEqualStrings("Internal Server Error", d.body());

    // Even a URL is a failure when the status says so.
    try testing.expectError(error.HttpStatus, parseResponse(.found, "https://example.com", null));
}

test "Progress.init forwards to a typed callback" {
    const Counter = struct {
        calls: usize = 0,
        last: [2]u64 = .{ 0, 0 },
        fn update(c: *@This(), sent: u64, total: u64) void {
            c.calls += 1;
            c.last = .{ sent, total };
        }
    };
    var counter: Counter = .{};
    const progress: Progress = .init(&counter, Counter.update);
    progress.report(5, 10);
    progress.report(10, 10);
    try testing.expectEqual(2, counter.calls);
    try testing.expectEqual([2]u64{ 10, 10 }, counter.last);
}
