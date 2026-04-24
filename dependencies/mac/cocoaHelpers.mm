#ifdef __APPLE__

#import <Foundation/Foundation.h>
#include <string.h>
#include "cocoaHelpers.hpp"

const char* getResourcesPath(void)
{
    static char cached[1024] = {0};
    if (cached[0] == 0) {
        @autoreleasepool
        {
            NSArray *paths = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
            NSString *applicationSupportDirectory = [paths firstObject];
            const char *s = [applicationSupportDirectory UTF8String];
            if (s) strncpy(cached, s, sizeof(cached) - 1);
        }
    }
    return cached;
}
#endif