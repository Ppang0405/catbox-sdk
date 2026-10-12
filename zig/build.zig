const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // The library. `addModule` (rather than `createModule`) makes it visible to
    // projects that depend on this package: `dependency.module("catbox")`.
    const catbox = b.addModule("catbox", .{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });

    // The command-line tool, a thin layer over the library.
    const exe = b.addExecutable(.{
        .name = "catbox",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/cli/main.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "catbox", .module = catbox },
            },
        }),
    });
    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    run_cmd.addPassthruArgs();
    const run_step = b.step("run", "Run the catbox command");
    run_step.dependOn(&run_cmd.step);

    const library_tests = b.addRunArtifact(b.addTest(.{ .root_module = catbox }));
    const cli_tests = b.addRunArtifact(b.addTest(.{ .root_module = exe.root_module }));
    const test_step = b.step("test", "Run the unit tests");
    test_step.dependOn(&library_tests.step);
    test_step.dependOn(&cli_tests.step);
}
