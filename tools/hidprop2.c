// hidprop2: test whether "displayUUID" set via IOHIDServiceClientSetProperty persists
// across client instances, and inspect surrounding HID service property behavior.
// Build: clang -O2 -o hidprop2 hidprop2.c -framework IOKit -framework CoreFoundation
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hidsystem/IOHIDEventSystemClient.h>
#include <IOKit/hidsystem/IOHIDServiceClient.h>
#include <stdio.h>
#include <unistd.h>

static void print_s(CFTypeRef v) {
    if (!v) { printf("(nil)"); return; }
    CFStringRef d = CFCopyDescription(v);
    if (d) {
        char buf[1024];
        if (CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8)) printf("%s", buf);
        else printf("<?>");
        CFRelease(d);
    }
}

static IOHIDServiceClientRef find_digitizer(IOHIDEventSystemClientRef client) {
    CFArrayRef services = IOHIDEventSystemClientCopyServices(client);
    if (!services) return NULL;
    IOHIDServiceClientRef target = NULL;
    CFIndex n = CFArrayGetCount(services);
    for (CFIndex i = 0; i < n; i++) {
        IOHIDServiceClientRef svc = (IOHIDServiceClientRef)CFArrayGetValueAtIndex(services, i);
        CFTypeRef pup = IOHIDServiceClientCopyProperty(svc, CFSTR("PrimaryUsagePage"));
        CFTypeRef pu = IOHIDServiceClientCopyProperty(svc, CFSTR("PrimaryUsage"));
        long usagePage = 0, usage = 0;
        if (pup && CFGetTypeID(pup) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)pup, kCFNumberLongType, &usagePage);
        if (pu && CFGetTypeID(pu) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)pu, kCFNumberLongType, &usage);
        if (pup) CFRelease(pup);
        if (pu) CFRelease(pu);
        if (usagePage == 0x0D && usage == 0x04) { target = svc; CFRetain(target); break; }
    }
    CFRelease(services);
    return target;
}

int main(void) {
    IOHIDEventSystemClientRef c1 = IOHIDEventSystemClientCreateSimpleClient(kCFAllocatorDefault);
    if (!c1) { printf("client1 failed\n"); return 1; }
    IOHIDServiceClientRef svc1 = find_digitizer(c1);
    if (!svc1) { printf("digitizer not found (client1)\n"); return 2; }
    printf("== client1 service ==\n");
    printf("displayUUID before: "); print_s(IOHIDServiceClientCopyProperty(svc1, CFSTR("displayUUID"))); printf("\n");
    printf("RegistryID: "); print_s(IOHIDServiceClientCopyProperty(svc1, CFSTR("IORegistryEntryID"))); printf("\n");
    printf("BuiltIn: "); print_s(IOHIDServiceClientCopyProperty(svc1, CFSTR("BuiltIn"))); printf("\n");
    printf("DisplayIntegrated: "); print_s(IOHIDServiceClientCopyProperty(svc1, CFSTR("DisplayIntegrated"))); printf("\n");

    CFStringRef uuidStr = CFStringCreateWithCString(NULL, "30D985CD-BE3F-4C0E-B055-C5E22CFF2E07", kCFStringEncodingUTF8);
    CFUUIDRef uuid = CFUUIDCreateFromString(NULL, uuidStr);
    printf("\nset displayUUID (CFUUID) on client1: %s\n",
           IOHIDServiceClientSetProperty(svc1, CFSTR("displayUUID"), uuid) ? "true" : "false");
    printf("read back on client1: "); print_s(IOHIDServiceClientCopyProperty(svc1, CFSTR("displayUUID"))); printf("\n");

    CFRelease(svc1);
    CFRelease(c1);

    sleep(1);

    printf("\n== new client2, same service ==\n");
    IOHIDEventSystemClientRef c2 = IOHIDEventSystemClientCreateSimpleClient(kCFAllocatorDefault);
    IOHIDServiceClientRef svc2 = find_digitizer(c2);
    if (!svc2) { printf("digitizer not found (client2)\n"); return 3; }
    printf("displayUUID on client2: "); print_s(IOHIDServiceClientCopyProperty(svc2, CFSTR("displayUUID"))); printf("\n");

    // also try IORegistryEntryID lookup path used by other APIs
    printf("\nset again from client2: %s\n",
           IOHIDServiceClientSetProperty(svc2, CFSTR("displayUUID"), uuid) ? "true" : "false");
    printf("read back on client2: "); print_s(IOHIDServiceClientCopyProperty(svc2, CFSTR("displayUUID"))); printf("\n");

    // Try setting on the service's event system client level too
    printf("\nIOHIDEventSystemClientSetProperty displayUUID: %s\n",
           IOHIDEventSystemClientSetProperty(c2, CFSTR("displayUUID"), uuid) ? "true" : "false");

    CFRelease(svc2);
    CFRelease(c2);
    CFRelease(uuid);
    CFRelease(uuidStr);
    return 0;
}
