// macOS platform paths - ~/Library/Application Support, ~/Library/Caches

#include <cstdlib>
#include <string>
#include <unistd.h>

std::string getCacheDir() {
    if (const char* home = std::getenv("HOME")) {
        return std::string(home) + "/Library/Caches/yetty";
    }
    return "/tmp/yetty";
}

std::string getRuntimeDir() {
    // macOS doesn't have XDG_RUNTIME_DIR, use tmp with uid
    return "/tmp/yetty-" + std::to_string(getuid());
}

std::string getConfigDir() {
    if (const char* home = std::getenv("HOME")) {
        return std::string(home) + "/Library/Application Support/yetty";
    }
    return "/tmp/yetty";
}
