#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <yetty/core/result.hpp>

namespace yetty {

/**
 * Pty - Pseudo-terminal abstraction
 *
 * Provides read/write/resize operations for a terminal session.
 *
 * Desktop: Direct PTY fd via forkpty() (fast, non-blocking)
 * Windows: ConPTY
 * Android: Telnet to Termux/toybox
 * iOS: SSH to remote host
 * WebAssembly: JSLinux iframe via postMessage
 */
class Pty {
public:
  using Ptr = std::shared_ptr<Pty>;
  using DataAvailableCallback = std::function<void()>;
  using ExitCallback = std::function<void(int exitCode)>;

  virtual ~Pty() = default;

  /**
   * Read up to maxLen bytes into buf.
   * Returns actual bytes read, 0 if no data available.
   * NON-BLOCKING - returns immediately if no data.
   */
  virtual size_t read(char *buf, size_t maxLen) = 0;

  /**
   * Write data to PTY input.
   */
  virtual void write(const char *data, size_t len) = 0;

  /**
   * Resize PTY dimensions.
   */
  virtual void resize(uint32_t cols, uint32_t rows) = 0;

  /**
   * Check if PTY/process still running.
   */
  virtual bool isRunning() const = 0;

  /**
   * Stop the PTY/process.
   */
  virtual void stop() = 0;

  /**
   * Set callback for when data becomes available.
   */
  virtual void setDataAvailableCallback(DataAvailableCallback cb) = 0;

  /**
   * Set callback for when process exits.
   */
  virtual void setExitCallback(ExitCallback cb) = 0;

  /**
   * Start async operations (e.g., SSH connect).
   * Called after init() when event loop is about to start.
   */
  virtual Result<void> run() { return Ok(); }
};

} // namespace yetty
