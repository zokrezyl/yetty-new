// iOS platform paths - app container directories

#import <Foundation/Foundation.h>
#include <string>

std::string getCacheDir() {
    NSArray* paths = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    if (paths.count > 0) {
        return std::string([paths[0] UTF8String]) + "/yetty";
    }
    return "/tmp/yetty";
}

std::string getRuntimeDir() {
    // iOS uses cache dir for runtime files
    return getCacheDir();
}

std::string getConfigDir() {
    NSArray* paths = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    if (paths.count > 0) {
        return std::string([paths[0] UTF8String]) + "/yetty";
    }
    return "/tmp/yetty";
}
