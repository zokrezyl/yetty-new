#pragma once

#include <yetty/core/factory-object.hpp>
#include <yetty/platform/pty.hpp>

namespace yetty {

class Config;

// PtyFactory - Creates platform-appropriate PTY instances
//
// create(Config*, void*) creates the factory with config and platform-specific
// context (e.g., JS interop on WebAssembly). createPty() then creates individual
// Pty instances using the stored config.
//
// Platform implementations provide createImpl():
// - linux/unix-pty.cpp: forkpty
// - macos/unix-pty.cpp: forkpty
// - windows/pty-io.cpp: ConPTY
// - android/pty-io.cpp: Telnet to Termux/toybox
// - ios/pty-io.cpp: SSH
// - webasm/pty-io.cpp: JSLinux iframe
//
class PtyFactory : public core::FactoryObject<PtyFactory> {
public:
  static Result<PtyFactory *> createImpl(Config *config,
                                         void *osSpecific = nullptr);

  virtual ~PtyFactory() = default;

  virtual Result<Pty *> createPty() = 0;
};

} // namespace yetty
