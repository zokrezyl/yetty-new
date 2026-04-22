# QEMU Build Modes: JIT vs Interpreter

QEMU uses TCG (Tiny Code Generator) for dynamic binary translation. Two modes exist:

## JIT Mode (--enable-tcg)
- Compiles guest code to host native code at runtime
- Fast execution (~native speed for simple code)
- Requires writable+executable memory (W^X violation)
- **Not allowed on iOS** due to code signing restrictions

## Interpreter Mode (--enable-tcg-interpreter)
- Interprets TCG intermediate representation
- No JIT compilation, no W^X issues
- **~20x slower** than JIT mode
- **Required for iOS** (both device and simulator)

## Performance Comparison (RISC-V guest, 4 threads, 20M iterations)

| Mode | Time |
|------|------|
| JIT | 69 ms |
| Interpreter | 1326 ms |

## iOS Restrictions

iOS enforces W^X (Write XOR Execute) - memory pages cannot be both writable and executable simultaneously. This blocks JIT compilation.

Apple provides `pthread_jit_write_protect_np()` for toggling W^X on entitled apps, but this requires:
- `com.apple.security.cs.allow-jit` entitlement
- Distribution through App Store or Enterprise program
- BrowserEngineKit for third-party browser engines

For general iOS apps, interpreter mode is the only option.

## Build Scripts

- `build-ios-sim.sh` - iOS Simulator x86_64, JIT mode (for macOS testing only)
- `build-ios-sim-interp.sh` - iOS Simulator x86_64, interpreter mode
- `build-ios.sh` - iOS device arm64, JIT mode (won't work without entitlements)
- `build-macos-minimal.sh` - macOS native, interpreter mode, minimal size
