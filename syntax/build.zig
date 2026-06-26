const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    // Plain standardOptimizeOption (no preferred_optimize_mode) so the
    // `-Doptimize` flag stays exposed -- the shared cmake helper
    // (cmake/AddZigLibrary.cmake) drives every neomg Zig lib with
    // `zig build -Doptimize=ReleaseFast` uniformly.
    const optimize = b.standardOptimizeOption(.{});

    // pic = true: required when the static library is linked into a PIE
    // executable (the default on hardened Linux distros such as Alpine/musl).
    // Without PIC the linker emits R_AARCH64_RELATIVE relocations that point
    // into the read-only .rodata segment; musl's dynamic linker tries to patch
    // them at load time, hits PROT_READ and segfaults before main() runs.
    // Setting pic here is safe on all platforms (macOS ignores it for MachO).
    const mod = b.createModule(.{
        .root_source_file = b.path("neomg_syntax.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const lib = b.addLibrary(.{
        .name = "neomg_syntax",
        .root_module = mod,
        .linkage = .static,
    });
    b.installArtifact(lib); // -> zig-out/lib/libneomg_syntax.a

    const tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("test_syntax.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Run the tokenizer tests");
    test_step.dependOn(&run_tests.step);
}
