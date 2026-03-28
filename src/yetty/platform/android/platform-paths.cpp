// Android platform paths - app internal storage

#include <string>

// Android paths are provided by the app at runtime via JNI
// These are fallbacks - the actual paths come from Context.getCacheDir() etc.

static std::string g_cacheDir = "/data/local/tmp/yetty";
static std::string g_configDir = "/data/local/tmp/yetty";

void setPlatformPaths(const char* cacheDir, const char* configDir) {
    if (cacheDir) g_cacheDir = cacheDir;
    if (configDir) g_configDir = configDir;
}

std::string getCacheDir() {
    return g_cacheDir;
}

std::string getRuntimeDir() {
    return g_cacheDir;  // Android uses cache dir for runtime
}

std::string getConfigDir() {
    return g_configDir;
}
