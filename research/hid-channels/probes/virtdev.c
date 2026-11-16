// virtdev.c — read-only probe: does a plain (unentitled) user process get a
// virtual HID digitizer on macOS 27?
//
// 构建: clang -O1 -o virtdev virtdev.c -framework IOKit -framework CoreFoundation
// 运行: ./virtdev      # 普通用户即可，无需 sudo
//
// 预期（依据 SDK 头文件注释）：IOHIDUserDeviceCreateWithProperties 需要
// entitlement "com.apple.developer.hid.virtual.device"，普通进程应返回 NULL。
//
// 本程序不会留下任何系统改动：未创建成功则无副作用；即使创建成功，进程退出
// 即销毁虚拟设备。

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hidsystem/IOHIDUserDevice.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

// 最小 Touch Screen digitizer 描述符：单一 finger，TipSwitch + X/Y(16bit)
static const uint8_t kDesc[] = {
    0x05, 0x0D,        // Usage Page (Digitizer)
    0x09, 0x04,        // Usage (Touch Screen)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x22,        //   Usage (Finger)
    0xA1, 0x02,        //   Collection (Logical)
    0x09, 0x42,        //     Usage (Tip Switch)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x75, 0x01,        //     Report Size (1)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x09, 0x32,        //     Usage (In Range)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x95, 0x06,        //     Report Count (6) padding
    0x81, 0x03,        //     Input (Const,Var,Abs)
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x15, 0x00,        //     Logical Minimum (0)
    0x26, 0xFF, 0x7F,  //     Logical Maximum (32767)
    0x75, 0x10,        //     Report Size (16)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x09, 0x31,        //     Usage (Y)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0xC0,              //   End Collection
    0xC0,              // End Collection
};

static void show_entitlement_note(void)
{
    FILE *f = popen("codesign -d --entitlements - \"$0\" 2>/dev/null", "r");
    if (f) { fclose(f); }
    printf("note: this binary is ad-hoc/plain signed; check `codesign -d --entitlements - ./virtdev`\n");
}

int main(void)
{
    printf("== IOHIDUserDevice virtual device probe ==\n");
    printf("uid=%d euid=%d\n", getuid(), geteuid());
    show_entitlement_note();

    CFMutableDictionaryRef props = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDataRef desc = CFDataCreate(kCFAllocatorDefault, kDesc, sizeof(kDesc));
    CFDictionarySetValue(props, CFSTR(kIOHIDReportDescriptorKey), desc);

    int vendor = 0x32D7, product = 0x0001, usagePage = 0x0D, usage = 0x04, builtin = 0;
    CFNumberRef nVendor = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendor);
    CFNumberRef nProduct = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &product);
    CFNumberRef nUP = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usagePage);
    CFNumberRef nU = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);
    CFNumberRef nBI = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &builtin);
    CFDictionarySetValue(props, CFSTR(kIOHIDVendorIDKey), nVendor);
    CFDictionarySetValue(props, CFSTR(kIOHIDProductIDKey), nProduct);
    CFDictionarySetValue(props, CFSTR(kIOHIDPrimaryUsagePageKey), nUP);
    CFDictionarySetValue(props, CFSTR(kIOHIDPrimaryUsageKey), nU);
    CFDictionarySetValue(props, CFSTR("Built-In"), nBI);
    // 尝试在创建时直接带上 displayUUID（Sidecar 路径的行为），看是否被接受
    CFUUIDRef uuid = CFUUIDCreateFromString(kCFAllocatorDefault,
        CFSTR("30D985CD-BE3F-4C0E-B055-C5E22CFF2E07"));
    if (uuid) CFDictionarySetValue(props, CFSTR("displayUUID"), uuid);

    errno = 0;
    IOHIDUserDeviceRef dev = IOHIDUserDeviceCreateWithProperties(kCFAllocatorDefault, props, 0);
    printf("IOHIDUserDeviceCreateWithProperties -> %p (errno=%d %s)\n",
           (void *)dev, errno, strerror(errno));

    if (!dev) {
        printf("RESULT: BLOCKED (no entitlement / no permission)\n");
        printf("hint: `log show --last 30s --predicate 'eventMessage CONTAINS \"IOHIDUserDevice\"'` for the kernel/server reason\n");
    } else {
        printf("RESULT: created virtual device %p\n", (void *)dev);
        IOHIDUserDeviceActivate(dev);
        printf("activate called\n");
        CFTypeRef v = IOHIDUserDeviceCopyProperty(dev, CFSTR("displayUUID"));
        printf("copyProperty(displayUUID) -> %p\n", v);
        IOHIDUserDeviceCancel(dev);
        CFRelease(dev);
    }
    CFRelease(props);
    return dev ? 0 : 1;
}
