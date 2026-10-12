//! End-to-end tests of the clients against a one-shot HTTP server on the loopback interface,
//! checking what actually goes over the wire.

const std = @import("std");
const Io = std.Io;
const http = std.http;
const testing = std.testing;
const Allocator = std.mem.Allocator;

const catbox = @import("root.zig");

const Received = struct {
    target: []u8 = &.{},
    content_length: ?u64 = null,
    transfer_encoding: http.TransferEncoding = .none,
    user_agent: []u8 = &.{},
    content_type: []u8 = &.{},
    body: []u8 = &.{},

    fn deinit(r: *Received, gpa: Allocator) void {
        for ([_][]u8{ r.target, r.user_agent, r.content_type, r.body }) |bytes| gpa.free(bytes);
    }

    fn expectBodyContains(r: *const Received, needle: []const u8) !void {
        if (std.mem.find(u8, r.body, needle) == null) {
            std.debug.print("body does not contain {s}:\n{s}\n", .{ needle, r.body });
            return error.TestExpectedEqual;
        }
    }
};

/// Accepts one connection, records the request, and answers with `status` and `reply`.
const TestServer = struct {
    listener: Io.net.Server,
    endpoint_buffer: [64]u8 = undefined,
    endpoint: []const u8 = &.{},

    fn init(ts: *TestServer, io: Io) !void {
        const address: Io.net.IpAddress = try .parseIp4("127.0.0.1", 0);
        ts.* = .{ .listener = try address.listen(io, .{}) };
        ts.endpoint = try std.fmt.bufPrint(&ts.endpoint_buffer, "http://127.0.0.1:{d}/api.php", .{
            ts.listener.socket.address.getPort(),
        });
    }

    fn deinit(ts: *TestServer, io: Io) void {
        ts.listener.deinit(io);
    }

    fn serveOne(ts: *TestServer, io: Io, gpa: Allocator, status: http.Status, reply: []const u8, received: *Received) !void {
        const stream = try ts.listener.accept(io);
        defer stream.close(io);
        var read_buffer: [4096]u8 = undefined;
        var write_buffer: [1024]u8 = undefined;
        var reader = stream.reader(io, &read_buffer);
        var writer = stream.writer(io, &write_buffer);
        var server: http.Server = .init(&reader.interface, &writer.interface);

        var request = try server.receiveHead();
        received.target = try gpa.dupe(u8, request.head.target);
        received.content_length = request.head.content_length;
        received.transfer_encoding = request.head.transfer_encoding;
        received.content_type = try gpa.dupe(u8, request.head.content_type orelse "");
        var headers = request.iterateHeaders();
        while (headers.next()) |header| {
            if (std.ascii.eqlIgnoreCase(header.name, "user-agent")) received.user_agent = try gpa.dupe(u8, header.value);
        }
        const body = try request.readerExpectContinue(&.{});
        received.body = try body.allocRemaining(gpa, .unlimited);
        try request.respond(reply, .{ .status = status, .keep_alive = false });
    }
};

fn expectMessage(expected: []const u8, diagnostics: *const catbox.Diagnostics) !void {
    var buffer: [256]u8 = undefined;
    try testing.expectEqualStrings(expected, try std.fmt.bufPrint(&buffer, "{f}", .{diagnostics}));
}

test "uploadBytes sends a multipart body with an exact Content-Length" {
    const io = testing.io;
    const gpa = testing.allocator;
    var server: TestServer = undefined;
    try server.init(io);
    defer server.deinit(io);

    var received: Received = .{};
    defer received.deinit(gpa);
    var serving = try io.concurrent(TestServer.serveOne, .{ &server, io, gpa, .ok, "https://files.catbox.moe/abc123.txt\n", &received });

    var client: http.Client = .{ .allocator = gpa, .io = io };
    defer client.deinit();
    const box: catbox.Catbox = .{ .http = &client, .user_hash = "HASH", .endpoint = server.endpoint };
    const url = try box.uploadBytes(gpa, "hello", "notes.txt", .{});
    defer gpa.free(url);
    try serving.await(io);

    try testing.expectEqualStrings("https://files.catbox.moe/abc123.txt", url);
    try testing.expectEqualStrings("/api.php", received.target);
    try testing.expectEqual(http.TransferEncoding.none, received.transfer_encoding);
    try testing.expectEqual(@as(?u64, received.body.len), received.content_length);
    try testing.expectEqualStrings(catbox.user_agent, received.user_agent);
    try testing.expect(std.mem.startsWith(u8, received.content_type, "multipart/form-data; boundary=catbox-"));
    try received.expectBodyContains("name=\"reqtype\"\r\n\r\nfileupload\r\n");
    try received.expectBodyContains("name=\"userhash\"\r\n\r\nHASH\r\n");
    try received.expectBodyContains("name=\"fileToUpload\"; filename=\"notes.txt\"\r\nContent-Type: text/plain\r\n\r\nhello\r\n");
}

test "uploadFile streams a file larger than the send buffer and reports progress" {
    const io = testing.io;
    const gpa = testing.allocator;
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();

    const contents = try gpa.alloc(u8, 200 * 1024 + 7);
    defer gpa.free(contents);
    for (contents, 0..) |*byte, i| byte.* = @truncate(i *% 31);
    try tmp.dir.writeFile(io, .{ .sub_path = "Big Photo.PNG", .data = contents });
    const path = try std.fs.path.join(gpa, &.{ ".zig-cache", "tmp", &tmp.sub_path, "Big Photo.PNG" });
    defer gpa.free(path);

    var server: TestServer = undefined;
    try server.init(io);
    defer server.deinit(io);
    var received: Received = .{};
    defer received.deinit(gpa);
    var serving = try io.concurrent(TestServer.serveOne, .{ &server, io, gpa, .ok, "https://litter.catbox.moe/x.PNG", &received });

    const Recorder = struct {
        calls: usize = 0,
        sent: u64 = 0,
        total: u64 = 0,
        fn update(r: *@This(), sent: u64, total: u64) void {
            std.debug.assert(sent >= r.sent); // never goes backwards
            r.* = .{ .calls = r.calls + 1, .sent = sent, .total = total };
        }
    };
    var recorder: Recorder = .{};

    var client: http.Client = .{ .allocator = gpa, .io = io };
    defer client.deinit();
    const box: catbox.Litterbox = .{ .http = &client, .endpoint = server.endpoint };
    const url = try box.uploadFile(gpa, path, .one_hour, .{ .progress = .init(&recorder, Recorder.update) });
    defer gpa.free(url);
    try serving.await(io);

    try testing.expectEqualStrings("https://litter.catbox.moe/x.PNG", url);
    try testing.expectEqual(@as(?u64, received.body.len), received.content_length);
    try received.expectBodyContains("name=\"time\"\r\n\r\n1h\r\n");
    try received.expectBodyContains("filename=\"Big Photo.PNG\"\r\nContent-Type: image/png\r\n\r\n");
    try testing.expect(std.mem.find(u8, received.body, contents) != null);
    try testing.expectEqual(received.body.len, recorder.total);
    try testing.expectEqual(recorder.total, recorder.sent);
    try testing.expect(recorder.calls > 3);
}

test "uploadUrl sends only fields" {
    const io = testing.io;
    const gpa = testing.allocator;
    var server: TestServer = undefined;
    try server.init(io);
    defer server.deinit(io);
    var received: Received = .{};
    defer received.deinit(gpa);
    var serving = try io.concurrent(TestServer.serveOne, .{ &server, io, gpa, .ok, "https://files.catbox.moe/copy.png", &received });

    var client: http.Client = .{ .allocator = gpa, .io = io };
    defer client.deinit();
    const box: catbox.Catbox = .{ .http = &client, .user_hash = "", .endpoint = server.endpoint };
    const url = try box.uploadUrl(gpa, "https://example.com/a.png", .{});
    defer gpa.free(url);
    try serving.await(io);

    try testing.expectEqualStrings("https://files.catbox.moe/copy.png", url);
    try received.expectBodyContains("name=\"reqtype\"\r\n\r\nurlupload\r\n");
    try received.expectBodyContains("name=\"url\"\r\n\r\nhttps://example.com/a.png\r\n");
    try testing.expect(std.mem.find(u8, received.body, "userhash") == null); // empty means anonymous
    try testing.expect(std.mem.find(u8, received.body, "filename=") == null);
}

test "an HTTP error keeps the status and the server's message" {
    const io = testing.io;
    const gpa = testing.allocator;
    var server: TestServer = undefined;
    try server.init(io);
    defer server.deinit(io);
    var received: Received = .{};
    defer received.deinit(gpa);
    var serving = try io.concurrent(TestServer.serveOne, .{ &server, io, gpa, .internal_server_error, "Internal Server Error\n", &received });

    var client: http.Client = .{ .allocator = gpa, .io = io };
    defer client.deinit();
    const box: catbox.Catbox = .{ .http = &client, .endpoint = server.endpoint };
    var diagnostics: catbox.Diagnostics = .{};
    try testing.expectError(error.HttpStatus, box.uploadBytes(gpa, "x", "a.bin", .{ .diagnostics = &diagnostics }));
    try serving.await(io);

    try testing.expectEqual(http.Status.internal_server_error, diagnostics.status.?);
    try testing.expectEqualStrings("Internal Server Error", diagnostics.body());
    try expectMessage("server returned HTTP 500: Internal Server Error", &diagnostics);
}

test "a plain-text error with status 200 is a failure" {
    const io = testing.io;
    const gpa = testing.allocator;
    var server: TestServer = undefined;
    try server.init(io);
    defer server.deinit(io);
    var received: Received = .{};
    defer received.deinit(gpa);
    var serving = try io.concurrent(TestServer.serveOne, .{ &server, io, gpa, .ok, "No files given.", &received });

    var client: http.Client = .{ .allocator = gpa, .io = io };
    defer client.deinit();
    const box: catbox.Catbox = .{ .http = &client, .endpoint = server.endpoint };
    var diagnostics: catbox.Diagnostics = .{};
    try testing.expectError(error.UnexpectedResponse, box.uploadBytes(gpa, "x", "a.bin", .{ .diagnostics = &diagnostics }));
    try serving.await(io);
    try expectMessage("server error: No files given.", &diagnostics);
}

test "a missing file or a directory fails before connecting" {
    const io = testing.io;
    const gpa = testing.allocator;
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const missing = try std.fs.path.join(gpa, &.{ ".zig-cache", "tmp", &tmp.sub_path, "missing.png" });
    defer gpa.free(missing);
    const directory = try std.fs.path.join(gpa, &.{ ".zig-cache", "tmp", &tmp.sub_path });
    defer gpa.free(directory);

    var client: http.Client = .{ .allocator = gpa, .io = io };
    defer client.deinit();
    // Nothing listens on port 9 (discard); connecting would fail with a different error.
    const box: catbox.Catbox = .{ .http = &client, .endpoint = "http://127.0.0.1:9/api.php" };
    var diagnostics: catbox.Diagnostics = .{};
    try testing.expectError(error.FileNotFound, box.uploadFile(gpa, missing, .{ .diagnostics = &diagnostics }));
    try expectMessage("no such file", &diagnostics);
    try testing.expectError(error.IsDir, box.uploadFile(gpa, directory, .{ .diagnostics = &diagnostics }));
    try expectMessage("is a directory", &diagnostics);
}

test "an endpoint that isn't an http URL is rejected" {
    var client: http.Client = .{ .allocator = testing.allocator, .io = testing.io };
    defer client.deinit();
    const box: catbox.Catbox = .{ .http = &client, .endpoint = "ftp://example.com/api.php" };
    try testing.expectError(error.InvalidEndpoint, box.uploadUrl(testing.allocator, "https://example.com/", .{}));
}
