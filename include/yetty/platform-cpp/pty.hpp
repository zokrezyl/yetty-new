#pragma once

#include <cstddef>
#include <cstdint>
#include <yetty/core/result.hpp>
#include <yetty/platform/pty-poll-source.hpp>

namespace yetty {

// Pty - Pseudo-terminal abstraction
//
// Provides read/write/resize operations for a terminal session.
// pollSource() returns a platform-specific PtyPollSource that the
// EventLoop can monitor for data availability.
//
// Desktop: Direct PTY fd via forkpty() (fast, non-blocking)
// Windows: ConPTY
// Android: Telnet to Termux/toybox
// iOS: SSH to remote host
// WebAssembly: JSLinux iframe via postMessage
//
class Pty {
public:
  virtual ~Pty() = default;

  // Read up to maxLen bytes into buf.
  // Returns actual bytes read, 0 if no data available.
  // NON-BLOCKING - returns immediately if no data.
  virtual size_t read(char *buf, size_t maxLen) = 0;

  // Write data to PTY input.
  virtual void write(const char *data, size_t len) = 0;

  // Resize PTY dimensions.
  virtual void resize(uint32_t cols, uint32_t rows) = 0;

  // Stop the PTY/process.
  virtual void stop() = 0;

  // Platform-specific pollable source for EventLoop integration.
  // Ownership stays with the Pty.
  virtual PtyPollSource *pollSource() = 0;
};

} // namespace yetty
