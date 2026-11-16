// regmark: set an arbitrary property on the HOUSI digitizer HID event service via IORegistry, then read back.
// usage: regmark <key> <string-value>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
static long get_int(io_registry_entry_t e, CFStringRef k, long d){CFTypeRef v=IORegistryEntryCreateCFProperty(e,k,kCFAllocatorDefault,0);long r=d;if(v){if(CFGetTypeID(v)==CFNumberGetTypeID())CFNumberGetValue((CFNumberRef)v,kCFNumberLongType,&r);CFRelease(v);}return r;}
int main(int argc,char**argv){
  if(argc<3){printf("usage: %s key value\n",argv[0]);return 2;}
  CFStringRef key=CFStringCreateWithCString(NULL,argv[1],kCFStringEncodingUTF8);
  CFStringRef val=CFStringCreateWithCString(NULL,argv[2],kCFStringEncodingUTF8);
  CFMutableDictionaryRef m=IOServiceMatching("AppleUserHIDEventService");io_iterator_t it=0;
  if(IOServiceGetMatchingServices(kIOMainPortDefault,m,&it)!=KERN_SUCCESS){printf("match fail\n");return 1;}
  io_object_t o;io_registry_entry_t target=0;
  while((o=IOIteratorNext(it))){ if(get_int(o,CFSTR("PrimaryUsagePage"),-1)==0x0D && get_int(o,CFSTR("PrimaryUsage"),-1)==0x04){target=o;break;} IOObjectRelease(o);}
  IOObjectRelease(it);
  if(!target){printf("digitizer not found\n");return 2;}
  kern_return_t kr=IORegistryEntrySetCFProperty(target,key,val);
  printf("set %s=%s kr=0x%x\n",argv[1],argv[2],kr);
  CFTypeRef rb=IORegistryEntryCreateCFProperty(target,key,kCFAllocatorDefault,0);
  printf("readback: %s\n", rb ? "non-nil" : "nil");
  if(rb)CFRelease(rb);
  CFRelease(key);CFRelease(val);IOObjectRelease(target);
  return 0;
}
