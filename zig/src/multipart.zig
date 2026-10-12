//! multipart/form-data encoding (RFC 7578) for the few shapes catbox needs: some text fields
//! followed by at most one file part.
//!
//! The encoder never holds the file contents. It writes everything before the file bytes
//! (`writeHead`) and everything after them (`writeTail`), and can measure both without
//! allocating, so the caller knows the exact body length before sending a single byte.

const std = @import("std");
const Io = std.Io;
const Writer = Io.Writer;

pub const Field = struct {
    name: []const u8,
    value: []const u8,
};

/// The header of the file part. Its contents are supplied separately, between
/// `writeHead` and `writeTail`.
pub const FileHeader = struct {
    name: []const u8,
    filename: []const u8,
    content_type: []const u8,
};

/// "catbox-" followed by 32 random hex digits. Long enough that it won't occur in a file.
pub const Boundary = [39]u8;

pub fn randomBoundary(io: Io) Boundary {
    var random_bytes: [16]u8 = undefined;
    io.random(&random_bytes);
    var boundary: Boundary = undefined;
    _ = std.fmt.bufPrint(&boundary, "catbox-{x}", .{&random_bytes}) catch unreachable;
    return boundary;
}

pub const Form = struct {
    boundary: Boundary,
    fields: []const Field,
    file: ?FileHeader = null,

    const content_type_prefix = "multipart/form-data; boundary=";
    pub const ContentTypeBuffer = [content_type_prefix.len + @sizeOf(Boundary)]u8;

    /// The request's Content-Type header value, which names the boundary.
    pub fn contentType(form: *const Form, buffer: *ContentTypeBuffer) []const u8 {
        buffer.* = (content_type_prefix ++ form.boundary).*;
        return buffer;
    }

    /// Writes the text fields and, if there is a file, its part header. The file bytes
    /// come next, then `writeTail`.
    pub fn writeHead(form: *const Form, w: *Writer) Writer.Error!void {
        for (form.fields) |field| {
            try w.print("--{s}\r\nContent-Disposition: form-data; name=\"", .{&form.boundary});
            try writeQuoted(w, field.name);
            try w.writeAll("\"\r\n\r\n");
            try w.writeAll(field.value);
            try w.writeAll("\r\n");
        }
        if (form.file) |file| {
            try w.print("--{s}\r\nContent-Disposition: form-data; name=\"", .{&form.boundary});
            try writeQuoted(w, file.name);
            try w.writeAll("\"; filename=\"");
            try writeQuoted(w, file.filename);
            try w.print("\"\r\nContent-Type: {s}\r\n\r\n", .{file.content_type});
        }
    }

    /// Ends the file part, if any, and closes the body.
    pub fn writeTail(form: *const Form, w: *Writer) Writer.Error!void {
        if (form.file != null) try w.writeAll("\r\n");
        try w.print("--{s}--\r\n", .{&form.boundary});
    }

    pub fn headLength(form: *const Form) u64 {
        return measure(form, writeHead);
    }

    pub fn tailLength(form: *const Form) u64 {
        return measure(form, writeTail);
    }

    fn measure(form: *const Form, comptime write: fn (*const Form, *Writer) Writer.Error!void) u64 {
        var counter: Writer.Discarding = .init(&.{});
        write(form, &counter.writer) catch unreachable; // a Discarding writer cannot fail
        return counter.fullCount();
    }
};

/// Writes a header parameter value. Backslashes and quotes are escaped, and line breaks are
/// dropped, because they would end the header early.
fn writeQuoted(w: *Writer, value: []const u8) Writer.Error!void {
    for (value) |c| switch (c) {
        '\\', '"' => try w.writeAll(&.{ '\\', c }),
        '\r', '\n' => {},
        else => try w.writeByte(c),
    };
}

const testing = std.testing;

test "fields and a file part" {
    const form: Form = .{
        .boundary = "catbox-0123456789abcdef0123456789abcdef".*,
        .fields = &.{
            .{ .name = "reqtype", .value = "fileupload" },
            .{ .name = "userhash", .value = "HASH" },
        },
        .file = .{ .name = "fileToUpload", .filename = "my \"photo\".png", .content_type = "image/png" },
    };
    var buffer: [512]u8 = undefined;
    var w: Writer = .fixed(&buffer);
    try form.writeHead(&w);
    const head_len = w.end;
    try w.writeAll("PNGDATA");
    try form.writeTail(&w);

    try testing.expectEqualStrings(
        "--catbox-0123456789abcdef0123456789abcdef\r\n" ++
            "Content-Disposition: form-data; name=\"reqtype\"\r\n\r\nfileupload\r\n" ++
            "--catbox-0123456789abcdef0123456789abcdef\r\n" ++
            "Content-Disposition: form-data; name=\"userhash\"\r\n\r\nHASH\r\n" ++
            "--catbox-0123456789abcdef0123456789abcdef\r\n" ++
            "Content-Disposition: form-data; name=\"fileToUpload\"; filename=\"my \\\"photo\\\".png\"\r\n" ++
            "Content-Type: image/png\r\n\r\n" ++
            "PNGDATA\r\n" ++
            "--catbox-0123456789abcdef0123456789abcdef--\r\n",
        w.buffered(),
    );
    try testing.expectEqual(head_len, form.headLength());
    try testing.expectEqual(w.end - head_len - "PNGDATA".len, form.tailLength());
}

test "fields only" {
    const form: Form = .{
        .boundary = "catbox-0123456789abcdef0123456789abcdef".*,
        .fields = &.{.{ .name = "url", .value = "https://example.com/a.png" }},
    };
    var buffer: [256]u8 = undefined;
    var w: Writer = .fixed(&buffer);
    try form.writeHead(&w);
    try form.writeTail(&w);
    try testing.expectEqualStrings(
        "--catbox-0123456789abcdef0123456789abcdef\r\n" ++
            "Content-Disposition: form-data; name=\"url\"\r\n\r\nhttps://example.com/a.png\r\n" ++
            "--catbox-0123456789abcdef0123456789abcdef--\r\n",
        w.buffered(),
    );
    try testing.expectEqual(@as(u64, w.end), form.headLength() + form.tailLength());

    var content_type: Form.ContentTypeBuffer = undefined;
    try testing.expectEqualStrings(
        "multipart/form-data; boundary=catbox-0123456789abcdef0123456789abcdef",
        form.contentType(&content_type),
    );
}

test "header values cannot break out of their quotes" {
    var buffer: [64]u8 = undefined;
    var w: Writer = .fixed(&buffer);
    try writeQuoted(&w, "a\\b\"c\r\nd.txt");
    try testing.expectEqualStrings("a\\\\b\\\"cd.txt", w.buffered());
}

test randomBoundary {
    const a = randomBoundary(testing.io);
    const b = randomBoundary(testing.io);
    try testing.expect(std.mem.startsWith(u8, &a, "catbox-"));
    for (a["catbox-".len..]) |c| try testing.expect(std.ascii.isHex(c));
    try testing.expect(!std.mem.eql(u8, &a, &b));
}
