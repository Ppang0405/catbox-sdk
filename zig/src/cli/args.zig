//! Command-line parsing. Hand-written, because std has no argument parser and the grammar is
//! small: a subcommand, a few options (anywhere, as `--name value`, `--name=value`, `-t value`
//! or `-t1h`), `--` to end options, and operands.

const std = @import("std");
const Allocator = std.mem.Allocator;
const Writer = std.Io.Writer;

const catbox = @import("catbox");
const Expiry = catbox.Expiry;

pub const Subcommand = enum {
    upload,
    upload_url,
    litter,

    pub fn name(s: Subcommand) []const u8 {
        return switch (s) {
            .upload => "upload",
            .upload_url => "upload-url",
            .litter => "litter",
        };
    }

    fn fromName(text: []const u8) ?Subcommand {
        inline for (@typeInfo(Subcommand).@"enum".field_names) |field| {
            const s = @field(Subcommand, field);
            if (std.mem.eql(u8, text, s.name())) return s;
        }
        return null;
    }
};

/// A subcommand to run, with its options and operands.
pub const Run = struct {
    subcommand: Subcommand,
    /// Files, or URLs for `upload-url`.
    operands: []const []const u8,
    quiet: bool = false,
    /// From `--user-hash`. Null when not given, so the environment can supply it.
    user_hash: ?[]const u8 = null,
    endpoint: ?[]const u8 = null,
    expiry: Expiry = .default,
};

pub const Invocation = union(enum) {
    run: Run,
    /// `--help`, for the program or one subcommand.
    help: ?Subcommand,
    version,
    usage_error: UsageError,
};

pub const UsageError = struct {
    /// Which usage line to print after the message.
    subcommand: ?Subcommand,
    message: []const u8,
};

/// Parses the arguments after the program name. Messages and the operand list are
/// allocated with `arena`.
pub fn parse(arena: Allocator, args: []const []const u8) Allocator.Error!Invocation {
    var parser: Parser = .{ .arena = arena, .args = args };
    return parser.parse();
}

const Parser = struct {
    arena: Allocator,
    args: []const []const u8,
    index: usize = 0,
    subcommand: ?Subcommand = null,

    fn parse(p: *Parser) Allocator.Error!Invocation {
        const first = p.next() orelse return p.fail("no command given", .{});
        if (isHelp(first)) return .{ .help = null };
        if (std.mem.eql(u8, first, "--version")) return .version;
        if (isOption(first)) return p.fail("unknown option '{s}'", .{first});
        const subcommand = Subcommand.fromName(first) orelse
            return p.fail("unknown command '{s}'", .{first});
        p.subcommand = subcommand;

        var run: Run = .{ .subcommand = subcommand, .operands = &.{} };
        var operands: std.ArrayList([]const u8) = .empty;
        var only_operands = false;
        while (p.next()) |arg| {
            if (only_operands or !isOption(arg)) {
                try operands.append(p.arena, arg);
            } else if (std.mem.eql(u8, arg, "--")) {
                only_operands = true;
            } else if (std.mem.startsWith(u8, arg, "--")) {
                const eq = std.mem.findScalar(u8, arg, '=');
                const name = arg[0 .. eq orelse arg.len];
                const inline_value: ?[]const u8 = if (eq) |i| arg[i + 1 ..] else null;
                if (std.mem.eql(u8, name, "--help")) return .{ .help = subcommand };
                if (std.mem.eql(u8, name, "--quiet") and inline_value == null) {
                    run.quiet = true;
                    continue;
                }
                const option = LongOption.fromName(name, subcommand) orelse
                    return p.fail("unknown option '{s}'", .{arg});
                const value = inline_value orelse p.next() orelse
                    return p.fail("option '{s}' needs a value", .{name});
                if (try p.setOption(&run, option, name, value)) |usage_error| return usage_error;
            } else {
                // A cluster of short options, such as "-q", "-t 1h", "-t1h" or "-qt1h".
                var i: usize = 1;
                while (i < arg.len) : (i += 1) switch (arg[i]) {
                    'h' => return .{ .help = subcommand },
                    'q' => run.quiet = true,
                    't' => if (subcommand == .litter) {
                        const value = if (i + 1 < arg.len) arg[i + 1 ..] else p.next() orelse
                            return p.fail("option '-t' needs a value", .{});
                        if (try p.setOption(&run, .time, "-t", value)) |usage_error| return usage_error;
                        break;
                    } else return p.fail("unknown option '-t'", .{}),
                    else => return p.fail("unknown option '-{c}'", .{arg[i]}),
                };
            }
        }

        if (operands.items.len == 0) return p.fail("no {s} given", .{
            if (subcommand == .upload_url) "URLs" else "files",
        });
        run.operands = operands.items;
        return .{ .run = run };
    }

    const LongOption = enum {
        user_hash,
        endpoint,
        time,

        fn fromName(name: []const u8, subcommand: Subcommand) ?LongOption {
            if (std.mem.eql(u8, name, "--endpoint")) return .endpoint;
            if (subcommand == .litter) {
                if (std.mem.eql(u8, name, "--time")) return .time;
            } else {
                if (std.mem.eql(u8, name, "--user-hash")) return .user_hash;
            }
            return null;
        }
    };

    fn setOption(p: *Parser, run: *Run, option: LongOption, name: []const u8, value: []const u8) Allocator.Error!?Invocation {
        switch (option) {
            .user_hash => run.user_hash = value,
            .endpoint => {
                const uri = std.Uri.parse(value) catch
                    return try p.fail("invalid endpoint URL '{s}'", .{value});
                if (!std.ascii.eqlIgnoreCase(uri.scheme, "http") and !std.ascii.eqlIgnoreCase(uri.scheme, "https"))
                    return try p.fail("invalid endpoint URL '{s}'", .{value});
                run.endpoint = value;
            },
            .time => run.expiry = Expiry.parse(value) orelse
                return try p.fail("invalid value '{s}' for {s} (choose from 1h, 12h, 24h, 72h)", .{ value, name }),
        }
        return null;
    }

    fn next(p: *Parser) ?[]const u8 {
        if (p.index == p.args.len) return null;
        defer p.index += 1;
        return p.args[p.index];
    }

    fn fail(p: *Parser, comptime format: []const u8, args: anytype) Allocator.Error!Invocation {
        return .{ .usage_error = .{
            .subcommand = p.subcommand,
            .message = try std.fmt.allocPrint(p.arena, format, args),
        } };
    }
};

/// "-x" and "--x" are options; "-" alone is an operand (conventionally standard input).
fn isOption(arg: []const u8) bool {
    return arg.len > 1 and arg[0] == '-';
}

fn isHelp(arg: []const u8) bool {
    return std.mem.eql(u8, arg, "-h") or std.mem.eql(u8, arg, "--help");
}

pub fn writeUsage(w: *Writer, subcommand: ?Subcommand) Writer.Error!void {
    try w.writeAll(switch (subcommand orelse return w.writeAll(main_usage)) {
        .upload => "Usage: catbox upload [--user-hash HASH] [-q] FILE...\n",
        .upload_url => "Usage: catbox upload-url [--user-hash HASH] URL...\n",
        .litter => "Usage: catbox litter [-t 1h|12h|24h|72h] [-q] FILE...\n",
    });
}

const main_usage = "Usage: catbox <command> [options] <args>...\n";

pub fn writeHelp(w: *Writer, subcommand: ?Subcommand) Writer.Error!void {
    try writeUsage(w, subcommand);
    try w.writeAll(switch (subcommand orelse return w.writeAll(main_help)) {
        .upload =>
        \\
        \\Upload files to catbox.moe permanently (max 200 MB each).
        \\Prints one URL per line.
        \\
        \\Options:
        \\      --user-hash HASH  your catbox user hash, so uploads show up in your
        \\                        account (default: $CATBOX_USERHASH)
        \\  -q, --quiet           don't show upload progress
        \\  -h, --help            show this help
        \\
        ,
        .upload_url =>
        \\
        \\Have catbox.moe fetch and store files from remote URLs.
        \\Prints one catbox URL per line.
        \\
        \\Options:
        \\      --user-hash HASH  your catbox user hash, so uploads show up in your
        \\                        account (default: $CATBOX_USERHASH)
        \\  -h, --help            show this help
        \\
        ,
        .litter =>
        \\
        \\Upload files to litterbox.catbox.moe temporarily (max 1 GB each).
        \\Prints one URL per line.
        \\
        \\Options:
        \\  -t, --time TIME  how long to keep the files: 1h, 12h, 24h or 72h
        \\                   (default: 72h)
        \\  -q, --quiet      don't show upload progress
        \\  -h, --help       show this help
        \\
        ,
    });
}

const main_help =
    \\
    \\Upload files to catbox.moe and litterbox.catbox.moe.
    \\
    \\Commands:
    \\  upload      upload files to catbox.moe permanently (max 200 MB each)
    \\  upload-url  have catbox.moe fetch and store files from remote URLs
    \\  litter      upload files to litterbox.catbox.moe temporarily (max 1 GB each)
    \\
    \\Options:
    \\  -h, --help  show this help
    \\  --version   print the version
    \\
    \\Run 'catbox <command> --help' for the options of a command.
    \\
;

const testing = std.testing;

fn parseForTest(arena: *std.heap.ArenaAllocator, args: []const []const u8) !Invocation {
    return parse(arena.allocator(), args);
}

fn expectUsageError(args: []const []const u8, expected_message: []const u8) !void {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const invocation = try parseForTest(&arena, args);
    try testing.expectEqualStrings(expected_message, invocation.usage_error.message);
}

test "upload with options anywhere" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const run = (try parseForTest(&arena, &.{ "upload", "a.png", "--user-hash", "H", "-q", "b.txt", "--endpoint=http://127.0.0.1:1/x" })).run;
    try testing.expectEqual(Subcommand.upload, run.subcommand);
    try testing.expectEqualStrings("H", run.user_hash.?);
    try testing.expect(run.quiet);
    try testing.expectEqualStrings("http://127.0.0.1:1/x", run.endpoint.?);
    try testing.expectEqual(2, run.operands.len);
    try testing.expectEqualStrings("a.png", run.operands[0]);
    try testing.expectEqualStrings("b.txt", run.operands[1]);
}

test "user hash defaults to null, and an empty one is kept" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    try testing.expectEqual(null, (try parseForTest(&arena, &.{ "upload", "a" })).run.user_hash);
    try testing.expectEqualStrings("", (try parseForTest(&arena, &.{ "upload", "--user-hash=", "a" })).run.user_hash.?);
}

test "litter expiry forms" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    try testing.expectEqual(Expiry.three_days, (try parseForTest(&arena, &.{ "litter", "f" })).run.expiry);
    try testing.expectEqual(Expiry.one_hour, (try parseForTest(&arena, &.{ "litter", "-t", "1h", "f" })).run.expiry);
    try testing.expectEqual(Expiry.twelve_hours, (try parseForTest(&arena, &.{ "litter", "--time", "12h", "f" })).run.expiry);
    try testing.expectEqual(Expiry.one_day, (try parseForTest(&arena, &.{ "litter", "--time=24h", "f" })).run.expiry);
    const run = (try parseForTest(&arena, &.{ "litter", "-qt1h", "f" })).run;
    try testing.expectEqual(Expiry.one_hour, run.expiry);
    try testing.expect(run.quiet);
}

test "double dash ends options" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const run = (try parseForTest(&arena, &.{ "upload", "--", "-q", "--odd-name" })).run;
    try testing.expect(!run.quiet);
    try testing.expectEqual(2, run.operands.len);
    try testing.expectEqualStrings("--odd-name", run.operands[1]);
}

test "help and version" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    try testing.expectEqual(Invocation.version, try parseForTest(&arena, &.{"--version"}));
    try testing.expectEqual(Invocation{ .help = null }, try parseForTest(&arena, &.{"--help"}));
    try testing.expectEqual(Invocation{ .help = .litter }, try parseForTest(&arena, &.{ "litter", "-h" }));
    try testing.expectEqual(Invocation{ .help = .upload_url }, try parseForTest(&arena, &.{ "upload-url", "x", "--help" }));
}

test "usage errors" {
    try expectUsageError(&.{}, "no command given");
    try expectUsageError(&.{"download"}, "unknown command 'download'");
    try expectUsageError(&.{"upload"}, "no files given");
    try expectUsageError(&.{"upload-url"}, "no URLs given");
    try expectUsageError(&.{ "upload", "--no-such-option", "a" }, "unknown option '--no-such-option'");
    try expectUsageError(&.{ "upload", "-x", "a" }, "unknown option '-x'");
    try expectUsageError(&.{ "upload", "-t", "1h", "a" }, "unknown option '-t'");
    try expectUsageError(&.{ "litter", "--user-hash", "H", "a" }, "unknown option '--user-hash'");
    try expectUsageError(&.{ "litter", "-t", "5h", "a" }, "invalid value '5h' for -t (choose from 1h, 12h, 24h, 72h)");
    try expectUsageError(&.{ "litter", "a", "--time" }, "option '--time' needs a value");
    try expectUsageError(&.{ "upload", "--endpoint", "ftp://x/y", "a" }, "invalid endpoint URL 'ftp://x/y'");
    try expectUsageError(&.{ "upload", "--quiet=yes", "a" }, "unknown option '--quiet=yes'");
}
