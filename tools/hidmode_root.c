// Test whether a root process can open the digitizer via IOHIDManager with restricted matching
// (no Input Monitoring TCC if digitizer-only matching is allowed).
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <stdio.h>

int main(void) {
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int vid = 0x32D7, pid = 1, up = 0x0D, us = 0x04;
    CFNumberRef nv = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
    CFNumberRef np = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
    CFNumberRef nu = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &up);
    CFNumberRef ns = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &us);
    CFDictionarySetValue(match, CFSTR("VendorID"), nv);
    CFDictionarySetValue(match, CFSTR("ProductID"), np);
    CFDictionarySetValue(match, CFSTR("PrimaryUsagePage"), nu);
    CFDictionarySetValue(match, CFSTR("PrimaryUsage"), ns);
    IOHIDManagerSetDeviceMatching(mgr, match);
    IOReturn kr = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    printf("IOHIDManagerOpen kr=0x%x\n", kr);
    if (kr != kIOReturnSuccess) return 1;
    CFSetRef devs = IOHIDManagerCopyDevices(mgr);
    CFIndex n = devs ? CFSetGetCount(devs) : 0;
    printf("matched devices=%ld\n", (long)n);
    if (n > 0) {
        const void *v = NULL;
        CFSetGetValues(devs, &v);
        IOHIDDeviceRef dev = (IOHIDDeviceRef)v;
        IOReturn okr = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);
        printf("IOHIDDeviceOpen kr=0x%x\n", okr);
        if (okr == kIOReturnSuccess) {
            uint8_t report[3] = {0x13, 0x02, 0x00};
            CFIndex len = sizeof(report);
            IOReturn sr = IOHIDDeviceSetReport(dev, kIOHIDReportTypeFeature, 0x13, report, 2);
            printf("IOHIDDeviceSetReport kr=0x%x\n", sr);
            uint8_t buf[8] = {0}; CFIndex blen = sizeof(buf);
            IOReturn gr = IOHIDDeviceGetReport(dev, kIOHIDReportTypeFeature, 0x13, buf, &blen);
            printf("IOHIDDeviceGetReport kr=0x%x len=%ld bytes=%02x %02x %02x\n", gr, (long)blen, buf[0], buf[1], buf[2]);
            IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
        }
    }
    return 0;
}
