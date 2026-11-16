// svcprobe.c — read-only probe of the IOHIDEventSystem service table.
// 目的：确认 HOUSI(VID 0x32D7/PID 0x0001) digitizer service 在 WindowServer 可见的
// 事件系统里出现，并且**没有** displayUUID 属性。
// 仅调用 Copy* 只读 API，不 set 任何属性，不修改系统状态。
//
// 构建: clang -O1 -o svcprobe svcprobe.c -framework IOKit -framework CoreFoundation
// 运行: ./svcprobe

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hidsystem/IOHIDEventSystemClient.h>
#include <IOKit/hidsystem/IOHIDServiceClient.h>
#include <stdio.h>
#include <stdlib.h>

static void print_cf(const char *label, CFTypeRef v)
{
    if (!v) { printf("  %-22s = (null)\n", label); return; }
    CFStringRef s = CFCopyDescription(v);
    char buf[256] = {0};
    if (s) { CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8); CFRelease(s); }
    printf("  %-22s = %s\n", label, buf);
}

static CFTypeRef copy_prop(IOHIDServiceClientRef svc, const char *key)
{
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFTypeRef v = IOHIDServiceClientCopyProperty(svc, k);
    CFRelease(k);
    return v;
}

int main(void)
{
    IOHIDEventSystemClientRef client = IOHIDEventSystemClientCreateSimpleClient(kCFAllocatorDefault);
    printf("client = %p\n", (void *)client);
    if (!client) { printf("RESULT: simple client creation failed (entitlement/TCC?)\n"); return 1; }

    CFArrayRef services = IOHIDEventSystemClientCopyServices(client);
    printf("services = %p count=%ld\n", (void *)services,
           services ? (long)CFArrayGetCount(services) : -1L);
    if (!services) { CFRelease(client); return 1; }

    long n = CFArrayGetCount(services);
    int hits = 0;
    for (long i = 0; i < n; i++) {
        IOHIDServiceClientRef svc = (IOHIDServiceClientRef)CFArrayGetValueAtIndex(services, i);
        CFTypeRef vid = copy_prop(svc, "VendorID");
        CFTypeRef pid = copy_prop(svc, "ProductID");
        CFTypeRef pup = copy_prop(svc, "PrimaryUsagePage");
        CFTypeRef pu = copy_prop(svc, "PrimaryUsage");
        int isTarget = 0;
        int iv = -1, ip = -1;
        if (vid && CFGetTypeID(vid) == CFNumberGetTypeID()) CFNumberGetValue(vid, kCFNumberIntType, &iv);
        if (pid && CFGetTypeID(pid) == CFNumberGetTypeID()) CFNumberGetValue(pid, kCFNumberIntType, &ip);
        if (iv == 0x32D7 && ip == 0x0001) isTarget = 1;
        if (isTarget || (pup && pu)) {
            // 打印所有 digitizer (usage page 13) 服务，或目标设备
            int up = -1;
            if (pup && CFGetTypeID(pup) == CFNumberGetTypeID()) CFNumberGetValue(pup, kCFNumberIntType, &up);
            if (!isTarget && up != 13) goto next;
            hits++;
            printf("\n== service[%ld] target=%d ==\n", i, isTarget);
            CFTypeRef prod = copy_prop(svc, "Product");
            CFTypeRef trans = copy_prop(svc, "Transport");
            CFTypeRef builtin = copy_prop(svc, "Built-In");
            CFTypeRef dispint = copy_prop(svc, "DisplayIntegrated");
            CFTypeRef duuid = copy_prop(svc, "displayUUID");
            CFTypeRef virt = copy_prop(svc, "isVirtualService");
            CFTypeRef sid = IOHIDServiceClientGetRegistryID(svc);
            print_cf("VendorID", vid); print_cf("ProductID", pid);
            print_cf("PrimaryUsagePage", pup); print_cf("PrimaryUsage", pu);
            print_cf("Product", prod); print_cf("Transport", trans);
            print_cf("Built-In", builtin); print_cf("DisplayIntegrated", dispint);
            print_cf("isVirtualService", virt);
            print_cf("RegistryID", sid);
            print_cf("displayUUID", duuid);
            if (prod) CFRelease(prod);
            if (trans) CFRelease(trans);
            if (builtin) CFRelease(builtin);
            if (dispint) CFRelease(dispint);
            if (virt) CFRelease(virt);
            if (sid) CFRelease(sid);
            if (duuid) CFRelease(duuid);
        }
    next:
        if (vid) CFRelease(vid);
        if (pid) CFRelease(pid);
        if (pup) CFRelease(pup);
        if (pu) CFRelease(pu);
    }
    if (hits == 0) printf("no digitizer service visible to this client\n");
    CFRelease(services);
    CFRelease(client);
    return hits ? 0 : 2;
}
