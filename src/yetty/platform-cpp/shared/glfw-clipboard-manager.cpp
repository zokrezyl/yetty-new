#include <yetty/platform/clipboard-manager.hpp>

#include <GLFW/glfw3.h>

namespace yetty {

class ClipboardManagerImpl : public ClipboardManager {
public:
  const char *typeName() const override { return "ClipboardManager"; }

  std::string getText() const override {
    auto text = glfwGetClipboardString(nullptr);
    return text ? std::string(text) : std::string();
  }

  void setText(const std::string &text) override {
    glfwSetClipboardString(nullptr, text.c_str());
  }
};

Result<ClipboardManager*> ClipboardManager::createImpl() {
  return Ok(static_cast<ClipboardManager*>(new ClipboardManagerImpl()));
}

} // namespace yetty
