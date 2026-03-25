#pragma once

namespace yetty {

class Config;
class PtyFactory;
class ClipboardManager;

namespace core {
class EventLoop;
class EventQueue;
} // namespace core

// Application-level context created by platform main() and passed to yetty::create.
// Raw pointers — the app owns these objects and outlives all children.
struct AppContext {
  Config *config = nullptr;
  core::EventLoop *eventLoop = nullptr;
  core::EventQueue *eventQueue = nullptr;
  ClipboardManager *clipboardManager = nullptr;
  PtyFactory *ptyFactory = nullptr;
};

} // namespace yetty
