// svcmode: set Input Mode (0x0D/0x52) of HOUSI digitizer via private
// IOHIDServiceClientSetElementValue(service, usagePage, usage, value)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hidsystem/IOHIDEventSystemClient.h>
#include <IOKit/hidsystem/IOHIDServiceClient.h>
#include <dlfcn.h>
#include <stdio.h>

typedef void (*set_elem_fn)(IOHIDServiceClientRef, uint32_t, uint32_t, uint32_t);

static IOHIDServiceClientRef find_digitizer(IOHIDEventSystemClientRef c) {
    CFArrayRef arr = IOHIDEventSystemClientCopyServices(c);
    if (!arr) return NULL;
    IOHIDServiceClientRef t = NULL;
    for (CFIndex i = 0; i < CFArrayGetCount(arr); i++) {
        IOHIDServiceClientRef s = (IOHIDServiceClientRef)CFArrayGetValueAtIndex(arr, i);
        if (IOHIDServiceClientConformsTo(s, 0x0D, 0x04)) { t = s; CFRetain(t); break; }
    }
    CFRelease(arr);
    return t;
}

int main(int argc, char** argv) {
    uint32_t mode = (argc > 1) ? (uint32_t)atoi(argv[1]) : 2;
    void *h = dlopen("/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit", RTLD_NOW);
    if (!h) h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
    set_elem_fn setv = h ? (set_elem_fn)dlsym(h, "IOHIDServiceClientSetElementValue") : NULL;
    printf("dlsym setElementValue=%p\n", (void*)setv);
    IOHIDEventSystemClientRef c = IOHIDEventSystemClientCreateSimpleClient(kCFAllocatorDefault);
    if (!c) { printf("client failed\n"); return 1; }
    IOHIDServiceClientRef s = find_digitizer(c);
    if (!s) { printf("digitizer not found\n"); return 2; }
    for (uint32_t attempt = 0; attempt < 2; attempt++) {
        if (!setv) break;
        setv(s, 0x0D, 0x52, mode);
        printf("set element 0D/52 -> %u (attempt %u)\n", mode, attempt);
        usleep(200000);
    }
    CFRelease(s); CFRelease(c);
    return 0;
}
