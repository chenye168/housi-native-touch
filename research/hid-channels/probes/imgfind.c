// imgfind.c — read-only local probe for macOS 27 (arm64e)
// 目的：确认关键字符串（HID 插件路径 / displayUUID / WindowServer 丢弃日志 /
// 私有 entitlement）究竟位于哪个已加载的 Mach-O 镜像中。
// 原理：dlopen 一组系统框架，然后遍历 _dyld 已加载镜像，逐 segment 做字节搜索。
// 不写任何系统位置，不修改系统状态。
//
// 构建: clang -O1 -o imgfind imgfind.c -framework CoreFoundation
// 运行: ./imgfind

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/getsect.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static const char *kFrameworks[] = {
    "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit",
    "/System/Library/PrivateFrameworks/HID.framework/Versions/A/HID",
    "/System/Library/PrivateFrameworks/HIDPreferences.framework/Versions/A/HIDPreferences",
    "/System/Library/PrivateFrameworks/UniversalHID.framework/Versions/A/UniversalHID",
    "/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight",
    "/System/Library/PrivateFrameworks/BackBoardServices.framework/Versions/A/BackBoardServices",
    "/System/Library/PrivateFrameworks/BackBoardHIDEventFoundation.framework/Versions/A/BackBoardHIDEventFoundation",
    "/System/Library/PrivateFrameworks/BackBoardHIDTouchEventProcessor.framework/Versions/A/BackBoardHIDTouchEventProcessor",
    "/System/Library/PrivateFrameworks/MultitouchSupport.framework/Versions/A/MultitouchSupport",
    "/System/Library/Frameworks/CoreHID.framework/Versions/A/CoreHID",
    NULL,
};

static const char *kPatterns[] = {
    "/System/Library/HIDPlugins",
    "ServicePluginDebug",
    "com.apple.iohideventsystem.server",
    "com.apple.developer.hid.virtual.device",
    "displayUUID",
    "handle_digitizer_added",
    "from unsupported service",
    "BKTouchEvents",
    "com.apple.backboardd.touchStreams",
    "com.apple.backboardd.virtualDisplay",
    "Cannot set displayUUID",
    "HIDServicePlugin",
    "IOHIDEventSystemServerStart",
    "virtual.device",
    "hid.virtual",
    "notPrivileged",
    "MultitouchSessionFilter",
    "com.apple.Multitouch",
    "IOHIDEventSystemClientCreateWithType",
    "kIOHIDEventSystemClientType",
    "BKSTouchStream",
    "CGVirtualDisplay",
    "com.apple.hid.manager.user-access-device",
    "com.apple.developer.driverkit.family.hid.eventservice",
    "HIDVirtualDevice",
    NULL,
};

static void scan_image(uint32_t idx, const char *patterns_to_report)
{
    (void)patterns_to_report;
    const struct mach_header_64 *hdr = (const struct mach_header_64 *)_dyld_get_image_header(idx);
    const char *name = _dyld_get_image_name(idx);
    if (!hdr || hdr->magic != MH_MAGIC_64) return;
    intptr_t slide = _dyld_get_image_vmaddr_slide(idx);

    const uint8_t *p = (const uint8_t *)(hdr + 1);
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)p;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
            uint64_t len = seg->filesize ? seg->filesize : seg->vmsize;
            if (len == 0 || len > (512ull << 20)) { p += lc->cmdsize; continue; }
            const uint8_t *base = (const uint8_t *)(uintptr_t)(seg->vmaddr + (uint64_t)slide);
            if (!base) { p += lc->cmdsize; continue; }
            for (int k = 0; kPatterns[k]; k++) {
                const char *pat = kPatterns[k];
                size_t pl = strlen(pat);
                if (pl >= len) continue;
                const uint8_t *hit = memmem(base, (size_t)len, pat, pl);
                if (hit) {
                    printf("HIT  pattern=%-42s image=%s seg=%s off=0x%llx\n",
                           pat, name ? name : "?",
                           seg->segname, (unsigned long long)(hit - base));
                }
            }
        }
        p += lc->cmdsize;
    }
}

int main(void)
{
    printf("== dlopen targets ==\n");
    for (int i = 0; kFrameworks[i]; i++) {
        void *h = dlopen(kFrameworks[i], RTLD_LAZY | RTLD_LOCAL);
        printf("%s : %s\n", kFrameworks[i], h ? "OK" : dlerror());
    }
    printf("\n== image count: %u ==\n", _dyld_image_count());
    printf("\n== string hits ==\n");
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        scan_image(i, NULL);
    }
    return 0;
}
