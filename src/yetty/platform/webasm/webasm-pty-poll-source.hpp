#pragma once

#include <yetty/platform/pty-poll-source.hpp>
#include <functional>

namespace yetty {

// Callback type for notifying EventLoop
using PtyNotifyCallback = std::function<void()>;

// WebasmPtyPollSource - notification only, no buffer (like FdPtyPollSource holds fd only)
// EventLoop casts PtyPollSource* to this type and calls setNotifyCallback()
class WebasmPtyPollSource : public PtyPollSource {
public:
    void setNotifyCallback(PtyNotifyCallback callback) {
        _notifyCallback = std::move(callback);
    }

    void notify() {
        if (_notifyCallback) {
            _notifyCallback();
        }
    }

private:
    PtyNotifyCallback _notifyCallback;
};

} // namespace yetty
