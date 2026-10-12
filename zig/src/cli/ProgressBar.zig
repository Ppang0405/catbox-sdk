//! A single-line progress bar on stderr, so stdout stays clean for piping URLs:
//!
//!     photo.jpg [===========>            ]  47.3% 1.2 MB/2.5 MB
//!
//! Each redraw returns to the start of the line with '\r' and pads over the previous text
//! with spaces. ANSI erase codes would be shorter, but older Windows consoles print them
//! literally.

const std = @import("std");
const Writer = std.Io.Writer;

const ProgressBar = @This();

out: *Writer,
label: []const u8,
/// The last drawn progress in tenths of a percent, to redraw only when it changes.
last_permille: ?u64 = null,
/// Width of the last line drawn, in characters, so the next one can cover it.
line_width: usize = 0,

const bar_width = 24;

pub fn init(out: *Writer, label: []const u8) ProgressBar {
    return .{ .out = out, .label = label };
}

/// Matches `catbox.Progress`, so it can be passed as `.progress = .init(&bar, ProgressBar.update)`.
pub fn update(bar: *ProgressBar, sent: u64, total: u64) void {
    if (total == 0) return;
    const permille = @min(sent, total) * 1000 / total;
    if (bar.last_permille == permille) return;
    bar.last_permille = permille;

    var line_buffer: [1024]u8 = undefined;
    var line: Writer = .fixed(&line_buffer);
    render(&line, bar.label, permille, sent, total) catch {}; // a very long label is cut off
    const text = line.buffered();
    const width = std.unicode.utf8CountCodepoints(text) catch text.len;

    // Progress is cosmetic: a failed write to stderr shouldn't fail the upload.
    bar.out.writeByte('\r') catch return;
    bar.out.writeAll(text) catch return;
    bar.out.splatByteAll(' ', bar.line_width -| width) catch return;
    bar.out.flush() catch return;
    bar.line_width = width;
}

/// Erases the bar, leaving the cursor at the start of an empty line.
pub fn finish(bar: *ProgressBar) void {
    if (bar.line_width == 0) return;
    bar.out.writeByte('\r') catch return;
    bar.out.splatByteAll(' ', bar.line_width) catch return;
    bar.out.writeByte('\r') catch return;
    bar.out.flush() catch return;
    bar.line_width = 0;
}

fn render(w: *Writer, label: []const u8, permille: u64, sent: u64, total: u64) Writer.Error!void {
    const filled: usize = @intCast(permille * bar_width / 1000);
    try w.print("{s} [", .{label});
    try w.splatByteAll('=', filled);
    if (filled < bar_width) {
        try w.writeByte('>');
        try w.splatByteAll(' ', bar_width - filled - 1);
    }
    try w.print("] {d:>3}.{d}% ", .{ permille / 10, permille % 10 });
    try writeBytes(w, sent);
    try w.writeByte('/');
    try writeBytes(w, total);
}

/// "512 B", "1.5 KB", "24.0 MB": binary units, one decimal.
fn writeBytes(w: *Writer, count: u64) Writer.Error!void {
    const units = [_][]const u8{ "B", "KB", "MB", "GB" };
    var value: f64 = @floatFromInt(count);
    var unit: usize = 0;
    while (value >= 1024 and unit < units.len - 1) : (unit += 1) value /= 1024;
    if (unit == 0) return w.print("{d} B", .{count});
    try w.print("{d:.1} {s}", .{ value, units[unit] });
}

const testing = std.testing;

test render {
    var buffer: [256]u8 = undefined;
    var w: Writer = .fixed(&buffer);
    try render(&w, "a.bin", 0, 0, 2048);
    try testing.expectEqualStrings("a.bin [>                       ]   0.0% 0 B/2.0 KB", w.buffered());

    w = .fixed(&buffer);
    try render(&w, "a.bin", 473, 1240000, 2621440);
    try testing.expectEqualStrings("a.bin [===========>            ]  47.3% 1.2 MB/2.5 MB", w.buffered());

    w = .fixed(&buffer);
    try render(&w, "a.bin", 1000, 25165831, 25165831);
    try testing.expectEqualStrings("a.bin [========================] 100.0% 24.0 MB/24.0 MB", w.buffered());
}

test "redraws pad over the previous line and finish erases it" {
    var buffer: [1024]u8 = undefined;
    var w: Writer = .fixed(&buffer);
    var bar: ProgressBar = .init(&w, "long-name.bin");
    bar.update(0, 1000);
    bar.update(0, 1000); // unchanged: not redrawn
    const first = w.buffered().len;
    try testing.expectEqual('\r', w.buffered()[0]);

    bar.label = "x";
    bar.update(500, 1000);
    const second = w.buffered()[first..];
    try testing.expectEqual(first, second.len); // the shorter line is padded to the old width

    const width = bar.line_width;
    try testing.expect(width < first - 1);
    bar.finish();
    const erased = w.buffered()[first + second.len ..];
    try testing.expectEqual(0, bar.line_width);
    try testing.expectEqual(width + 2, erased.len); // "\r", spaces over the text, "\r"
    try testing.expectEqual('\r', erased[erased.len - 1]);
}

test "an empty body draws nothing" {
    var buffer: [64]u8 = undefined;
    var w: Writer = .fixed(&buffer);
    var bar: ProgressBar = .init(&w, "empty.txt");
    bar.update(0, 0);
    bar.finish();
    try testing.expectEqual(0, w.buffered().len);
}
