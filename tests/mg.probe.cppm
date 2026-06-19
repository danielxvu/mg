// Minimal C++20 module interface unit used only to verify the module toolchain
// (task F3). The real engine modules (mg.magit, mg.text, …) will follow this
// same shape: `export module <name>;` with an exported namespace.
export module mg.probe;

export namespace mg::probe {

// Resolved across the module boundary by tests/test_modules.cpp.
int answer()
{
    return 42;
}

} // namespace mg::probe
