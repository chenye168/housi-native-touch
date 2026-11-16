// usbmode: read/set HOUSI HID digitizer Input Mode via USB control transfers (no IOHID TCC needed)
// GET_REPORT/SET_REPORT feature report 0x13 on interface 1 (digitizer)
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <dlfcn.h>
#include <IOKit/usb/AppleUSBDefinitions.h>

static long get_int(io_registry_entry_t e, CFStringRef key, long def) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0);
    long r = def;
    if (v) { if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberLongType, &r); CFRelease(v); }
    return r;
}

typedef id (*init_fn)(id, SEL, io_service_t, NSUInteger, dispatch_queue_t, NSError**, id);
typedef BOOL (*open_fn)(id, SEL, NSUInteger, NSError**);
typedef BOOL (*send_fn)(id, SEL, IOUSBDeviceRequest, NSMutableData*, NSUInteger*, NSError**);
typedef void (*void_opt_fn)(id, SEL, NSUInteger);
typedef void (*void_fn)(id, SEL);

static io_service_t find_usb_device(void) {
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, m, &it) != KERN_SUCCESS) return 0;
    io_service_t t = 0; io_object_t o;
    while ((o = IOIteratorNext(it))) {
        if (get_int(o, CFSTR("idVendor"), -1) == 0x32D7 && get_int(o, CFSTR("idProduct"), -1) == 1) { t = o; break; }
        IOObjectRelease(o);
    }
    IOObjectRelease(it);
    return t;
}

int main(int argc, char** argv) {
    int doSet = 0, targetMode = 2;
    int rawMode = 0; unsigned int rawWValue = 0x0313; const char *rawData = "02 00";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--set")) { doSet = 1; if (i+1 < argc) targetMode = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--raw")) { rawMode = 1; if (i+1 < argc) rawWValue = (unsigned)strtoul(argv[++i], NULL, 16); if (i+1 < argc) rawData = argv[++i]; }
    }
    if (!dlopen("/System/Library/Frameworks/IOUSBHost.framework/Versions/A/IOUSBHost", RTLD_NOW)) {
        printf("dlopen failed\n"); return 1;
    }
    io_service_t svc = find_usb_device();
    if (!svc) { printf("USB device not found\n"); return 2; }
    Class cls = NSClassFromString(@"IOUSBHostDevice");
    NSError *err = nil;
    id dev = ((init_fn)objc_msgSend)([cls alloc], sel_registerName("initWithIOService:options:queue:error:interestHandler:"),
                                     svc, 0, dispatch_get_main_queue(), &err, nil);
    if (!dev) { printf("init failed: %s\n", err.localizedDescription.UTF8String ?: "nil"); return 3; }
    err = nil;
    if (!((open_fn)objc_msgSend)(dev, sel_registerName("openWithOptions:error:"), 0, &err)) {
        printf("open failed: %s\n", err.localizedDescription.UTF8String ?: "nil"); return 4;
    }
    send_fn send = (send_fn)objc_msgSend;

    if (rawMode) {
        unsigned int bytes[16]; int n = 0;
        const char *p2 = rawData;
        while (*p2 && n < 16) {
            while (*p2 == ' ' || *p2 == ',') p2++;
            if (!*p2) break;
            bytes[n++] = (unsigned)strtoul(p2, (char**)&p2, 16);
        }
        NSMutableData *wd = [NSMutableData dataWithLength:n];
        uint8_t *wp = wd.mutableBytes;
        for (int i = 0; i < n; i++) wp[i] = (uint8_t)bytes[i];
        IOUSBDeviceRequest setReq = {0x21, 0x09, (uint16_t)rawWValue, 0x0001, (uint16_t)n};
        NSUInteger sent = 0; err = nil;
        BOOL sok = send(dev, sel_registerName("sendDeviceRequest:data:bytesTransferred:error:"), setReq, wd, &sent, &err);
        printf("RAW SET wValue=0x%04x len=%d ok=%d sent=%lu err=%s\n", rawWValue, n, sok, (unsigned long)sent, err.localizedDescription.UTF8String ?: "nil");
        ((void_opt_fn)objc_msgSend)(dev, sel_registerName("closeWithOptions:"), 0);
        ((void_fn)objc_msgSend)(dev, sel_registerName("destroy"));
        IOObjectRelease(svc);
        return 0;
    }

    // GET_REPORT feature 0x13, interface 1
    IOUSBDeviceRequest getReq = {0xA1, 0x01, 0x0313, 0x0001, 0x0002};
    NSMutableData *rd = [NSMutableData dataWithLength:2];
    NSUInteger got = 0; err = nil;
    BOOL ok = send(dev, sel_registerName("sendDeviceRequest:data:bytesTransferred:error:"), getReq, rd, &got, &err);
    const uint8_t *rb = rd.bytes;
    printf("GET_REPORT ok=%d got=%lu data=", ok, (unsigned long)got);
    for (NSUInteger i = 0; i < rd.length; i++) printf("%02x ", rb[i]);
    printf("err=%s\n", err.localizedDescription.UTF8String ?: "nil");
    int curMode = (ok && got >= 1) ? rb[0] : -1;
    printf("current Input Mode=%d\n", curMode);

    if (doSet && curMode != targetMode) {
        uint8_t payload[2] = {(uint8_t)targetMode, 0x00};
        NSMutableData *wd = [NSMutableData dataWithBytes:payload length:2];
        IOUSBDeviceRequest setReq = {0x21, 0x09, 0x0313, 0x0001, 0x0002};
        NSUInteger sent = 0; err = nil;
        BOOL sok = send(dev, sel_registerName("sendDeviceRequest:data:bytesTransferred:error:"), setReq, wd, &sent, &err);
        printf("SET_REPORT(mode=%d) ok=%d sent=%lu err=%s\n", targetMode, sok, (unsigned long)sent, err.localizedDescription.UTF8String ?: "nil");
        usleep(200000);
        NSMutableData *rd2 = [NSMutableData dataWithLength:2];
        NSUInteger got2 = 0; err = nil;
        BOOL ok2 = send(dev, sel_registerName("sendDeviceRequest:data:bytesTransferred:error:"), getReq, rd2, &got2, &err);
        printf("GET after set ok=%d data=%02x %02x err=%s\n", ok2, ((const uint8_t*)rd2.bytes)[0], ((const uint8_t*)rd2.bytes)[1], err.localizedDescription.UTF8String ?: "nil");
    }
    ((void_opt_fn)objc_msgSend)(dev, sel_registerName("closeWithOptions:"), 0);
    ((void_fn)objc_msgSend)(dev, sel_registerName("destroy"));
    IOObjectRelease(svc);
    return 0;
}
