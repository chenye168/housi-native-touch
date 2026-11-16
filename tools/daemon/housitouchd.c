// housitouchd v2 — instantly inject displayUUID into HOUSI digitizer HID event service
// Uses IOKit matching notification (not polling) so injection happens as early as possible
// after the service is published, aiming to beat WindowServer's attach/read.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <pthread.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <errno.h>

extern char **environ;

static const char *kTargetUUID = "30D985CD-BE3F-4C0E-B055-C5E22CFF2E07";
static const char *kKey = "displayUUID";

static void logline(const char *fmt, ...) {
    time_t t = time(NULL);
    struct tm tmv; localtime_r(&t, &tmv);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    printf("%s ", ts);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n"); fflush(stdout);
}

static long get_int(io_registry_entry_t e, CFStringRef key, long def) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0);
    long r = def;
    if (v) { if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberLongType, &r); CFRelease(v); }
    return r;
}

static int is_digitizer(io_registry_entry_t e) {
    return get_int(e, CFSTR("PrimaryUsagePage"), -1) == 0x0D && get_int(e, CFSTR("PrimaryUsage"), -1) == 0x04;
}

static void inject_now(io_registry_entry_t svc) {
    uint64_t rid = 0;
    IORegistryEntryGetRegistryEntryID(svc, &rid);
    CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, kKey, kCFStringEncodingUTF8);
    CFStringRef want = CFStringCreateWithCString(kCFAllocatorDefault, kTargetUUID, kCFStringEncodingUTF8);
    kern_return_t kr = IORegistryEntrySetCFProperty(svc, key, want);
    logline("inject displayUUID rid=0x%llx kr=0x%x", (unsigned long long)rid, kr);
    CFRelease(key); CFRelease(want);
}

static long current_boottime(void) {
    struct timeval tv;
    size_t len = sizeof(tv);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &tv, &len, NULL, 0) != 0) return 0;
    return (long)tv.tv_sec;
}

#define RESET_MARKER "/var/db/housitouchd.lastreset"
#define USB_RESET_HELPER "/usr/local/libexec/housitouch-usbreset"

static int already_reset_this_boot(void) {
    FILE *f = fopen(RESET_MARKER, "r");
    if (!f) return 0;
    long bt = 0;
    int ok = (fscanf(f, "%ld", &bt) == 1);
    fclose(f);
    long cur = current_boottime();
    return ok && cur > 0 && bt == cur;
}

static void mark_reset_this_boot(void) {
    FILE *f = fopen(RESET_MARKER, "w");
    if (!f) return;
    fprintf(f, "%ld\n", current_boottime());
    fclose(f);
    chmod(RESET_MARKER, 0644);
}

static uid_t console_uid(void) {
    uid_t uid = 0;
    CFStringRef user = SCDynamicStoreCopyConsoleUser(NULL, &uid, NULL);
    if (user) CFRelease(user);
    return uid;
}

static void *late_mode_hook(void *arg) {
    (void)arg;
    sleep(4);
    uid_t uid = console_uid();
    if (uid == 0) { logline("late hook: no console user"); return NULL; }
    char uidbuf[16];
    snprintf(uidbuf, sizeof(uidbuf), "%u", (unsigned)uid);
    pid_t pid = 0;
    char *argv[] = {"/bin/launchctl", "asuser", uidbuf, "/usr/bin/open", "-g", "-j", "-a",
                    "/Applications/HOUSITouchHelper.app", NULL};
    int rc = posix_spawn(&pid, "/bin/launchctl", NULL, NULL, argv, environ);
    logline("late hook: launch helper as uid=%s rc=%d pid=%d", uidbuf, rc, (int)pid);
    return NULL;
}

static void spawn_helper(const char *path) {
    pid_t pid = 0;
    char *argv[] = {(char *)path, NULL};
    int rc = posix_spawn(&pid, path, NULL, NULL, argv, environ);
    logline("spawn %s rc=%d pid=%d (detached)", path, rc, (int)pid);
}

static void inject_all(void) {
    CFMutableDictionaryRef m = IOServiceMatching("AppleUserHIDEventService");
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, m, &it) != KERN_SUCCESS) return;
    io_object_t o;
    while ((o = IOIteratorNext(it))) { if (is_digitizer(o)) inject_now(o); IOObjectRelease(o); }
    IOObjectRelease(it);
}

static io_iterator_t g_iter = 0;
static void service_added(void *refcon, io_iterator_t it) {
    io_object_t o;
    while ((o = IOIteratorNext(it))) {
        if (is_digitizer(o)) {
            inject_now(o);
            // give WindowServer a moment; re-assert once shortly after in case of ordering
            usleep(20000);
            inject_now(o);
        }
        IOObjectRelease(o);
    }
}

int main(void) {
    logline("housitouchd v3 start pid=%d target=%s boot=%ld", getpid(), kTargetUUID, current_boottime());
    int have = 0;
    {
        CFMutableDictionaryRef mm = IOServiceMatching("AppleUserHIDEventService");
        io_iterator_t it0 = 0;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, mm, &it0) == KERN_SUCCESS) {
            io_object_t o0;
            while ((o0 = IOIteratorNext(it0))) { if (is_digitizer(o0)) have = 1; IOObjectRelease(o0); }
            IOObjectRelease(it0);
        }
    }
    inject_all();
    IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
    if (!port) { logline("failed to create notification port"); return 1; }
    CFMutableDictionaryRef m = IOServiceMatching("AppleUserHIDEventService");
    if (IOServiceAddMatchingNotification(port, kIOFirstMatchNotification, m, service_added, NULL, &g_iter) == KERN_SUCCESS) {
        io_object_t o; while ((o = IOIteratorNext(g_iter))) { if (is_digitizer(o)) inject_now(o); IOObjectRelease(o); }
        CFRunLoopSourceRef src = IONotificationPortGetRunLoopSource(port);
        CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
        logline("matching notification armed");
    } else {
        logline("failed to arm matching notification");
    }
    if (have) {
        if (already_reset_this_boot()) {
            logline("USB re-enumerate already done this boot; skip");
        } else {
            logline("forcing one USB re-enumerate so WindowServer re-attaches with displayUUID");
            spawn_helper(USB_RESET_HELPER);
            mark_reset_this_boot();
            pthread_t th;
            if (pthread_create(&th, NULL, late_mode_hook, NULL) == 0) pthread_detach(th);
        }
    } else {
        logline("no digitizer at startup; waiting for service notification");
    }
    /* keep run loop alive so notification callback can inject immediately after reset */
    CFRunLoopRun();
    return 0;
}
