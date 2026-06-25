const std = @import("std");
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const exe = b.addExecutable(.{
        .name = "zigstatus",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .link_libc = true, // std.Threaded needs libc on macOS
        }),
    });
    b.installArtifact(exe);

    const lib = b.addLibrary(.{
        .name = "neomg_zig",
        .linkage = .static,
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/lib.zig"),
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            // Emit PIC so the static .a links into the mg_magit PIE on musl
            // targets (Alpine). Without it the non-PIC relocations into .rodata
            // crash the musl loader at startup. Harmless on glibc/macOS.
            .pic = true,
        }),
    });
    b.installArtifact(lib);

    // `zig build test` -- unit tests for the byte-level index parser, which we
    // exercise on hand-crafted (incl. hostile/malformed) bytes that real git
    // tools won't produce, complementing the end-to-end test/validate.py suite.
    const unit_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/index.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    const run_unit_tests = b.addRunArtifact(unit_tests);
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_unit_tests.step);
}
