#pragma once

#include <yetty/core/factory-object.hpp>
#include <yetty/platform/pty.hpp>

namespace yetty {

// Forward declaration
class Config;

// PtyFactory - Creates platform-appropriate PTY instances
//
// Platform implementations:
// - linux/pty-io.cpp: forkpty
// - macos/pty-io.cpp: forkpty
// - windows/pty-io.cpp: ConPTY
// - android/pty-io.cpp: Telnet to Termux/toybox
// - ios/pty-io.cpp: SSH
// - webasm/pty-io.cpp: JSLinux iframe
//
class PtyFactory : public core::FactoryObject<PtyFactory> {
public:
  using Ptr = std::shared_ptr<PtyFactory>;

  static Result<Ptr> createImpl();

  virtual ~PtyFactory() = default;

  // Create a Pty instance.
  // osSpecific: platform-specific context (e.g., JS interop on WebAssembly)
  virtual Result<Pty::Ptr> create(Config *config,
                                  void *osSpecific = nullptr) = 0;
};

} // namespace yetty
