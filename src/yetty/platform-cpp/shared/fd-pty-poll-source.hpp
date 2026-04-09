#pragma once

#include <yetty/platform/pty-poll-source.hpp>

namespace yetty {

// Unix FdPtyPollSource — wraps the pty master fd
// Created by UnixPty, consumed by libuv EventLoop via static_cast
class FdPtyPollSource : public PtyPollSource {
public:
  explicit FdPtyPollSource(int fd = -1) : _fd(fd) {}
  int fd() const { return _fd; }

private:
  int _fd;
};

} // namespace yetty
