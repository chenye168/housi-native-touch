// fastpath: test private IOKit IOHIDServiceClientFastPathSetProperty / related setters
// Build: clang -O2 -o fastpath fastpath.c -framework IOKit -framework CoreFoundation
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hidsystem/IOHIDEventSystemClient.h>
#include <IOKit/hidsystem/IOHIDServiceClient.h>
#include <dlfcn.h>
#include <stdio.h>

typedef Boolean (*set_fn)(IOHIDServiceClientRef, CFStringRef, CFTypeRef);

static IOHIDServiceClientRef find_digitizer(IOHIDEventSystemClientRef c) {
    CFArrayRef arr = IOHIDEventSystemClientCopyServices(c);
    if (!arr) return NULL;
    IOHIDServiceClientRef t = NULL;
    for (CFIndex i = 0; i < CFArrayGetCount(arr); i++) {
        IOHIDServiceClientRef s = (IOHIDServiceClientRef)CFArrayGetValueAtIndex(arr, i);
        CFTypeRef a = IOHIDServiceClientCopyProperty(s, CFSTR("PrimaryUsagePage"));
        CFTypeRef b = IOHIDServiceClientCopyProperty(s, CFSTR("PrimaryUsage"));
        long p = -1, u = -1;
        if (a) { CFNumberGetValue((CFNumberRef)a, kCFNumberLongType, &p); CFRelease(a); }
        if (b) { CFNumberGetValue((CFNumberRef)b, kCFNumberLongType, &u); CFRelease(b); }
        if (p == 0x0D && u == 0x04) { t = s; CFRetain(t); break; }
    }
    CFRelease(arr);
    return t;
}

int main(void) {
    printf("IOHIDServiceClientFastPathSetProperty = %p\n", dlsym(RTLD_DEFAULT, "IOHIDServiceClientFastPathSetProperty"));
    set_fn fastset = (set_fn)dlsym(RTLD_DEFAULT, "IOHIDServiceClientFastPathSetProperty");
    set_fn slowset = (set_fn)dlsym(RTLD_DEFAULT, "IOHIDServiceClientSetProperty");

    IOHIDEventSystemClientRef c = IOHIDEventSystemClientCreateSimpleClient(kCFAllocatorDefault);
    IOHIDServiceClientRef s = find_digitizer(c);
    if (!s) { printf("no digitizer service\n"); return 1; }

    CFStringRef uuid = CFStringCreateWithCString(NULL, "30D985CD-BE3F-4C0E-B055-C5E22CFF2E07", kCFStringEncodingUTF8);

    printf("\n-- slow set (public) --\n");
    printf("ret=%d\n", slowset ? slowset(s, CFSTR("displayUUID"), uuid) : -1);
    CFTypeRef v = IOHIDServiceClientCopyProperty(s, CFSTR("displayUUID"));
    printf("copy after slow: %s\n", v ? "non-nil" : "nil");
    if (v) CFRelease(v);

    printf("\n-- fast set (private) --\n");
    printf("fastset ptr=%p\n", (void*)fastset);
    if (fastset) {
        printf("ret=%d\n", fastset(s, CFSTR("displayUUID"), uuid));
        CFTypeRef v2 = IOHIDServiceClientCopyProperty(s, CFSTR("displayUUID"));
        printf("copy after fast: %s\n", v2 ? "non-nil" : "nil");
        if (v2) CFRelease(v2);
    }

    // also try setting Built-In
    printf("\n-- set Built-In true via slow --\n");
    printf("ret=%d\n", slowset ? slowset(s, CFSTR("Built-In"), kCFBooleanTrue) : -1);
    CFTypeRef v3 = IOHIDServiceClientCopyProperty(s, CFSTR("Built-In"));
    printf("copy Built-In: %s\n", v3 ? "non-nil" : "nil");
    if (v3) CFRelease(v3);

    CFRelease(uuid);
    CFRelease(s);
    CFRelease(c);
    return 0;
}
