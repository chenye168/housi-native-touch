// dumpimg.c — read-only helper: dump printable strings around a pattern inside a
// loaded (shared-cache) image. Used to study the IOKit.framework HID plugin loader.
//
// 构建: clang -O1 -o dumpimg dumpimg.c -framework CoreFoundation
// 用法: ./dumpimg <image-path-substring> <pattern> [context-bytes]
// 例:   ./dumpimg IOKit.framework/Versions/A/IOKit ServicePluginDebug 12000

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void dl(const char *p) { (void)dlopen(p, RTLD_LAZY | RTLD_LOCAL); }

static int is_printable(int c) { return c >= 32 && c <= 126; }

static void dump_window(const uint8_t *base, const uint8_t *hit, size_t ctx, size_t cap)
{
    size_t start = (size_t)(hit - base);
    start = start > ctx ? start - ctx : 0;
    size_t end = (size_t)(hit - base) + ctx;
    if (end > cap) end = cap;
    const uint8_t *p = base + start;
    const uint8_t *e = base + end;
    char buf[512];
    size_t n = 0;
    while (p < e) {
        if (is_printable(*p)) {
            if (n < sizeof(buf) - 1) buf[n++] = (char)*p;
        } else {
            if (n >= 4) { buf[n] = 0; printf("%s\n", buf); }
            n = 0;
        }
        p++;
    }
    if (n >= 4) { buf[n] = 0; printf("%s\n", buf); }
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <image-substr> <pattern> [ctx]\n", argv[0]); return 2; }
    const char *imgsub = argv[1], *pat = argv[2];
    size_t ctx = argc > 3 ? (size_t)atol(argv[3]) : 8000;

    const char *preloads[] = {
        "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit",
        "/System/Library/PrivateFrameworks/HID.framework/Versions/A/HID",
        "/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight",
        "/System/Library/PrivateFrameworks/BackBoardHIDTouchEventProcessor.framework/Versions/A/BackBoardHIDTouchEventProcessor",
        "/System/Library/PrivateFrameworks/BackBoardHIDEventFoundation.framework/Versions/A/BackBoardHIDEventFoundation",
        "/System/Library/PrivateFrameworks/UniversalHID.framework/Versions/A/UniversalHID",
        "/System/Library/PrivateFrameworks/BackBoardServices.framework/Versions/A/BackBoardServices",
        NULL
    };
    for (int i = 0; preloads[i]; i++) dl(preloads[i]);

    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *name = _dyld_get_image_name(i);
        if (!name || !strstr(name, imgsub)) continue;
        const struct mach_header_64 *hdr = (const struct mach_header_64 *)_dyld_get_image_header(i);
        if (!hdr || hdr->magic != MH_MAGIC_64) continue;
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        const uint8_t *p = (const uint8_t *)(hdr + 1);
        int printed = 0;
        for (uint32_t c = 0; c < hdr->ncmds; c++) {
            const struct load_command *lc = (const struct load_command *)p;
            if (lc->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
                size_t len = (size_t)(seg->filesize ? seg->filesize : seg->vmsize);
                if (len && len < (512ull << 20)) {
                    const uint8_t *base = (const uint8_t *)(uintptr_t)(seg->vmaddr + (uint64_t)slide);
                    const uint8_t *hit = memmem(base, len, pat, strlen(pat));
                    if (hit) {
                        printf("=== image=%s seg=%s hit_off=0x%llx ===\n", name, seg->segname,
                               (unsigned long long)(hit - base));
                        dump_window(base, hit, ctx, len);
                        printed++;
                    }
                }
            }
            p += lc->cmdsize;
        }
        if (!printed) printf("(%s: pattern not found)\n", name);
    }
    return 0;
}
