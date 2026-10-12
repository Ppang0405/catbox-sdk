//! The `catbox` command. See conformance/SPEC.md for the behavior shared by every
//! implementation: one URL per line on stdout, one `catbox: <arg>: <reason>` line per failure
//! on stderr, exit status 0 (all succeeded), 1 (some failed) or 2 (usage error).

const std = @import("std");
const Io = std.Io;
const Allocator = std.mem.Allocator;

const catbox = @import("catbox");
const args = @import("args.zig");
const ProgressBar = @import("ProgressBar.zig");

const exit_failure = 1;
const exit_usage = 2;

pub fn main(init: std.process.Init) u8 {
    const io = init.io;
    const arena = init.arena.allocator();

    // Streaming (not positional) writers: when stdout is redirected to a file, appending at
    // the current offset is what the shell expects, e.g. with `>>`.
    var stdout_buffer: [4096]u8 = undefined;
    var stdout = Io.File.stdout().writerStreaming(io, &stdout_buffer);
    var stderr_buffer: [1024]u8 = undefined;
    var stderr = Io.File.stderr().writerStreaming(io, &stderr_buffer);

    var cli: Cli = .{
        .gpa = init.gpa,
        .io = io,
        .stdout = &stdout.interface,
        .stderr = &stderr.interface,
        .environ = init.environ_map,
    };
    // Windows hands over UTF-16 arguments; `toSlice` converts them to WTF-8 like std expects.
    const argv = init.minimal.args.toSlice(arena) catch return cli.outOfMemory();
    const invocation = args.parse(arena, if (argv.len > 0) argv[1..] else argv) catch return cli.outOfMemory();
    return cli.dispatch(invocation);
}

const Cli = struct {
    gpa: Allocator,
    io: Io,
    stdout: *Io.Writer,
    stderr: *Io.Writer,
    environ: *const std.process.Environ.Map,

    fn dispatch(cli: *Cli, invocation: args.Invocation) u8 {
        switch (invocation) {
            .version => return cli.print("catbox {s}\n", .{catbox.version}),
            .help => |subcommand| {
                args.writeHelp(cli.stdout, subcommand) catch {};
                return cli.flushStdout();
            },
            .usage_error => |usage| {
                cli.stderr.print("catbox: {s}\n", .{usage.message}) catch {};
                args.writeUsage(cli.stderr, usage.subcommand) catch {};
                if (usage.subcommand) |s| {
                    cli.stderr.print("Run 'catbox {s} --help' for more information.\n", .{s.name()}) catch {};
                } else {
                    cli.stderr.writeAll("Run 'catbox --help' for more information.\n") catch {};
                }
                cli.stderr.flush() catch {};
                return exit_usage;
            },
            .run => |run| {
                const all_succeeded = cli.execute(run);
                return if (all_succeeded) 0 else exit_failure;
            },
        }
    }

    /// Runs one subcommand over all its operands. Returns false if any of them failed.
    fn execute(cli: *Cli, run: args.Run) bool {
        var http: std.http.Client = .{ .allocator = cli.gpa, .io = cli.io };
        defer http.deinit();

        // Progress is drawn only for a person watching, never into a pipe or log file.
        const show_progress = !run.quiet and (Io.File.stderr().isTty(cli.io) catch false);
        var all_succeeded = true;
        for (run.operands) |operand| {
            var diagnostics: catbox.Diagnostics = .{};
            var bar: ProgressBar = .init(cli.stderr, std.fs.path.basename(operand));
            const options: catbox.Options = .{
                .diagnostics = &diagnostics,
                .progress = if (show_progress) .init(&bar, ProgressBar.update) else null,
            };
            const result = switch (run.subcommand) {
                .upload => cli.catboxClient(&http, run).uploadFile(cli.gpa, operand, options),
                .upload_url => cli.catboxClient(&http, run).uploadUrl(cli.gpa, operand, options),
                .litter => litterbox: {
                    const client: catbox.Litterbox = .{
                        .http = &http,
                        .endpoint = run.endpoint orelse catbox.Litterbox.default_endpoint,
                    };
                    break :litterbox client.uploadFile(cli.gpa, operand, run.expiry, options);
                },
            };
            bar.finish();

            if (result) |url| {
                defer cli.gpa.free(url);
                if (cli.print("{s}\n", .{url}) != 0) return false;
            } else |_| {
                all_succeeded = false;
                cli.stderr.print("catbox: {s}: {f}\n", .{ operand, &diagnostics }) catch {};
                cli.stderr.flush() catch {};
            }
        }
        return all_succeeded;
    }

    fn catboxClient(cli: *Cli, http: *std.http.Client, run: args.Run) catbox.Catbox {
        return .{
            .http = http,
            .user_hash = run.user_hash orelse cli.environ.get("CATBOX_USERHASH"),
            .endpoint = run.endpoint orelse catbox.Catbox.default_endpoint,
        };
    }

    /// Writes to stdout and flushes, so each URL is visible (and pipeable) as soon as it is known.
    fn print(cli: *Cli, comptime format: []const u8, values: anytype) u8 {
        cli.stdout.print(format, values) catch {};
        return cli.flushStdout();
    }

    fn flushStdout(cli: *Cli) u8 {
        cli.stdout.flush() catch return exit_failure; // e.g. the reading end of a pipe closed
        return 0;
    }

    fn outOfMemory(cli: *Cli) u8 {
        cli.stderr.writeAll("catbox: out of memory\n") catch {};
        cli.stderr.flush() catch {};
        return exit_failure;
    }
};

test {
    _ = args;
    _ = ProgressBar;
}
