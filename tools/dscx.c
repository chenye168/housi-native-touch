// dscx: minimal driver for /usr/lib/dsc_extractor.bundle (dyld-27104)
// usage: dscx <path-to-dyld_shared_cache_xxx> <output-dir>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef int (*extract_fn_t)(const char* cache_path, const char* extraction_root,
                            void (^progress)(unsigned current, unsigned total));

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <dyld_shared_cache_file> <output-dir>\n", argv[0]);
        return 2;
    }
    void* h = dlopen("/usr/lib/dsc_extractor.bundle", RTLD_NOW);
    if (!h) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    extract_fn_t fn = (extract_fn_t)dlsym(h, "dyld_shared_cache_extract_dylibs_progress");
    if (!fn) {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        return 1;
    }
    fprintf(stderr, "extracting %s -> %s\n", argv[1], argv[2]);
    int rc = fn(argv[1], argv[2], ^(unsigned current, unsigned total) {
        if (current == total || (current % 200) == 0)
            fprintf(stderr, "progress %u/%u\n", current, total);
    });
    fprintf(stderr, "extract result: %d\n", rc);
    return rc;
}
