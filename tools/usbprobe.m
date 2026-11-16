// usbprobe: inspect IOUSBHost framework classes/methods and try to re-enumerate the HOUSI USB device
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <IOKit/IOKitLib.h>
#import <dlfcn.h>

static void dumpMethods(Class c) {
    unsigned int n = 0;
    Method *ms = class_copyMethodList(c, &n);
    printf("== class %s instance methods: %u\n", class_getName(c), n);
    for (unsigned int i = 0; i < n; i++) printf("  -%s\n", sel_getName(method_getName(ms[i])));
    free(ms);
    Class meta = object_getClass(c);
    n = 0; ms = class_copyMethodList(meta, &n);
    printf("== class %s class methods: %u\n", class_getName(c), n);
    for (unsigned int i = 0; i < n; i++) printf("  +%s\n", sel_getName(method_getName(ms[i])));
    free(ms);
}

int main(int argc, char** argv) {
    void* h = dlopen("/System/Library/Frameworks/IOUSBHost.framework/Versions/A/IOUSBHost", RTLD_NOW);
    if (!h) h = dlopen("/System/Library/Frameworks/IOUSBHost.framework/IOUSBHost", RTLD_NOW);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    Class dev = objc_getClass("IOUSBHostDevice");
    Class obj = objc_getClass("IOUSBHostObject");
    if (!dev) { printf("IOUSBHostDevice class not found\n"); return 1; }
    dumpMethods(dev);
    if (obj) dumpMethods(obj);
    return 0;
}
