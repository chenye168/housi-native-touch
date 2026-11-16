// usbreset: try to reset the HOUSI USB touch device via IOUSBHost (objc_msgSend, no headers)
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <dlfcn.h>
#include <time.h>

static long get_int(io_registry_entry_t e, CFStringRef key, long def) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0);
    long r = def;
    if (v) { if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberLongType, &r); CFRelease(v); }
    return r;
}

typedef id (*init_fn)(id, SEL, io_service_t, dispatch_queue_t, NSError**);
typedef BOOL (*bool_err_fn)(id, SEL, NSError**);
typedef BOOL (*bool_opt_err_fn)(id, SEL, NSUInteger, NSError**);
typedef void (*void_opt_fn)(id, SEL, NSUInteger);
typedef void (*void_fn)(id, SEL);

int main(int argc, char** argv) {
    int doReset = 1;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--dry-run")) doReset = 0;

    void* h = dlopen("/System/Library/Frameworks/IOUSBHost.framework/Versions/A/IOUSBHost", RTLD_NOW);
    printf("IOUSBHost dlopen: %s\n", h ? "ok" : dlerror());

    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, m, &it) != KERN_SUCCESS) { printf("match fail\n"); return 1; }
    io_registry_entry_t target = 0;
    io_object_t o;
    while ((o = IOIteratorNext(it))) {
        long vid = get_int(o, CFSTR("idVendor"), -1);
        long pid = get_int(o, CFSTR("idProduct"), -1);
        if (vid == 0x32D7 && pid == 1) { target = o; break; }
        IOObjectRelease(o);
    }
    IOObjectRelease(it);
    if (!target) { printf("HOUSI USB device not found\n"); return 2; }
    io_name_t name; IORegistryEntryGetName(target, name);
    printf("found IOUSBHostDevice: %s\n", name);
    if (!doReset) { IOObjectRelease(target); return 0; }

    Class cls = NSClassFromString(@"IOUSBHostDevice");
    if (!cls) { printf("no IOUSBHostDevice class\n"); return 3; }
    NSError *err = nil;
    id dev = nil;
    @try {
        typedef id (*init2_fn)(id, SEL, io_service_t, NSUInteger, dispatch_queue_t, NSError**, id);
        dev = ((init2_fn)objc_msgSend)([cls alloc], sel_registerName("initWithIOService:options:queue:error:interestHandler:"),
                                       target, 0, dispatch_get_main_queue(), &err, nil);
    } @catch (NSException *ex) {
        printf("init exception: %s\n", ex.reason.UTF8String);
    }
    if (!dev) { printf("init failed: %s\n", err.localizedDescription.UTF8String ?: "nil"); return 4; }
    err = nil;
    BOOL opened = ((bool_opt_err_fn)objc_msgSend)(dev, sel_registerName("openWithOptions:error:"), 0, &err);
    printf("openWithOptions: %s err=%s\n", opened ? "YES" : "NO", err.localizedDescription.UTF8String ?: "nil");
    err = nil;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    BOOL ok = ((bool_err_fn)objc_msgSend)(dev, sel_registerName("resetWithError:"), &err);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("resetWithError: %s err=%s (%.1f ms)\n", ok ? "YES" : "NO", err.localizedDescription.UTF8String ?: "nil",
           (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    ((void_opt_fn)objc_msgSend)(dev, sel_registerName("closeWithOptions:"), 0);
    ((void_fn)objc_msgSend)(dev, sel_registerName("destroy"));
    IOObjectRelease(target);
    return 0;
}
