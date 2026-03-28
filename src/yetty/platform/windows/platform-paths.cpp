// Windows platform paths - AppData directories

#include <cstdlib>
#include <string>

std::string getCacheDir() {
    if (const char* localAppData = std::getenv("LOCALAPPDATA")) {
        return std::string(localAppData) + "\\yetty\\cache";
    }
    return "C:\\temp\\yetty";
}

std::string getRuntimeDir() {
    if (const char* temp = std::getenv("TEMP")) {
        return std::string(temp) + "\\yetty";
    }
    return "C:\\temp\\yetty";
}

std::string getConfigDir() {
    if (const char* appData = std::getenv("APPDATA")) {
        return std::string(appData) + "\\yetty";
    }
    return "C:\\temp\\yetty";
}
