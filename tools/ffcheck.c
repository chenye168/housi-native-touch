// ffcheck: query libsystem_featureflags state for a domain/flag pair.
// usage: ffcheck [domain flag] (defaults to backboardd SupportExternalTouchscreen)
#include <dlfcn.h>
#include <stdio.h>

typedef int (*ff_fn)(const char* domain, const char* feature);

int main(int argc, char** argv) {
    const char* domain = (argc > 2) ? argv[1] : "backboardd";
    const char* flag   = (argc > 2) ? argv[2] : "SupportExternalTouchscreen";

    const char* paths[] = {
        "/usr/lib/system/libsystem_featureflags.dylib",
        "libsystem_featureflags.dylib",
        "/usr/lib/libsystem_featureflags.dylib",
    };
    void* h = NULL;
    for (size_t i = 0; i < sizeof(paths)/sizeof(paths[0]) && !h; i++)
        h = dlopen(paths[i], RTLD_NOW);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }

    ff_fn fn = (ff_fn)dlsym(h, "_os_feature_enabled_impl");
    if (!fn) { printf("dlsym failed: %s\n", dlerror()); return 1; }

    printf("%s/%s = %d\n", domain, flag, fn(domain, flag));

    // sanity checks against known flags
    printf("WindowServer/DirectTouchEventType = %d\n", fn("WindowServer", "DirectTouchEventType"));
    printf("UIKit/suppress_inauthentic_touches = %d\n", fn("UIKit", "suppress_inauthentic_touches"));
    printf("Accessibility/HoverTextTouch = %d\n", fn("Accessibility", "HoverTextTouch"));
    return 0;
}
