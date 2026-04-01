#pragma once

namespace yetty {

// PtyPollSource - opaque platform-specific pollable handle for PTY I/O
//
// Returned by Pty::pollSource(), passed to EventLoop::createPoll().
// The platform-specific EventLoop static_casts to the concrete type
// it knows (FdPtyPollSource on Unix, WebasmPtyPollSource on webasm, etc).
//
// Unix (FdPtyPollSource): wraps the pty master fd
// Webasm (WebasmPtyPollSource): holds ptyId + receive buffer, JS interop pushes data
// Windows (WinPtyPollSource): wraps a HANDLE
//
class PtyPollSource {
public:
  virtual ~PtyPollSource() = default;
};

} // namespace yetty
