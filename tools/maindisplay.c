// maindisplay: temporarily swap main display (for Dock touch gating test), and restore.
// usage: maindisplay --save | --housi | --restore
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SAVE "/tmp/housitouch_display_origins.txt"

static int find_ids(CGDirectDisplayID *builtin, CGDirectDisplayID *ext) {
    CGDirectDisplayID ids[16]; uint32_t n = 0;
    if (CGGetOnlineDisplayList(16, ids, &n) != kCGErrorSuccess) return -1;
    *builtin = 0; *ext = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (CGDisplayIsBuiltin(ids[i])) *builtin = ids[i];
        else if (!CGDisplayIsMain(ids[i])) *ext = ids[i];
        else if (ids[i] != *builtin) *ext = ids[i];
    }
    if (!*builtin) return -1;
    if (!*ext) { for (uint32_t i = 0; i < n; i++) if (ids[i] != *builtin) { *ext = ids[i]; break; } }
    return *ext ? 0 : -1;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: %s --save|--housi|--restore\n", argv[0]); return 2; }
    CGDirectDisplayID builtin = 0, ext = 0;
    if (find_ids(&builtin, &ext) != 0) { printf("display find failed\n"); return 1; }
    CGRect bi = CGDisplayBounds(builtin);
    CGRect ex = CGDisplayBounds(ext);

    if (!strcmp(argv[1], "--save")) {
        FILE *f = fopen(SAVE, "w");
        fprintf(f, "%u %u %d %d %d %d\n", builtin, ext, (int)bi.origin.x, (int)bi.origin.y, (int)ex.origin.x, (int)ex.origin.y);
        fclose(f);
        printf("saved builtin=%u(%d,%d) ext=%u(%d,%d)\n", builtin, (int)bi.origin.x, (int)bi.origin.y, ext, (int)ex.origin.x, (int)ex.origin.y);
        return 0;
    }
    unsigned sb = 0, se = 0; int sbx=0, sby=0, sex=0, sey=0;
    FILE *f = fopen(SAVE, "r");
    if (!f) { printf("no saved origins; run --save first\n"); return 1; }
    if (fscanf(f, "%u %u %d %d %d %d", &sb, &se, &sbx, &sby, &sex, &sey) != 6) { fclose(f); printf("bad save file\n"); return 1; }
    fclose(f);

    CGDisplayConfigRef cfg = NULL;
    if (CGBeginDisplayConfiguration(&cfg) != kCGErrorSuccess || !cfg) { printf("begin config failed\n"); return 1; }
    if (!strcmp(argv[1], "--housi")) {
        CGConfigureDisplayOrigin(cfg, ext, 0, 0);
        CGConfigureDisplayOrigin(cfg, builtin, (int)ex.size.width, 0);
    } else if (!strcmp(argv[1], "--restore")) {
        CGConfigureDisplayOrigin(cfg, builtin, sbx, sby);
        CGConfigureDisplayOrigin(cfg, ext, sex, sey);
    } else {
        CGCancelDisplayConfiguration(cfg);
        printf("unknown option\n");
        return 2;
    }
    CGError err = CGCompleteDisplayConfiguration(cfg, kCGConfigureForSession);
    printf("complete config err=%d\n", (int)err);
    return err == kCGErrorSuccess ? 0 : 1;
}
