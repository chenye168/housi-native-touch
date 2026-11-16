// hidprop: enumerate IOHIDEventSystemClient services, inspect HOUSI digitizer,
// attempt to set "displayUUID" with several types and read it back.
// Build: clang -O2 -o hidprop hidprop.c -framework IOKit -framework CoreFoundation
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hidsystem/IOHIDEventSystemClient.h>
#include <IOKit/hidsystem/IOHIDServiceClient.h>
#include <stdio.h>
#include <string.h>

static void print_s(CFStringRef s) {
    if (!s) { printf("(null)"); return; }
    char buf[512];
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) printf("%s", buf);
    else printf("<?>");
}

static void dump_props(IOHIDServiceClientRef svc) {
    const char* keys[] = {"Product", "Manufacturer", "Transport", "PrimaryUsagePage",
                          "PrimaryUsage", "DisplayIntegrated", "BuiltIn", "displayUUID",
                          "DeviceUsagePairs", "HIDServiceSupport", "VendorID", "ProductID"};
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        CFStringRef k = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
        CFTypeRef v = IOHIDServiceClientCopyProperty(svc, k);
        printf("    %-20s = ", keys[i]);
        if (!v) printf("(nil)\n");
        else if (CFGetTypeID(v) == CFStringGetTypeID()) { print_s((CFStringRef)v); printf("\n"); }
        else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
            long n = 0; CFNumberGetValue((CFNumberRef)v, kCFNumberLongType, &n); printf("%ld\n", n);
        } else { print_s(CFCopyDescription(v)); printf("\n"); }
        if (v) CFRelease(v);
        CFRelease(k);
    }
}

int main(void) {
    IOHIDEventSystemClientRef client = IOHIDEventSystemClientCreateSimpleClient(kCFAllocatorDefault);
    if (!client) { printf("IOHIDEventSystemClientCreateSimpleClient failed\n"); return 1; }
    CFArrayRef services = IOHIDEventSystemClientCopyServices(client);
    if (!services) { printf("CopyServices failed\n"); return 1; }
    CFIndex n = CFArrayGetCount(services);
    printf("services: %ld\n", (long)n);

    IOHIDServiceClientRef target = NULL;
    for (CFIndex i = 0; i < n; i++) {
        IOHIDServiceClientRef svc = (IOHIDServiceClientRef)CFArrayGetValueAtIndex(services, i);
        CFTypeRef pup = IOHIDServiceClientCopyProperty(svc, CFSTR("PrimaryUsagePage"));
        CFTypeRef pu = IOHIDServiceClientCopyProperty(svc, CFSTR("PrimaryUsage"));
        long usagePage = 0, usage = 0;
        if (pup && CFGetTypeID(pup) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)pup, kCFNumberLongType, &usagePage);
        if (pu && CFGetTypeID(pu) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)pu, kCFNumberLongType, &usage);
        if (pup) CFRelease(pup);
        if (pu) CFRelease(pu);
        if (usagePage == 0x0D && usage == 0x04) { target = svc; break; }
    }
    if (!target) { printf("HOUSI digitizer service not found\n"); return 2; }

    printf("=== digitizer service properties (before) ===\n");
    dump_props(target);

    const char* uuidStr = "30D985CD-BE3F-4C0E-B055-C5E22CFF2E07";
    CFStringRef uuidString = CFStringCreateWithCString(NULL, uuidStr, kCFStringEncodingUTF8);
    CFUUIDRef uuid = CFUUIDCreateFromString(NULL, uuidString);
    CFDataRef uuidData = NULL;
    if (uuid) {
        CFUUIDBytes bytes = CFUUIDGetUUIDBytes(uuid);
        uuidData = CFDataCreate(NULL, (const UInt8*)&bytes, sizeof(bytes));
    }

    struct { const char* label; CFTypeRef value; } attempts[] = {
        {"CFString", uuidString},
        {"CFUUID", uuid},
        {"CFData16", uuidData},
    };
    for (size_t i = 0; i < sizeof(attempts)/sizeof(attempts[0]); i++) {
        if (!attempts[i].value) continue;
        printf("\n=== set displayUUID as %s ===\n", attempts[i].label);
        Boolean ok = IOHIDServiceClientSetProperty(target, CFSTR("displayUUID"), attempts[i].value);
        printf("SetProperty returned: %s\n", ok ? "true" : "false");
        CFTypeRef v = IOHIDServiceClientCopyProperty(target, CFSTR("displayUUID"));
        printf("CopyProperty after set: ");
        if (!v) printf("(nil)\n");
        else { print_s(CFCopyDescription(v)); printf("\n"); CFRelease(v); }
    }

    printf("\n=== also try HIDEventServiceProperties dict merge ===\n");
    CFTypeRef hprop = IOHIDServiceClientCopyProperty(target, CFSTR("HIDEventServiceProperties"));
    printf("HIDEventServiceProperties = "); print_s(hprop ? CFCopyDescription(hprop) : NULL); printf("\n");
    if (hprop) CFRelease(hprop);

    if (uuidData) CFRelease(uuidData);
    if (uuid) CFRelease(uuid);
    CFRelease(uuidString);
    CFRelease(services);
    CFRelease(client);
    return 0;
}
