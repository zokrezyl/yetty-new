// Linux platform paths - XDG directories

#include <cstdlib>
#include <string>
#include <unistd.h>

std::string getCacheDir() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME")) {
        return std::string(xdg) + "/yetty";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::string(home) + "/.cache/yetty";
    }
    return "/tmp/yetty";
}

std::string getRuntimeDir() {
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR")) {
        return std::string(xdg) + "/yetty";
    }
    return "/tmp/yetty-" + std::to_string(getuid());
}

std::string getConfigDir() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        return std::string(xdg) + "/yetty";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::string(home) + "/.config/yetty";
    }
    return "/tmp/yetty";
}
