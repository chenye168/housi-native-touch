// regset: attempt to set IORegistry properties (displayUUID / Built-In) on the
// HOUSI digitizer HID event service node, then read them back.
// Build: clang -O2 -o regset regset.c -framework IOKit -framework CoreFoundation
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>

static void print_cf(const char* label, CFTypeRef v) {
    printf("%s = ", label);
    if (!v) { printf("(nil)\n"); return; }
    CFStringRef d = CFCopyDescription(v);
    if (d) {
        char buf[1024];
        if (CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8)) printf("%s\n", buf);
        else printf("<?>\n");
        CFRelease(d);
    } else printf("<?>\n");
}

static long get_int(io_registry_entry_t e, CFStringRef key, long def) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0);
    long r = def;
    if (v) {
        if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberLongType, &r);
        CFRelease(v);
    }
    return r;
}

int main(void) {
    CFMutableDictionaryRef match = IOServiceMatching("AppleUserHIDEventService");
    io_iterator_t it = 0;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &it);
    if (kr != KERN_SUCCESS) { printf("IOServiceGetMatchingServices kr=%d\n", kr); return 1; }

    io_registry_entry_t target = 0;
    io_object_t obj;
    while ((obj = IOIteratorNext(it))) {
        long page = get_int(obj, CFSTR("PrimaryUsagePage"), -1);
        long usage = get_int(obj, CFSTR("PrimaryUsage"), -1);
        CFTypeRef prod = IORegistryEntryCreateCFProperty(obj, CFSTR("Product"), kCFAllocatorDefault, 0);
        char prodstr[256] = "";
        if (prod && CFGetTypeID(prod) == CFStringGetTypeID())
            CFStringGetCString((CFStringRef)prod, prodstr, sizeof(prodstr), kCFStringEncodingUTF8);
        if (prod) CFRelease(prod);
        if (page == 0x0D && usage == 0x04) {
            printf("found digitizer service entry: product=%s page=%ld usage=%ld\n", prodstr, page, usage);
            target = obj;
            break;
        }
        IOObjectRelease(obj);
    }
    IOObjectRelease(it);
    if (!target) { printf("digitizer event service not found\n"); return 2; }

    printf("=== before ===\n");
    CFTypeRef v1 = IORegistryEntryCreateCFProperty(target, CFSTR("displayUUID"), kCFAllocatorDefault, 0);
    print_cf("displayUUID", v1); if (v1) CFRelease(v1);
    CFTypeRef v2 = IORegistryEntryCreateCFProperty(target, CFSTR("Built-In"), kCFAllocatorDefault, 0);
    print_cf("Built-In", v2); if (v2) CFRelease(v2);

    CFStringRef uuid = CFStringCreateWithCString(kCFAllocatorDefault,
        "30D985CD-BE3F-4C0E-B055-C5E22CFF2E07", kCFStringEncodingUTF8);
    printf("\n=== set displayUUID (CFString) via IORegistryEntrySetCFProperty ===\n");
    kern_return_t r = IORegistryEntrySetCFProperty(target, CFSTR("displayUUID"), uuid);
    printf("IORegistryEntrySetCFProperty kr=0x%x\n", r);
    CFTypeRef rb = IORegistryEntryCreateCFProperty(target, CFSTR("displayUUID"), kCFAllocatorDefault, 0);
    print_cf("read-back displayUUID", rb); if (rb) CFRelease(rb);

    printf("\n=== set Built-In = Yes ===\n");
    r = IORegistryEntrySetCFProperty(target, CFSTR("Built-In"), kCFBooleanTrue);
    printf("set Built-In kr=0x%x\n", r);
    CFTypeRef rb2 = IORegistryEntryCreateCFProperty(target, CFSTR("Built-In"), kCFAllocatorDefault, 0);
    print_cf("read-back Built-In", rb2); if (rb2) CFRelease(rb2);

    CFStringRef uuidKey = CFStringCreateWithCString(kCFAllocatorDefault, "displayUUID", kCFStringEncodingUTF8);
    printf("\n=== set via dictionary API (displayUUID key) ===\n");
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef dispUUIDName = CFStringCreateWithCString(kCFAllocatorDefault, "displayUUID", kCFStringEncodingUTF8);
    CFDictionarySetValue(dict, dispUUIDName, uuid);
    r = IORegistryEntrySetCFProperties(target, dict);
    printf("IORegistryEntrySetCFProperties kr=0x%x\n", r);
    CFTypeRef rb3 = IORegistryEntryCreateCFProperty(target, dispUUIDName, kCFAllocatorDefault, 0);
    print_cf("read-back displayUUID(2)", rb3); if (rb3) CFRelease(rb3);

    CFRelease(dispUUIDName);
    CFRelease(dict);
    CFRelease(uuidKey);
    CFRelease(uuid);
    IOObjectRelease(target);
    return 0;
}
