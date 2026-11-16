// housitouch-setmode: user-context helper that puts the HOUSI digitizer into multi-touch mode (Input Mode=2)
// Requires Input Monitoring TCC grant for the enclosing app bundle.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>

static FILE *g_log;
static void logline(const char *fmt, ...) {
    time_t t = time(NULL); struct tm tmv; localtime_r(&t, &tmv);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    printf("%s %s\n", ts, buf);
    if (g_log) { fprintf(g_log, "%s %s\n", ts, buf); fflush(g_log); }
}

int main(void) {
    g_log = fopen("/tmp/housitouch-setmode.log", "a");
    logline("housitouch-setmode start pid=%d", getpid());
    IOHIDAccessType acc = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent);
    logline("Input Monitoring access=%d", (int)acc);
    if (acc != kIOHIDAccessTypeGranted) {
        IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
        logline("requested Input Monitoring access");
    }
    for (int attempt = 0; attempt < 20; attempt++) {
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
        CFRelease(nv); CFRelease(np); CFRelease(nu); CFRelease(ns);
        IOHIDManagerSetDeviceMatching(mgr, match);
        CFRelease(match);
        IOReturn kr = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
        if (kr == kIOReturnSuccess) {
            CFSetRef devs = IOHIDManagerCopyDevices(mgr);
            CFIndex n = devs ? CFSetGetCount(devs) : 0;
            if (n > 0) {
                const void *v = NULL;
                CFSetGetValues(devs, &v);
                IOHIDDeviceRef dev = (IOHIDDeviceRef)v;
                IOReturn okr = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);
                if (okr == kIOReturnSuccess) {
                    /* macOS quirk (verified with hidprobe): buffer must include Report ID as first byte */
                    uint8_t report[3] = {0x13, 0x02, 0x00};
                    IOReturn sr = IOHIDDeviceSetReport(dev, kIOHIDReportTypeFeature, 0x13, report, 3);
                    logline("attempt %d: IOHIDDeviceSetReport kr=0x%x", attempt, sr);
                    if (sr != kIOReturnSuccess) {
                        IOReturn sr2 = IOHIDDeviceSetReport(dev, kIOHIDReportTypeFeature, 0x13, report + 1, 2);
                        logline("attempt %d: retry data-only buffer kr=0x%x", attempt, sr2);
                        if (sr2 == kIOReturnSuccess) sr = sr2;
                    }
                    IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
                    IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
                    if (devs) CFRelease(devs);
                    CFRelease(mgr);
                    if (sr == kIOReturnSuccess) { logline("SETMODE OK mode=2"); fclose(g_log); return 0; }
                    usleep(500000);
                    continue;
                } else {
                    logline("attempt %d: IOHIDDeviceOpen kr=0x%x", attempt, okr);
                }
                if (devs) CFRelease(devs);
            } else {
                logline("attempt %d: no matching device", attempt);
            }
            IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
        } else {
            logline("attempt %d: IOHIDManagerOpen kr=0x%x (need Input Monitoring for this app)", attempt, kr);
        }
        CFRelease(mgr);
        usleep(500000);
    }
    logline("SETMODE FAILED — grant Input Monitoring to HOUSITouchHelper.app in System Settings > Privacy & Security > Input Monitoring");
    fclose(g_log);
    return 1;
}
