// hidprobe.c — HOUSI / USB2IIC_CTP 触控 HID 探针（IOHIDManager，纯 C，Command Line Tools 可编译）
//
// 功能:
//   --list      枚举 VID 0x32D7 / PID 0x0001 的所有 HID 接口，打印 usagePage/usage/Transport/
//               Product/ReportDescriptor 长度、report ID 布局、feature 元素等。
//   --dump      打开 Digitizer 接口并打印 input report（reportID + 原始 hex + 触点解码），
//               带频率限制；--seconds 控制时长（0 = Ctrl-C 退出）。
//   --setmode   按 HID 描述符定位 Input Mode (usagePage 0x0D, usage 0x52) 所在的 Feature Report，
//               构造 report 并调用 IOHIDDeviceSetReport 写入多点模式 (默认 2)，随后打印
//               IOHIDDeviceGetReport 回读值，并通过全新的子进程重新打开设备验证 Input Mode。
//   --request-access  触发 Input Monitoring 授权请求。
//
// 重要实现说明（2026-11-16 在 macOS 27.2.1 实测得到）:
//   * 本机该控制器 Setting Input Mode 会导致 HID 设备重新枚举，持有旧 IOHIDDevice/IOHIDElement
//     引用的进程若继续访问可能崩溃（SIGTRAP）。因此 --setmode 把「读 / 写 / 回读」分别放进独立
//     子进程执行，父进程只做编排，回读一定来自重新打开设备的新进程。
//   * 本固件 IOHIDDeviceGetReport 对任意 report ID 都返回固定的 3 字节 11 0a 00（report 0x11 的
//     内容）。它不能用于校验 0x13 的内容，回读以 IOHIDDeviceGetValue(feature element) 为准；
//     GetReport 的原始返回值仍按要求打印出来。
//   * IOHIDDeviceSetReport 的 report buffer 第一个字节必须是 Report ID（Apple 文档语义），
//     实测写入 report 0x13 的 {0x13, 0x01, 0x00} 确实把 Input Mode 改为 1、Device ID 改为 0。
//
// 编译: clang -O2 -Wall -o build/hidprobe hidprobe.c -framework IOKit -framework CoreFoundation

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/sysctl.h>
#include <spawn.h>
#include <mach-o/dyld.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOReturn.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDElement.h>
#include <IOKit/hid/IOHIDValue.h>
#include <IOKit/hid/IOHIDBase.h>
#include <IOKit/hidsystem/IOHIDLib.h>

extern char **environ;

#define DP_VID_DEFAULT 0x32d7
#define DP_PID_DEFAULT 0x0001
#define MAX_DEVICES 8
#define MAX_FIELDS 512
#define MAX_USAGES 64

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------

static FILE *g_logfile = NULL;
static int g_raw_output = 0;   // 子进程模式: 纯 printf，不加时间戳
static const char *g_logfile_path = NULL;

static void log_open(const char *path) {
    if (!path || !*path) return;
    g_logfile = fopen(path, "a");
    if (!g_logfile) {
        fprintf(stderr, "[hidprobe] 无法以追加方式打开日志文件 %s（继续只输出到 stdout）\n", path);
        return;
    }
    g_logfile_path = path;
    setvbuf(g_logfile, NULL, _IONBF, 0);
}

static void log_close(void) {
    if (g_logfile) { fflush(g_logfile); fclose(g_logfile); g_logfile = NULL; }
}

static void timestamp(char *buf, size_t cap) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tmv;
    time_t t = tv.tv_sec;
    localtime_r(&t, &tmv);
    char base[32];
    strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &tmv);
    snprintf(buf, cap, "%s.%03d%+03d%02d", base, (int)(tv.tv_usec / 1000),
             (int)(tmv.tm_gmtoff / 3600), (int)((tmv.tm_gmtoff % 3600) / 60));
}

static void os_version_string(char *buf, size_t cap) {
    char prod[64] = "", build[64] = "";
    size_t sz = sizeof prod;
    if (sysctlbyname("kern.osproductversion", prod, &sz, NULL, 0) != 0) snprintf(prod, sizeof prod, "?");
    sz = sizeof build;
    if (sysctlbyname("kern.osversion", build, &sz, NULL, 0) != 0) snprintf(build, sizeof build, "?");
    snprintf(buf, cap, "%s (%s)", prod, build);
}

static void log_line(const char *fmt, ...) {
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (g_raw_output) {
        printf("%s\n", msg);
        fflush(stdout);
    } else {
        char ts[64];
        timestamp(ts, sizeof ts);
        printf("%s %s\n", ts, msg);
        fflush(stdout);
        if (g_logfile) { fprintf(g_logfile, "%s %s\n", ts, msg); fflush(g_logfile); }
    }
}

static void section(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    log_line("");
    log_line("=== %s ===", msg);
}

// ---------------------------------------------------------------------------
// 参数
// ---------------------------------------------------------------------------

typedef struct {
    int list;
    int dump;
    int setmode;
    int descriptor;
    int elements;
    int request_access;
    int dry_run;
    int force;
    int restore;
    int seconds;          // dump 默认 10 秒; 0 = 直到 Ctrl-C
    int mode;             // setmode 目标 Input Mode，默认 2
    int iface_all;
    int iface_mouse;
    int vid;
    int pid;
    const char *logfile;
    // 内部子命令
    int child_inspect;
    int child_open_close;
    int child_read_mode;
    int child_get_report;
    int child_write_mode;
    int get_report_id;    // -1 = 使用描述符里 Input Mode 所在 report
} Options;

static int parse_int_auto(const char *s, int def) {
    if (!s) return def;
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (end == s) return def;
    return (int)v;
}

static void usage(void) {
    printf(
"hidprobe — HOUSI / USB2IIC_CTP 触控 HID 探针（IOHIDManager）\n"
"\n"
"用法:\n"
"  hidprobe [--list] [--descriptor] [--elements]\n"
"  hidprobe --dump [--seconds N] [--interface digitizer|mouse|all]\n"
"  hidprobe --setmode [--mode N] [--dry-run] [--force] [--restore]\n"
"  hidprobe --request-access\n"
"  hidprobe [--vid 0x32d7] [--pid 0x0001] [--logfile PATH]\n"
"\n"
"选项:\n"
"  --list            枚举 VID/PID 匹配的 HID 接口（默认动作）\n"
"  --descriptor      额外打印完整 Report Descriptor hex\n"
"  --elements        额外打印所有 Feature Report 元素（IOHIDDeviceCopyMatchingElements）\n"
"  --dump            读取并打印 input report（含 reportID、原始 hex、触点解码）\n"
"  --seconds N       --dump 运行 N 秒后退出（默认 10；0 = Ctrl-C 退出）\n"
"  --interface ARG   digitizer（默认）| mouse | all\n"
"  --setmode         发送 Feature Report 把 Input Mode (0x0D/0x52) 设为 --mode（默认 2）\n"
"  --mode N          Input Mode 目标值（0..10，默认 2 = multi-touch）\n"
"  --dry-run         --setmode 时只打印将发送的 report，不实际写设备\n"
"  --force           Input Mode 已等于目标值时仍发送一次 SetReport\n"
"  --restore         写完并验证后把 Input Mode 恢复为原值\n"
"  --request-access  调用 IOHIDRequestAccess(kIOHIDRequestTypeListenEvent) 触发授权提示\n"
"  --logfile PATH    日志追加写入该文件\n"
"  --vid/--pid       覆盖默认 VID=0x32d7 PID=0x0001\n"
"  --help/-h         显示本帮助\n"
"\n"
"内部子命令（--setmode 会自动以子进程方式调用；也可手动运行）:\n"
"  --inspect         只读: 枚举 digitizer、dump feature 元素、打印当前 Input Mode、GetReport\n"
"  --open-close      只读: 打印 IOHIDDeviceOpen / IOHIDDeviceClose 的返回值（不做任何 GetValue）\n"
"  --read-mode       只读: 打印 READ_MODE mode=<n> devid=<n>\n"
"  --get-report [RID] 只读: 对 Feature Report 调 IOHIDDeviceGetReport 并打印返回值\n"
"  --write-mode N    写: 构造并发送 report，把 Input Mode 设为 N（内部用）\n"
"\n"
"权限: 本机该设备 RequiresTCCAuthorization=Yes。读取 input report / SetReport 需要\n"
"      「系统设置 → 隐私与安全性 → 输入监控」授权给运行本程序的终端 App（TCC 记在负责进程上），\n"
"      然后完全退出并重开终端。返回 0xE00002E2 即权限被拒。\n");
}

static Options parse_args(int argc, char **argv) {
    Options o;
    memset(&o, 0, sizeof o);
    o.seconds = 10;
    o.mode = 2;
    o.vid = DP_VID_DEFAULT;
    o.pid = DP_PID_DEFAULT;
    o.get_report_id = -1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); exit(0); }
        else if (!strcmp(a, "--list")) o.list = 1;
        else if (!strcmp(a, "--dump")) o.dump = 1;
        else if (!strcmp(a, "--setmode")) o.setmode = 1;
        else if (!strcmp(a, "--descriptor")) o.descriptor = 1;
        else if (!strcmp(a, "--elements")) o.elements = 1;
        else if (!strcmp(a, "--request-access")) o.request_access = 1;
        else if (!strcmp(a, "--dry-run")) o.dry_run = 1;
        else if (!strcmp(a, "--force")) o.force = 1;
        else if (!strcmp(a, "--restore")) o.restore = 1;
        else if (!strcmp(a, "--interface") && val) { if (!strcmp(val, "all")) o.iface_all = 1; else if (!strcmp(val, "mouse")) o.iface_mouse = 1; i++; }
        else if (!strcmp(a, "--seconds") && val) { o.seconds = parse_int_auto(val, 10); i++; }
        else if (!strcmp(a, "--mode") && val) { o.mode = parse_int_auto(val, 2); i++; }
        else if (!strcmp(a, "--vid") && val) { o.vid = parse_int_auto(val, DP_VID_DEFAULT); i++; }
        else if (!strcmp(a, "--pid") && val) { o.pid = parse_int_auto(val, DP_PID_DEFAULT); i++; }
        else if (!strcmp(a, "--logfile") && val) { o.logfile = val; i++; }
        else if (!strcmp(a, "--inspect")) o.child_inspect = 1;
        else if (!strcmp(a, "--open-close")) o.child_open_close = 1;
        else if (!strcmp(a, "--read-mode")) o.child_read_mode = 1;
        else if (!strcmp(a, "--write-mode") && val) { o.child_write_mode = 1; o.mode = parse_int_auto(val, 2); i++; }
        else if (!strcmp(a, "--get-report")) {
            o.child_get_report = 1;
            if (val && val[0] != '-') { o.get_report_id = parse_int_auto(val, -1); i++; }
        } else if (a[0] != '-') {
            fprintf(stderr, "hidprobe: 未知参数: %s（--help 查看用法）\n", a);
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// IOReturn / 权限辅助
// ---------------------------------------------------------------------------

static const char *kr_name(IOReturn kr) {
    switch (kr) {
        case kIOReturnSuccess: return "kIOReturnSuccess";
        case kIOReturnNotPermitted: return "kIOReturnNotPermitted(Input Monitoring 被拒)";
        case kIOReturnNotPrivileged: return "kIOReturnNotPrivileged";
        case kIOReturnExclusiveAccess: return "kIOReturnExclusiveAccess";
        case kIOReturnNoDevice: return "kIOReturnNoDevice";
        case kIOReturnUnsupported: return "kIOReturnUnsupported";
        case kIOReturnBadArgument: return "kIOReturnBadArgument";
        case kIOReturnNoResources: return "kIOReturnNoResources";
        default: return NULL;
    }
}

static void log_kr(const char *what, IOReturn kr) {
    const char *n = kr_name(kr);
    if (n) log_line("%s: kr=0x%08x (%s)", what, kr, n);
    else   log_line("%s: kr=0x%08x", what, kr);
}

static const char *access_name(IOHIDAccessType a) {
    switch (a) {
        case kIOHIDAccessTypeGranted: return "granted(0)";
        case kIOHIDAccessTypeDenied: return "denied(1)";
        case kIOHIDAccessTypeUnknown: return "unknown(2)";
        default: return "?";
    }
}

static void print_permission_state(void) {
    IOHIDAccessType a = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent);
    log_line("Input Monitoring 权限 (IOHIDCheckAccess/kIOHIDRequestTypeListenEvent): %s", access_name(a));
    if (a != kIOHIDAccessTypeGranted) {
        log_line("  需要授权才能读 input report / SetReport:");
        log_line("  系统设置 → 隐私与安全性 → 输入监控 → 添加并勾选运行本程序的终端 App");
        log_line("  （TCC 把授权记在「负责进程」上，命令行工具一般不单独列出；授权后需完全退出并重开终端）");
        log_line("  本机设备属性 RequiresTCCAuthorization=Yes；返回 0xE00002E2 即被拒。");
        log_line("  可运行 hidprobe --request-access 主动触发授权提示。");
    }
}

static void print_accessibility_note(void) {
    log_line("辅助功能(Accessibility)授权仅在本工具主动发送 CGEvent 时需要；本探针只监听 + 读写 HID feature，不需要。");
}

// ---------------------------------------------------------------------------
// HID Report Descriptor 解析
// ---------------------------------------------------------------------------

typedef enum { RT_INPUT = 0, RT_OUTPUT = 1, RT_FEATURE = 2 } ReportKind;

typedef struct {
    uint8_t type;         // RT_*
    uint16_t report_id;
    uint32_t bit_offset;  // 报告数据内的 bit 偏移（不含 Report ID 字节）
    uint32_t bit_size;
    int has_usage;
    uint32_t usage_page;
    uint32_t usage;
    int32_t logical_min;
    int32_t logical_max;
} HidField;

typedef struct {
    HidField fields[MAX_FIELDS];
    int field_count;
    uint32_t report_bits[3][256];
    uint8_t report_used[3][256];
} HidLayout;

typedef struct {
    uint32_t usage_page, report_size, report_count, report_id;
    int32_t logical_min, logical_max;
} HidGlobalState;

static int32_t item_value(const uint8_t *data, int size) {
    int32_t v = 0;
    for (int i = size - 1; i >= 0; i--) v = (v << 8) | data[i];
    if (size > 0 && (data[size - 1] & 0x80)) {
        // 符号扩展到 32 位
        for (int i = size; i < 4; i++) v |= (int32_t)0xFFFFFFFFu << (8 * i);
    }
    return v;
}

static void layout_add_field(HidLayout *L, ReportKind kind, uint16_t rid, uint32_t bit_offset,
                             uint32_t bit_size, int has_usage, uint32_t page, uint32_t usage,
                             int32_t lmin, int32_t lmax) {
    if (L->field_count >= MAX_FIELDS) return;
    HidField *f = &L->fields[L->field_count++];
    f->type = (uint8_t)kind;
    f->report_id = rid;
    f->bit_offset = bit_offset;
    f->bit_size = bit_size;
    f->has_usage = has_usage;
    f->usage_page = page;
    f->usage = usage;
    f->logical_min = lmin;
    f->logical_max = lmax;
}

static void layout_parse(HidLayout *L, const uint8_t *d, size_t len) {
    memset(L, 0, sizeof *L);
    HidGlobalState glob = {0, 0, 0, 0, 0, 0};
    HidGlobalState stack[16];
    int sp = 0;
    uint32_t local_usages[MAX_USAGES];
    int local_count = 0;

    size_t i = 0;
    while (i < len) {
        uint8_t prefix = d[i];
        if (prefix == 0xFE) { // long item
            if (i + 2 >= len) break;
            size_t size = d[i + 1];
            i += 3 + size;
            continue;
        }
        int size = (prefix & 3) == 3 ? 4 : (prefix & 3);
        int type = (prefix >> 2) & 3;
        int tag = (prefix >> 4) & 0xF;
        if (i + 1 + (size_t)size > len) break;
        int32_t val = item_value(&d[i + 1], size);
        i += 1 + (size_t)size;

        if (type == 1) { // Global
            switch (tag) {
                case 0x0: glob.usage_page = (uint32_t)val; break;
                case 0x1: glob.logical_min = val; break;
                case 0x2: glob.logical_max = val; break;
                case 0x7: glob.report_size = (uint32_t)val; break;
                case 0x8: glob.report_id = (uint32_t)(val & 0xFF); break;
                case 0x9: glob.report_count = (uint32_t)val; break;
                case 0xA: if (sp < 16) stack[sp++] = glob; break;
                case 0xB: if (sp > 0) glob = stack[--sp]; break;
                default: break;
            }
        } else if (type == 2) { // Local
            if (tag == 0x0 && local_count < MAX_USAGES) local_usages[local_count++] = (uint32_t)val;
        } else if (type == 0) { // Main
            if (tag == 0x8 || tag == 0x9 || tag == 0xB) {
                ReportKind kind = (tag == 0x8) ? RT_INPUT : (tag == 0x9) ? RT_OUTPUT : RT_FEATURE;
                uint16_t rid = (uint16_t)glob.report_id;
                int idx = kind;
                if (!L->report_used[idx][rid]) { L->report_used[idx][rid] = 1; L->report_bits[idx][rid] = 0; }
                uint32_t base = L->report_bits[idx][rid];
                uint32_t rsize = glob.report_size, rcount = glob.report_count;
                uint32_t total_bits = rsize * rcount;
                uint32_t page = glob.usage_page;
                if (local_count > 0) {
                    int n = local_count < (int)rcount ? local_count : (int)rcount;
                    for (int k = 0; k < n; k++) {
                        layout_add_field(L, kind, rid, base + (uint32_t)k * rsize, rsize,
                                         1, page, local_usages[k], glob.logical_min, glob.logical_max);
                    }
                    if ((uint32_t)n < rcount) {
                        layout_add_field(L, kind, rid, base + (uint32_t)n * rsize,
                                         (rcount - (uint32_t)n) * rsize, 0, page, 0,
                                         glob.logical_min, glob.logical_max);
                    }
                } else {
                    layout_add_field(L, kind, rid, base, total_bits, 0, page, 0,
                                     glob.logical_min, glob.logical_max);
                }
                L->report_bits[idx][rid] = base + total_bits;
                local_count = 0;
            } else if (tag == 0xA || tag == 0xC) {
                local_count = 0; // Collection / End Collection
            } else {
                local_count = 0;
            }
        }
    }
}

static uint32_t layout_report_data_bytes(const HidLayout *L, ReportKind kind, uint16_t rid) {
    return (L->report_bits[kind][rid] + 7) / 8;
}

static int layout_find_field(const HidLayout *L, ReportKind kind, uint32_t page, uint32_t usage,
                             uint16_t report_id_filter, HidField *out) {
    for (int i = 0; i < L->field_count; i++) {
        const HidField *f = &L->fields[i];
        if (f->type != (uint8_t)kind) continue;
        if (report_id_filter != 0xFFFF && f->report_id != report_id_filter) continue;
        if (!f->has_usage) continue;
        if (f->usage_page == page && f->usage == usage) { *out = *f; return 1; }
    }
    return 0;
}

static const char *kind_name(ReportKind k) {
    switch (k) { case RT_INPUT: return "Input"; case RT_OUTPUT: return "Output"; default: return "Feature"; }
}

static void layout_print(const HidLayout *L, int with_input_detail) {
    for (int k = RT_INPUT; k <= RT_FEATURE; k++) {
        ReportKind kind = (ReportKind)k;
        int any = 0;
        for (int rid = 0; rid < 256; rid++) {
            if (!L->report_used[k][rid]) continue;
            if (!any) { log_line("  [%s reports]", kind_name(kind)); any = 1; }
            uint32_t data_bytes = layout_report_data_bytes(L, kind, (uint16_t)rid);
            uint32_t total = data_bytes + (rid ? 1u : 0u);
            log_line("    %s ReportID=0x%02X: data=%u bytes, on-wire=%u bytes (含 ReportID)", kind_name(kind), rid, data_bytes, total);
            if (kind == RT_FEATURE || with_input_detail) {
                for (int i = 0; i < L->field_count; i++) {
                    const HidField *f = &L->fields[i];
                    if (f->type != (uint8_t)kind || f->report_id != rid) continue;
                    char usage_str[48];
                    if (f->has_usage) snprintf(usage_str, sizeof usage_str, "%04X/%04X",
                                               (unsigned)f->usage_page & 0xFFFFu, (unsigned)f->usage & 0xFFFFu);
                    else snprintf(usage_str, sizeof usage_str, "padding");
                    log_line("      bit=%-4u size=%-4u idOffset=%-2u usagePage/Usage=%-10s logical=%d..%d%s",
                             f->bit_offset, f->bit_size, f->bit_offset / 8, usage_str,
                             f->logical_min, f->logical_max,
                             (f->has_usage && f->usage_page == 0x0D && f->usage == 0x52) ? "   <-- Input Mode (多点=2)" : "");
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 设备属性辅助
// ---------------------------------------------------------------------------

static CFNumberRef dev_num_prop(IOHIDDeviceRef d, CFStringRef key) {
    CFTypeRef v = IOHIDDeviceGetProperty(d, key);
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) return (CFNumberRef)v;
    return NULL;
}

static int dev_int_prop(IOHIDDeviceRef d, CFStringRef key, int def) {
    CFNumberRef n = dev_num_prop(d, key);
    int out = def;
    if (n) CFNumberGetValue(n, kCFNumberIntType, &out);
    return out;
}

static const char *dev_str_prop(IOHIDDeviceRef d, CFStringRef key, char *buf, size_t cap) {
    CFTypeRef v = IOHIDDeviceGetProperty(d, key);
    buf[0] = 0;
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) {
        CFStringGetCString((CFStringRef)v, buf, (CFIndex)cap, kCFStringEncodingUTF8);
    }
    return buf;
}

static int dev_bool_prop(IOHIDDeviceRef d, CFStringRef key) {
    CFTypeRef v = IOHIDDeviceGetProperty(d, key);
    if (!v) return -1;
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) return CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0;
    return -2;
}


static int dev_cmp(const void *a, const void *b) {
    IOHIDDeviceRef da = *(IOHIDDeviceRef *)a;
    IOHIDDeviceRef db = *(IOHIDDeviceRef *)b;
    int pa = dev_int_prop(da, CFSTR(kIOHIDPrimaryUsagePageKey), 0);
    int pb = dev_int_prop(db, CFSTR(kIOHIDPrimaryUsagePageKey), 0);
    if (pa != pb) return pa - pb;
    int ua = dev_int_prop(da, CFSTR(kIOHIDPrimaryUsageKey), 0);
    int ub = dev_int_prop(db, CFSTR(kIOHIDPrimaryUsageKey), 0);
    return ua - ub;
}

// 打开 manager 并取回匹配设备（设备 ref 归 manager 持有，调用方负责释放 cfdevs/manager）
static IOHIDManagerRef open_manager(int vid, int pid, CFArrayRef *out_devs, IOReturn *out_kr) {
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!mgr) { if (out_kr) *out_kr = kIOReturnNoResources; return NULL; }
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberIntType, &vid);
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    const void *keys[] = { CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey) };
    const void *vals[] = { nv, np };
    CFDictionaryRef matching = CFDictionaryCreate(NULL, keys, vals, 2,
                                                  &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(mgr, matching);
    CFRelease(matching); CFRelease(nv); CFRelease(np);
    IOReturn kr = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    if (out_kr) *out_kr = kr;
    if (kr != kIOReturnSuccess) { CFRelease(mgr); return NULL; }
    CFSetRef set = IOHIDManagerCopyDevices(mgr);
    if (!set) { if (out_kr) *out_kr = kIOReturnNoDevice; CFRelease(mgr); return NULL; }
    CFIndex n = CFSetGetCount(set);
    IOHIDDeviceRef *arr = (IOHIDDeviceRef *)calloc((size_t)(n > 0 ? n : 1), sizeof(IOHIDDeviceRef));
    if (n > 0) {
        const void **tmp = (const void **)calloc((size_t)n, sizeof(void *));
        CFSetGetValues(set, tmp);
        for (CFIndex i = 0; i < n; i++) arr[i] = (IOHIDDeviceRef)tmp[i];
        free(tmp);
        qsort(arr, (size_t)n, sizeof(IOHIDDeviceRef), dev_cmp);
    }
    CFRelease(set);
    *out_devs = CFArrayCreate(NULL, (const void **)arr, n, &kCFTypeArrayCallBacks);
    free(arr);
    return mgr;
}

static int is_digitizer(IOHIDDeviceRef d) {
    return dev_int_prop(d, CFSTR(kIOHIDPrimaryUsagePageKey), -1) == 0x0D &&
           dev_int_prop(d, CFSTR(kIOHIDPrimaryUsageKey), -1) == 0x04;
}

static int is_mouse(IOHIDDeviceRef d) {
    return dev_int_prop(d, CFSTR(kIOHIDPrimaryUsagePageKey), -1) == 0x01 &&
           dev_int_prop(d, CFSTR(kIOHIDPrimaryUsageKey), -1) == 0x02;
}

static CFDataRef dev_report_descriptor(IOHIDDeviceRef d) {
    CFTypeRef v = IOHIDDeviceGetProperty(d, CFSTR(kIOHIDReportDescriptorKey));
    if (v && CFGetTypeID(v) == CFDataGetTypeID()) return (CFDataRef)v;
    return NULL;
}

static void print_hex_lines(const uint8_t *p, size_t n, const char *indent) {
    char line[128];
    for (size_t i = 0; i < n; i += 16) {
        size_t m = n - i > 16 ? 16 : n - i;
        char *w = line;
        w += snprintf(w, sizeof line - (size_t)(w - line), "%s%04zx: ", indent, i);
        for (size_t j = 0; j < m; j++) w += snprintf(w, sizeof line - (size_t)(w - line), "%02x ", p[i + j]);
        log_line("%s", line);
    }
}

// 列出所有匹配设备
static int run_list(const Options *o) {
    section("枚举 VID=0x%04X PID=0x%04X 的 HID 接口", o->vid, o->pid);
    CFArrayRef devs = NULL;
    IOReturn kr = kIOReturnSuccess;
    IOHIDManagerRef mgr = open_manager(o->vid, o->pid, &devs, &kr);
    if (!mgr) {
        if (kr == kIOReturnNoDevice || kr == kIOReturnNotAttached) {
            log_line("未发现 VID=0x%04X PID=0x%04X 的 HID 设备。", o->vid, o->pid);
            log_line("  请确认 HOUSI 触控屏已连接、VID/PID 正确（可用 --vid/--pid 覆盖默认值）。");
        } else {
            log_line("IOHIDManagerOpen 失败，无法枚举。");
            log_kr("  IOHIDManagerOpen", kr);
            if (kr == kIOReturnNotPermitted)
                log_line("  提示: 在 系统设置 → 隐私与安全性 → 输入监控 中授权当前终端 App，然后重开终端。");
        }
        return 3;
    }
    log_line("IOHIDManagerOpen: kr=0x%08x (success)", kr);
    CFIndex n = devs ? CFArrayGetCount(devs) : 0;
    log_line("匹配到 %ld 个 HID 接口（同一 USB 设备的每个 interface 是一条）", (long)n);
    if (n == 0) {
        log_line("未发现设备。请确认 HOUSI 触控屏已连接（VID=0x%04X PID=0x%04X）。", o->vid, o->pid);
        CFRelease(devs); CFRelease(mgr);
        return 4;
    }
    for (CFIndex i = 0; i < n; i++) {
        IOHIDDeviceRef d = (IOHIDDeviceRef)CFArrayGetValueAtIndex(devs, i);
        char transport[64], product[128], manufacturer[128], serial[128];
        dev_str_prop(d, CFSTR(kIOHIDTransportKey), transport, sizeof transport);
        dev_str_prop(d, CFSTR(kIOHIDProductKey), product, sizeof product);
        dev_str_prop(d, CFSTR(kIOHIDManufacturerKey), manufacturer, sizeof manufacturer);
        dev_str_prop(d, CFSTR(kIOHIDSerialNumberKey), serial, sizeof serial);
        int upp = dev_int_prop(d, CFSTR(kIOHIDPrimaryUsagePageKey), -1);
        int up = dev_int_prop(d, CFSTR(kIOHIDPrimaryUsageKey), -1);
        CFDataRef rd = dev_report_descriptor(d);
        int tcc = dev_bool_prop(d, CFSTR("RequiresTCCAuthorization"));
        log_line("");
        log_line("[%ld] IOHIDDevice transport=%s product=%s manufacturer=%s", (long)i,
                 transport[0] ? transport : "?", product[0] ? product : "?", manufacturer[0] ? manufacturer : "?");
        log_line("    VID=0x%04X PID=0x%04X PrimaryUsagePage=0x%02X PrimaryUsage=0x%02X%s%s",
                 dev_int_prop(d, CFSTR(kIOHIDVendorIDKey), 0), dev_int_prop(d, CFSTR(kIOHIDProductIDKey), 0),
                 upp, up, is_digitizer(d) ? "  <-- Digitizer (0x0D/0x04)" : "",
                 is_mouse(d) ? "  <-- Mouse (0x01/0x02)" : "");
        log_line("    Version=%d LocationID=0x%08X Serial=%s RequiresTCCAuthorization=%s",
                 dev_int_prop(d, CFSTR(kIOHIDVersionNumberKey), 0),
                 dev_int_prop(d, CFSTR(kIOHIDLocationIDKey), 0),
                 serial[0] ? serial : "-",
                 tcc == 1 ? "YES" : (tcc == 0 ? "no" : "n/a"));
        log_line("    ReportDescriptor=%ld bytes MaxInputReportSize=%d MaxOutputReportSize=%d MaxFeatureReportSize=%d",
                 rd ? (long)CFDataGetLength(rd) : -1L,
                 dev_int_prop(d, CFSTR(kIOHIDMaxInputReportSizeKey), -1),
                 dev_int_prop(d, CFSTR(kIOHIDMaxOutputReportSizeKey), -1),
                 dev_int_prop(d, CFSTR(kIOHIDMaxFeatureReportSizeKey), -1));
        if (rd) {
            HidLayout L;
            layout_parse(&L, CFDataGetBytePtr(rd), (size_t)CFDataGetLength(rd));
            layout_print(&L, 0);
            if (o->descriptor) print_hex_lines(CFDataGetBytePtr(rd), (size_t)CFDataGetLength(rd), "      ");
        }
    }
    CFRelease(devs);
    CFRelease(mgr);
    return 0;
}

// ---------------------------------------------------------------------------
// Feature 元素 / 当前值
// ---------------------------------------------------------------------------

static CFArrayRef copy_feature_elements(IOHIDDeviceRef d) {
    int type = kIOHIDElementTypeFeature;
    CFNumberRef tn = CFNumberCreate(NULL, kCFNumberIntType, &type);
    const void *keys[] = { CFSTR(kIOHIDElementTypeKey) };
    const void *vals[] = { tn };
    CFDictionaryRef matching = CFDictionaryCreate(NULL, keys, vals, 1,
                                                  &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    CFArrayRef arr = IOHIDDeviceCopyMatchingElements(d, matching, 0);
    CFRelease(matching); CFRelease(tn);
    return arr;
}


static int get_element_int(IOHIDDeviceRef d, IOHIDElementRef e, long *out) {
    if (!e) return 0;
    IOHIDValueRef v = NULL;
    IOReturn kr = IOHIDDeviceGetValue(d, e, &v);
    if (kr != kIOReturnSuccess || !v) { if (v) CFRelease(v); return 0; }
    *out = (long)IOHIDValueGetIntegerValue(v);
    CFRelease(v);
    return 1;
}

// 本机实测: 对 vendor-defined (0xFF00) 的 feature 元素读 IOHIDDeviceGetValue 后再 Close 会让进程
// 崩溃（很可能固件对 GET_REPORT 的大块 vendor 数据有副作用），因此只读标准 usage page 的元素。
static int element_safe_to_read(IOHIDElementRef e) {
    uint32_t page = IOHIDElementGetUsagePage(e);
    long size = (long)IOHIDElementGetReportSize(e);
    long count = (long)IOHIDElementGetReportCount(e);
    if (page == 0xFF00 || page == 0xFF01 || page == 0xFF02) return 0;
    if (size * count > 512) return 0;
    return 1;
}

static void dump_feature_elements(IOHIDDeviceRef d, CFArrayRef els, long *mode_out, long *devid_out) {
    log_line("  IOHIDDeviceCopyMatchingElements(type=Feature) -> %ld 个元素",
             els ? (long)CFArrayGetCount(els) : 0L);
    if (els) {
        for (CFIndex i = 0; i < CFArrayGetCount(els); i++) {
            IOHIDElementRef e = (IOHIDElementRef)CFArrayGetValueAtIndex(els, i);
            long val = 0;
            int has = element_safe_to_read(e) ? get_element_int(d, e, &val) : 0;
            char cur[48] = "-";
            if (has) snprintf(cur, sizeof cur, "%ld", val);
            else if (!element_safe_to_read(e)) snprintf(cur, sizeof cur, "(vendor/大块数据不读取)");
            log_line("    Feature[%ld] reportID=%d usagePage=0x%02X usage=0x%02X size=%ld count=%ld logical=%ld..%ld current=%s",
                     (long)i, IOHIDElementGetReportID(e),
                     IOHIDElementGetUsagePage(e), IOHIDElementGetUsage(e),
                     (long)IOHIDElementGetReportSize(e), (long)IOHIDElementGetReportCount(e),
                     (long)IOHIDElementGetLogicalMin(e), (long)IOHIDElementGetLogicalMax(e), cur);
            if (has && IOHIDElementGetUsagePage(e) == 0x0D) {
                if (IOHIDElementGetUsage(e) == 0x52 && mode_out) *mode_out = val;
                if (IOHIDElementGetUsage(e) == 0x53 && devid_out) *devid_out = val;
            } else if (!has && element_safe_to_read(e) && IOHIDElementGetUsagePage(e) == 0x0D) {
                if (IOHIDElementGetUsage(e) == 0x52 && mode_out) *mode_out = -2;
                if (IOHIDElementGetUsage(e) == 0x53 && devid_out) *devid_out = -2;
            }
        }
    }
}

// 本机实测（macOS 27.2.1 / AppleUserHIDDevice）: 同一个进程内对 feature 元素做「多轮」
// IOHIDDeviceGetValue（例如 get→release 后再 get 同一元素）会偶发/必然崩溃，而「对元素数组顺序
// 遍历一轮、逐个 GetValue」是稳定的。因此所有子进程都只在一次顺序遍历中读取 feature 值并缓存在
// 本结构里，后续不再重复 GetValue。
typedef struct { uint32_t page, usage; long value; } FeatureValue;

static int read_feature_values(IOHIDDeviceRef d, CFArrayRef els, FeatureValue *cache, int cap, int *count) {
    int n = 0;
    if (!els) { if (count) *count = 0; return 0; }
    for (CFIndex i = 0; i < CFArrayGetCount(els); i++) {
        IOHIDElementRef e = (IOHIDElementRef)CFArrayGetValueAtIndex(els, i);
        if (IOHIDElementGetType(e) != kIOHIDElementTypeFeature) continue;
        if (!element_safe_to_read(e)) continue;
        long v = 0;
        if (!get_element_int(d, e, &v)) continue;
        if (cache && n < cap) {
            cache[n].page = IOHIDElementGetUsagePage(e);
            cache[n].usage = IOHIDElementGetUsage(e);
            cache[n].value = v;
            n++;
        }
    }
    if (count) *count = n;
    return n;
}

static int cache_lookup(const FeatureValue *cache, int count, uint32_t page, uint32_t usage, long *out) {
    for (int i = 0; i < count; i++) {
        if (cache[i].page == page && cache[i].usage == usage) { if (out) *out = cache[i].value; return 1; }
    }
    return 0;
}

// 当前 digitizer 的 Input Mode (0x0D/0x52) / Device Identifier (0x0D/0x53)
static int read_mode_values(IOHIDDeviceRef d, CFArrayRef els, long *mode, long *devid) {
    FeatureValue cache[64];
    int n = 0;
    read_feature_values(d, els, cache, 64, &n);
    long m = -1, id = -1;
    int ok = cache_lookup(cache, n, 0x0D, 0x52, &m);
    cache_lookup(cache, n, 0x0D, 0x53, &id);
    if (mode) *mode = m;
    if (devid) *devid = id;
    return ok;
}

static void get_report_print(IOHIDDeviceRef d, CFIndex rid, CFIndex want_len) {
    uint8_t *buf = (uint8_t *)calloc((size_t)(want_len > 0 ? want_len : 1), 1);
    CFIndex len = want_len;
    IOReturn kr = IOHIDDeviceGetReport(d, kIOHIDReportTypeFeature, rid, buf, &len);
    log_line("IOHIDDeviceGetReport(Feature, reportID=0x%02lX, buffer=%ld) -> kr=0x%08x len=%ld",
             (long)rid, (long)want_len, kr, (long)len);
    if (kr == kIOReturnSuccess) {
        char hex[3 * 64 + 1];
        size_t p = 0;
        for (CFIndex i = 0; i < len && i < 64; i++) p += (size_t)snprintf(hex + p, sizeof hex - p, "%02x ", buf[i]);
        hex[p] = 0;
        log_line("  raw = %s", hex);
        if (len > 0 && (CFIndex)buf[0] != rid) {
            log_line("  注意: 返回的第一字节 0x%02X 与请求的 reportID 0x%02lX 不一致；本机固件实测对任意 report ID", buf[0], (long)rid);
            log_line("        都返回同一段 3 字节数据（0x11=Contact Count Maximum 的 report），GetReport 回读不可用于校验 0x13。");
        }
    }
    free(buf);
}

// ---------------------------------------------------------------------------
// 打开 Digitizer（子进程用）
// ---------------------------------------------------------------------------

typedef struct {
    IOHIDManagerRef mgr;
    CFArrayRef devs;
    IOHIDDeviceRef digitizer;
} DevSession;

static int ds_open(DevSession *s, const Options *o) {
    memset(s, 0, sizeof *s);
    IOReturn kr = kIOReturnSuccess;
    s->mgr = open_manager(o->vid, o->pid, &s->devs, &kr);
    if (!s->mgr) {
        log_line("IOHIDManagerOpen 失败: kr=0x%08x", kr);
        if (kr == kIOReturnNotPermitted) {
            log_line("  原因: Input Monitoring 未授权（kIOReturnNotPermitted）。");
            log_line("  处理: 系统设置 → 隐私与安全性 → 输入监控 → 勾选运行本程序的终端 App；完全退出并重开终端。");
        }
        return 0;
    }
    CFIndex n = CFArrayGetCount(s->devs);
    for (CFIndex i = 0; i < n; i++) {
        IOHIDDeviceRef d = (IOHIDDeviceRef)CFArrayGetValueAtIndex(s->devs, i);
        if (is_digitizer(d)) { s->digitizer = d; break; }
    }
    if (!s->digitizer) {
        log_line("未找到 Digitizer 接口 (usagePage=0x0D, usage=0x04)；共匹配 %ld 个接口。", (long)n);
        log_line("提示: HOUSI 触控屏是否已连接？hidprobe --list 可查看全部接口。");
        return 0;
    }
    IOReturn okr = IOHIDDeviceOpen(s->digitizer, kIOHIDOptionsTypeNone);
    log_line("IOHIDDeviceOpen(digitizer): kr=0x%08x %s", okr, okr == kIOReturnSuccess ? "(success)" : "");
    if (okr != kIOReturnSuccess) {
        if (okr == kIOReturnNotPermitted)
            log_line("  原因: Input Monitoring 未授权；SetReport/GetReport 需要该权限。");
        return 0;
    }
    return 1;
}

// 子进程成功路径: 不执行 IOHIDDeviceClose/CFRelease（本机实测偶发 SIGTRAP），直接退出。
static void child_exit_ok(void) { fflush(stdout); _exit(0); }

static void ds_close(DevSession *s) {
    if (s->mgr) {
        if (s->digitizer) IOHIDDeviceClose(s->digitizer, kIOHIDOptionsTypeNone);
        CFRelease(s->devs);
        CFRelease(s->mgr);
        s->mgr = NULL;
    }
}

// ---------------------------------------------------------------------------
// 子命令: --read-mode / --inspect / --get-report / --write-mode
// ---------------------------------------------------------------------------

static int child_read_mode(const Options *o, int verbose) {
    DevSession s;
    if (!ds_open(&s, o)) return 4;
    CFArrayRef els = copy_feature_elements(s.digitizer);
    long mode = -1, devid = -1;
    int ok = read_mode_values(s.digitizer, els, &mode, &devid);
    if (verbose) {
        log_line("digitizer current Input Mode (0x0D/0x52) = %ld, Device Identifier (0x0D/0x53) = %ld", mode, devid);
    }
    if (els) CFRelease(els);
    if (!ok) { log_line("READ_MODE error=no-mode-element"); ds_close(&s); return 5; }
    log_line("READ_MODE mode=%ld devid=%ld", mode, devid);
    child_exit_ok();
    return 0;
}

// 只做一次 Open + Close，不调用 GetValue/GetReport（本机后两者之后再 Close 可能崩溃）
static int child_open_close(const Options *o) {
    DevSession s;
    if (!ds_open(&s, o)) return 4;
    IOReturn ckr = IOHIDDeviceClose(s.digitizer, kIOHIDOptionsTypeNone);
    log_line("IOHIDDeviceClose(digitizer): kr=0x%08x %s", ckr, ckr == kIOReturnSuccess ? "(success)" : "(FAILED)");
    fflush(stdout);
    _exit(ckr == kIOReturnSuccess ? 0 : 8);
}

static int child_inspect(const Options *o) {
    DevSession s;
    if (!ds_open(&s, o)) return 4;
    CFDataRef rd = dev_report_descriptor(s.digitizer);
    if (rd) {
        HidLayout L;
        layout_parse(&L, CFDataGetBytePtr(rd), (size_t)CFDataGetLength(rd));
        log_line("Digitizer ReportDescriptor = %ld bytes", (long)CFDataGetLength(rd));
        layout_print(&L, 0);
        if (o->descriptor) print_hex_lines(CFDataGetBytePtr(rd), (size_t)CFDataGetLength(rd), "    ");
        HidField f;
        if (layout_find_field(&L, RT_FEATURE, 0x0D, 0x52, 0xFFFF, &f)) {
            log_line("定位 Input Mode: Feature ReportID=0x%02X, bitOffset=%u (byte %u), size=%u bits, logical=%d..%d",
                     f.report_id, f.bit_offset, f.bit_offset / 8, f.bit_size, f.logical_min, f.logical_max);
            log_line("该 report 数据长度 = %u 字节（含 ReportID 共 %u 字节）",
                     layout_report_data_bytes(&L, RT_FEATURE, f.report_id),
                     layout_report_data_bytes(&L, RT_FEATURE, f.report_id) + 1);
        } else {
            log_line("描述符中未找到 Feature 的 Input Mode (0x0D/0x52)");
        }
    } else {
        log_line("无法读取 ReportDescriptor 属性");
    }
    log_line("");
    long mode = -999, devid = -999;
    CFArrayRef els = copy_feature_elements(s.digitizer);
    dump_feature_elements(s.digitizer, els, &mode, &devid);
    if (els) CFRelease(els);
    log_line("");
    log_line("当前 Input Mode (0x0D/0x52) = %s%ld, Device Identifier (0x0D/0x53) = %ld",
             (mode == -999 || mode == -2) ? "(read failed) " : "", mode, devid);
    if (rd) {
        HidLayout L;
        layout_parse(&L, CFDataGetBytePtr(rd), (size_t)CFDataGetLength(rd));
        HidField f;
        if (layout_find_field(&L, RT_FEATURE, 0x0D, 0x52, 0xFFFF, &f)) {
            CFIndex len = (CFIndex)layout_report_data_bytes(&L, RT_FEATURE, f.report_id) + 1;
            get_report_print(s.digitizer, f.report_id, len);
        }
    }
    log_line("");
    log_line("IOHIDDeviceClose: 本进程执行过 GetValue/GetReport，为规避本机 IOHIDLib 崩溃怪癖不调用 Close；");
    log_line("  进程退出时由系统回收。Open/Close 返回值可看 --open-close 模式。");
    child_exit_ok();
    return 0;
}

static int child_get_report(const Options *o) {
    DevSession s;
    if (!ds_open(&s, o)) return 4;
    CFDataRef rd = dev_report_descriptor(s.digitizer);
    HidLayout L;
    HidField f;
    int have_field = 0;
    if (rd) {
        layout_parse(&L, CFDataGetBytePtr(rd), (size_t)CFDataGetLength(rd));
        have_field = layout_find_field(&L, RT_FEATURE, 0x0D, 0x52, 0xFFFF, &f);
    }
    CFIndex rid = o->get_report_id >= 0 ? o->get_report_id : (have_field ? f.report_id : 0x13);
    CFIndex len = 1;
    if (have_field && (int)rid == (int)f.report_id)
        len = (CFIndex)layout_report_data_bytes(&L, RT_FEATURE, (uint16_t)rid) + 1;
    else
        len = dev_int_prop(s.digitizer, CFSTR(kIOHIDMaxFeatureReportSizeKey), 64);
    if (len > 1024) len = 1024;
    get_report_print(s.digitizer, rid, len);
    child_exit_ok();
    return 0;
}

static int child_write_mode(const Options *o) {
    DevSession s;
    if (!ds_open(&s, o)) return 4;
    CFDataRef rd = dev_report_descriptor(s.digitizer);
    if (!rd) { log_line("无法读取 ReportDescriptor"); ds_close(&s); return 5; }
    HidLayout L;
    layout_parse(&L, CFDataGetBytePtr(rd), (size_t)CFDataGetLength(rd));
    HidField mode_field, devid_field;
    int have_mode = layout_find_field(&L, RT_FEATURE, 0x0D, 0x52, 0xFFFF, &mode_field);
    int have_devid = layout_find_field(&L, RT_FEATURE, 0x0D, 0x53, 0xFFFF, &devid_field);
    if (!have_mode) { log_line("描述符中未找到 Input Mode (0x0D/0x52) Feature 字段"); ds_close(&s); return 5; }
    if (mode_field.bit_size > 8 || (mode_field.bit_offset % 8) != 0) {
        log_line("Input Mode 字段不是字节对齐的 8-bit 字段，本工具暂不支持（size=%u bitOffset=%u）", mode_field.bit_size, mode_field.bit_offset);
        ds_close(&s); return 5;
    }
    CFArrayRef els = copy_feature_elements(s.digitizer);
    FeatureValue cache[64];
    int cache_count = 0;
    read_feature_values(s.digitizer, els, cache, 64, &cache_count);
    long orig_mode = -1, orig_devid = -1;
    cache_lookup(cache, cache_count, 0x0D, 0x52, &orig_mode);
    cache_lookup(cache, cache_count, 0x0D, 0x53, &orig_devid);
    log_line("写入前: Input Mode=%ld, Device Identifier=%ld（来自 IOHIDDeviceGetValue，单次顺序遍历缓存）",
             orig_mode, orig_devid);

    uint16_t rid = mode_field.report_id;
    uint32_t data_bytes = layout_report_data_bytes(&L, RT_FEATURE, rid);
    uint8_t *buf = (uint8_t *)calloc((size_t)data_bytes + 1, 1);
    buf[0] = (uint8_t)rid;
    // 用当前元素值填充该 feature report 的所有已知字段，尽量只改动 Input Mode
    for (int i = 0; i < L.field_count; i++) {
        HidField *f = &L.fields[i];
        if (f->type != RT_FEATURE || f->report_id != rid || !f->has_usage) continue;
        if ((f->bit_offset % 8) != 0 || f->bit_size > 8) continue;
        uint32_t off = 1 + f->bit_offset / 8;
        if (off >= data_bytes + 1) continue;
        long v = 0;
        cache_lookup(cache, cache_count, f->usage_page, f->usage, &v);
        buf[off] = (uint8_t)(v & 0xFF);
    }
    uint32_t mode_off = 1 + mode_field.bit_offset / 8;
    buf[mode_off] = (uint8_t)(o->mode & 0xFF);
    if (have_devid && orig_devid >= 0) {
        uint32_t off = 1 + devid_field.bit_offset / 8;
        if (off <= data_bytes) buf[off] = (uint8_t)(orig_devid & 0xFF);
    }
    char hex[3 * 64 + 1];
    size_t p = 0;
    for (uint32_t i = 0; i <= data_bytes && i < 64; i++)
        p += (size_t)snprintf(hex + p, sizeof hex - p, "%02x ", buf[i]);
    hex[p] = 0;
    log_line("构造 Feature Report: reportID=0x%02X, 总长度=%u 字节（含 ReportID）: %s", rid, data_bytes + 1, hex);
    log_line("  buffer[0]=ReportID=0x%02X, data byte[%u] (Input Mode) = %d", rid, mode_field.bit_offset / 8, o->mode);
    if (els) CFRelease(els);
    if (o->dry_run) {
        log_line("DRY_RUN: 未调用 IOHIDDeviceSetReport");
        free(buf);
        child_exit_ok();
        return 0;
    }
    IOReturn kr = IOHIDDeviceSetReport(s.digitizer, kIOHIDReportTypeFeature, rid, buf, (CFIndex)data_bytes + 1);
    log_kr("IOHIDDeviceSetReport(Feature, 0x13, buffer[0]=ReportID)", kr);
    if (kr != kIOReturnSuccess) {
        // 退而求其次：某些实现要求 buffer 不带 ReportID（长度 = data bytes）
        log_line("首次 SetReport 失败，尝试不带 ReportID 的 buffer（长度=%u）", data_bytes);
        IOReturn kr2 = IOHIDDeviceSetReport(s.digitizer, kIOHIDReportTypeFeature, rid, buf + 1, (CFIndex)data_bytes);
        log_kr("IOHIDDeviceSetReport(Feature, data-only buffer)", kr2);
        if (kr2 == kIOReturnSuccess) kr = kr2;
    }
    log_line("SetReport 结果: %s", kr == kIOReturnSuccess ? "SUCCESS" : "FAILED");
    fflush(stdout);
    free(buf);
    // 不调用 ds_close(): 改变 Input Mode 会使设备重新枚举，继续访问旧 ref 可能崩溃。
    // 直接退出进程，让 --setmode 父进程用全新的子进程回读。
    usleep(300000);
    fflush(NULL);
    _exit(0);
}

// ---------------------------------------------------------------------------
// 子进程编排（父进程用）
// ---------------------------------------------------------------------------

static char g_self_path[4096];

static int run_child(char *const argv[], char **captured, ssize_t *captured_len) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, g_self_path, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) { close(fds[0]); return -1; }
    size_t cap = 8192, len = 0;
    char *buf = (char *)malloc(cap);
    for (;;) {
        if (len + 4096 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        ssize_t n = read(fds[0], buf + len, cap - len - 1);
        if (n <= 0) break;
        len += (size_t)n;
    }
    buf[len] = 0;
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    // 转发每一行到父进程日志（带时间戳）。用副本切分，保留 buf 供上层解析。
    char *relay = strdup(buf);
    char *save = NULL;
    for (char *line = strtok_r(relay, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        log_line("  %s", line);
    }
    free(relay);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (captured) { *captured = buf; if (captured_len) *captured_len = (ssize_t)len; }
    else free(buf);
    return code;
}

static char **build_child_argv(const Options *o, const char *mode_arg, const char *val, int *n_out) {
    static char vidbuf[32], pidbuf[32], modebuf[32];
    char **argv = (char **)calloc(10, sizeof(char *));
    int n = 0;
    snprintf(vidbuf, sizeof vidbuf, "0x%x", o->vid);
    snprintf(pidbuf, sizeof pidbuf, "0x%x", o->pid);
    argv[n++] = g_self_path;
    argv[n++] = (char *)mode_arg;
    if (val) { snprintf(modebuf, sizeof modebuf, "%s", val); argv[n++] = modebuf; }
    argv[n++] = "--vid"; argv[n++] = vidbuf;
    argv[n++] = "--pid"; argv[n++] = pidbuf;
    if (o->descriptor) { argv[n++] = "--descriptor"; }
    if (o->dry_run) { argv[n++] = "--dry-run"; }
    argv[n] = NULL;
    if (n_out) *n_out = n;
    return argv;
}

static int read_mode_child(const Options *o, long *mode, long *devid) {
    char **argv = build_child_argv(o, "--read-mode", NULL, NULL);
    char *out = NULL;
    ssize_t outlen = 0;
    int code = run_child(argv, &out, &outlen);
    free(argv);
    int ok = 0;
    if (out) {
        char *p = strstr(out, "READ_MODE mode=");
        if (p) {
            long m = 0, d = 0;
            if (sscanf(p, "READ_MODE mode=%ld devid=%ld", &m, &d) == 2) {
                if (mode) *mode = m;
                if (devid) *devid = d;
                ok = 1;
            }
        }
        free(out);
    }
    if (!ok) log_line("子进程读取 Input Mode 失败 (exit=%d)", code);
    return ok;
}

static int orchestrate_setmode(const Options *o) {
    section("SetMode: 把 Digitizer 的 Input Mode (0x0D/0x52) 设为 %d", o->mode);
    if (o->mode < 0 || o->mode > 10) {
        log_line("警告: 目标 mode=%d 超出描述符逻辑范围 0..10", o->mode);
    }
    print_permission_state();
    log_line("");

    log_line("--- STEP 0: 子进程验证 IOHIDDeviceOpen / IOHIDDeviceClose 返回值 ---");
    {
        char **argv = build_child_argv(o, "--open-close", NULL, NULL);
        int code = run_child(argv, NULL, NULL);
        free(argv);
        if (code != 0) log_line("open-close 子进程失败 (exit=%d)", code);
    }

    log_line("");
    log_line("--- STEP 1: 子进程只读检查（feature 元素 dump / 描述符定位 / GetReport）---");
    {
        char **argv = build_child_argv(o, "--inspect", NULL, NULL);
        int code = run_child(argv, NULL, NULL);
        free(argv);
        if (code != 0) {
            log_line("inspect 子进程失败 (exit=%d)，终止 setmode。", code);
            return code;
        }
    }

    log_line("");
    log_line("--- STEP 2: 子进程读取当前 Input Mode（重新打开设备的权威值）---");
    long orig_mode = -1, orig_devid = -1;
    if (!read_mode_child(o, &orig_mode, &orig_devid)) {
        log_line("无法读取当前 Input Mode，终止。");
        return 6;
    }
    log_line("当前 Input Mode = %ld (multi-touch=%s), Device Identifier = %ld",
             orig_mode, orig_mode == 2 ? "是" : "否", orig_devid);

    int need_write = (orig_mode != (long)o->mode) || o->force;
    if (!need_write) {
        log_line("当前 Input Mode 已等于目标值 %d，跳过写入（如仍要发送一次 SetReport，用 --force）。", o->mode);
    } else if (o->dry_run) {
        log_line("--- STEP 3: --dry-run，只打印将发送的 report，不写设备 ---");
        char **argv = build_child_argv(o, "--write-mode", NULL, NULL);
        int code = run_child(argv, NULL, NULL);
        free(argv);
        if (code != 0) log_line("write-mode 子进程失败 (exit=%d)", code);
    } else {
        log_line("");
        log_line("--- STEP 3: 子进程发送 IOHIDDeviceSetReport（Input Mode = %d）---", o->mode);
        char valbuf[16];
        snprintf(valbuf, sizeof valbuf, "%d", o->mode);
        char **argv = build_child_argv(o, "--write-mode", valbuf, NULL);
        int code = run_child(argv, NULL, NULL);
        free(argv);
        if (code != 0) log_line("write-mode 子进程失败 (exit=%d)", code);
    }

    if (!need_write && o->dry_run) {
        log_line("");
        log_line("完成（未写设备）。");
        return 0;
    }

    log_line("");
    log_line("--- STEP 4: 用全新子进程回读 Input Mode（每次 SetReport 后设备会重新枚举）---");
    int verified = 0;
    long rb_mode = -1, rb_devid = -1;
    for (int attempt = 1; attempt <= 6; attempt++) {
        usleep(300000);
        log_line("回读尝试 %d/6 ...", attempt);
        if (read_mode_child(o, &rb_mode, &rb_devid)) {
            log_line("回读: Input Mode = %ld, Device Identifier = %ld", rb_mode, rb_devid);
            if (rb_mode == (long)o->mode) { verified = 1; break; }
        }
    }
    log_line("验证结果: %s (期望 Input Mode=%d, 实际=%ld)", verified ? "MATCH" : "MISMATCH", o->mode, rb_mode);

    log_line("");
    log_line("--- STEP 5: IOHIDDeviceGetReport 回读（原始 API 返回值，见上方已知固件怪癖）---");
    {
        char **argv = build_child_argv(o, "--get-report", NULL, NULL);
        run_child(argv, NULL, NULL);
        free(argv);
    }

    if (o->restore && orig_mode >= 0 && orig_mode != (long)o->mode) {
        log_line("");
        log_line("--- RESTORE: 恢复原 Input Mode = %ld ---", orig_mode);
        char valbuf[16];
        snprintf(valbuf, sizeof valbuf, "%ld", orig_mode);
        char **argv = build_child_argv(o, "--write-mode", valbuf, NULL);
        int code = run_child(argv, NULL, NULL);
        free(argv);
        if (code != 0) log_line("restore write 子进程失败 (exit=%d)", code);
        long rmode = -1;
        int restored = 0;
        for (int attempt = 1; attempt <= 6; attempt++) {
            usleep(300000);
            if (read_mode_child(o, &rmode, NULL)) { restored = 1; break; }
        }
        if (restored) log_line("恢复后 Input Mode = %ld", rmode);
        else log_line("恢复后 Input Mode 回读失败");
    }

    log_line("");
    if (verified) log_line("RESULT: OK — Input Mode=%d 已生效。", o->mode);
    else log_line("RESULT: FAILED — 未能确认 Input Mode=%d。", o->mode);
    return verified ? 0 : 7;
}

// ---------------------------------------------------------------------------
// --dump: input report 打印
// ---------------------------------------------------------------------------

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

typedef struct {
    double start;
    double second_start;
    unsigned printed_this_sec;
    unsigned suppressed_this_sec;
    uint64_t total;
    uint64_t counts[256];
    uint8_t last[4096];
    CFIndex last_len;
    uint32_t last_rid;
    uint64_t identical;
    int have_last;
    int filter_digitizer;
    int filter_mouse;
} DumpCtx;

static void hex_string(char *out, size_t cap, const uint8_t *p, size_t n, size_t maxn) {
    size_t o = 0;
    for (size_t i = 0; i < n && i < maxn; i++)
        o += (size_t)snprintf(out + o, cap - o, "%02x ", p[i]);
    if (n > maxn) snprintf(out + o, cap - o, "...");
}

static void decode_digitizer_payload(const uint8_t *p, size_t n, char *out, size_t cap) {
    out[0] = 0;
    if (n < 63) { snprintf(out, cap, "(payload %zu bytes, 不足 63，无法按本控制器布局解码)", n); return; }
    unsigned contact_count = p[60];
    unsigned scan = (unsigned)p[61] | ((unsigned)p[62] << 8);
    size_t o = (size_t)snprintf(out, cap, "contactCount=%u scanTime=%u fingers=[", contact_count, scan);
    int printed = 0;
    for (int f = 0; f < 10; f++) {
        const uint8_t *q = p + f * 6;
        int tip = q[0] & 1;
        int id = q[1];
        int x = q[2] | (q[3] << 8);
        int y = q[4] | (q[5] << 8);
        if (!tip && f >= (int)contact_count) continue;
        if (printed++) o += (size_t)snprintf(out + o, cap - o, " ");
        o += (size_t)snprintf(out + o, cap - o, "%d:tip=%d,id=%d,x=%d,y=%d", f, tip, id, x, y);
        if (o > cap - 64) break;
    }
    snprintf(out + o, cap - o, "]");
}

static void dump_callback(void *context, IOReturn result, void *sender, IOHIDReportType type,
                          uint32_t reportID, uint8_t *report, CFIndex reportLength) {
    DumpCtx *ctx = (DumpCtx *)context;
    IOHIDDeviceRef dev = (IOHIDDeviceRef)sender;
    if (ctx->filter_digitizer && !is_digitizer(dev)) return;
    if (ctx->filter_mouse && !is_mouse(dev)) return;
    (void)type;
    if (result != kIOReturnSuccess) {
        log_line("input report error: kr=0x%08x reportID=0x%02X", result, reportID);
        return;
    }
    ctx->total++;
    if (reportID < 256) ctx->counts[reportID]++;

    // 判断 report 缓冲区是否包含 Report ID 首字节
    int has_id = (reportLength > 0 && report[0] == (uint8_t)reportID);
    const uint8_t *payload = has_id ? report + 1 : report;
    CFIndex plen = has_id ? reportLength - 1 : reportLength;

    if (ctx->have_last && ctx->last_len == reportLength && ctx->last_rid == reportID &&
        memcmp(ctx->last, report, (size_t)reportLength) == 0) {
        ctx->identical++;
        return;
    }
    if (ctx->identical > 0) {
        log_line("    (previous identical report repeated x%llu)", (unsigned long long)ctx->identical);
        ctx->identical = 0;
    }
    if (reportLength <= (CFIndex)sizeof ctx->last) {
        memcpy(ctx->last, report, (size_t)reportLength);
        ctx->last_len = reportLength;
        ctx->last_rid = reportID;
        ctx->have_last = 1;
    } else {
        ctx->have_last = 0;
    }

    double now = CFAbsoluteTimeGetCurrent();
    if (now - ctx->second_start >= 1.0) {
        if (ctx->suppressed_this_sec > 0)
            log_line("    (上一秒限频丢弃 %u 条报告)", ctx->suppressed_this_sec);
        ctx->suppressed_this_sec = 0;
        ctx->printed_this_sec = 0;
        ctx->second_start = now;
    }
    if (ctx->printed_this_sec >= 20) { ctx->suppressed_this_sec++; return; }
    ctx->printed_this_sec++;

    char hex[3 * 128 + 8];
    hex_string(hex, sizeof hex, report, (size_t)reportLength, 128);
    log_line("t=%8.3fs reportID=0x%02X len=%ld%s hex=%s",
             now - ctx->start, reportID, (long)reportLength, has_id ? "(含ID)" : "", hex);
    if (reportID == 0x54) {
        char dec[1024];
        decode_digitizer_payload(payload, (size_t)plen, dec, sizeof dec);
        log_line("    %s", dec);
    }
}

static int run_dump(const Options *o) {
    section("Dump: 读取 Digitizer input report（%s）",
            o->iface_all ? "所有接口" : (o->iface_mouse ? "Mouse 接口" : "Digitizer 接口"));
    print_permission_state();
    log_line("");

    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    int vid = o->vid, pid = o->pid;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberIntType, &vid);
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    const void *keys[] = { CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey) };
    const void *vals[] = { nv, np };
    CFDictionaryRef matching = CFDictionaryCreate(NULL, keys, vals, 2,
                                                  &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(mgr, matching);
    CFRelease(matching); CFRelease(nv); CFRelease(np);

    DumpCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.start = CFAbsoluteTimeGetCurrent();
    ctx.second_start = ctx.start;
    ctx.filter_digitizer = !o->iface_all && !o->iface_mouse;
    ctx.filter_mouse = o->iface_mouse;

    IOHIDManagerRegisterInputReportCallback(mgr, dump_callback, &ctx);
    IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOReturn kr = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    log_kr("IOHIDManagerOpen", kr);
    if (kr != kIOReturnSuccess) {
        if (kr == kIOReturnNotPermitted)
            log_line("Input Monitoring 未授权，无法接收 input report。按上方提示授权后重试。");
        CFRelease(mgr);
        return 4;
    }
    CFSetRef set = IOHIDManagerCopyDevices(mgr);
    log_line("已打开 %ld 个匹配接口；开始监听 input report。", set ? (long)CFSetGetCount(set) : 0L);
    if (set) CFRelease(set);
    if (o->seconds > 0)
        log_line("监听 %d 秒（--seconds 0 = Ctrl-C 退出）。现在触摸 HOUSI 屏幕……", o->seconds);
    else
        log_line("持续监听（Ctrl-C 退出）。现在触摸 HOUSI 屏幕……");

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    double deadline = (o->seconds > 0) ? ctx.start + o->seconds : 0;
    while (!g_stop) {
        if (deadline > 0 && CFAbsoluteTimeGetCurrent() >= deadline) break;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
    }
    log_line("");
    log_line("=== DUMP 汇总 ===");
    log_line("总 input report 数 = %llu", (unsigned long long)ctx.total);
    for (int rid = 0; rid < 256; rid++) {
        if (ctx.counts[rid]) log_line("  reportID=0x%02X: %llu 条", rid, (unsigned long long)ctx.counts[rid]);
    }
    if (ctx.identical > 0) log_line("  最后一条相同报告重复 x%llu", (unsigned long long)ctx.identical);
    if (ctx.total == 0) {
        log_line("");
        log_line("未收到任何 input report。可能原因:");
        log_line("  1) 你没有触摸 HOUSI 屏幕（该控制器只在触摸时发报告）；");
        log_line("  2) Input Monitoring 未授权 —— 设备能枚举但读不到报告（见上方权限状态）；");
        log_line("  3) 设备被其它驱动独占，或接口不在 Digitizer 模式。");
        log_line("可先用 hidprobe --list 确认 Digitizer 接口存在，再触摸屏幕重试。");
    }
    IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    CFRelease(mgr);
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    uint32_t sz = sizeof g_self_path;
    if (_NSGetExecutablePath(g_self_path, &sz) != 0) snprintf(g_self_path, sizeof g_self_path, "%s", argv[0]);

    Options o = parse_args(argc, argv);

    // 内部子命令：纯文本输出，由父进程加时间戳
    if (o.child_inspect || o.child_open_close || o.child_read_mode || o.child_get_report || o.child_write_mode) {
        g_raw_output = 1;
        int rc = 0;
        if (o.child_inspect) rc = child_inspect(&o);
        else if (o.child_open_close) rc = child_open_close(&o);
        else if (o.child_read_mode) rc = child_read_mode(&o, 1);
        else if (o.child_get_report) rc = child_get_report(&o);
        else if (o.child_write_mode) rc = child_write_mode(&o);
        fflush(stdout);
        // 尽量不执行正常 close/CF 释放路径: 本机 AppleUserHIDDevice 在 GetReport/GetValue 之后
        // 立刻 IOHIDDeviceClose 偶发 SIGTRAP（测试中不稳定复现），_exit 可保证输出确定性。
        _exit(rc);
    }

    log_open(o.logfile);
    log_line("hidprobe — HOUSI / USB2IIC_CTP HID 探针");
    char osv[128];
    os_version_string(osv, sizeof osv);
    log_line("OS: macOS %s / VID=0x%04X PID=0x%04X / pid=%d", osv, o.vid, o.pid, (int)getpid());
    log_line("logfile=%s", g_logfile_path ? g_logfile_path : "(未启用，仅 stdout)");
    log_line("参数: list=%d dump=%d setmode=%d mode=%d dry-run=%d force=%d restore=%d seconds=%d",
             o.list, o.dump, o.setmode, o.mode, o.dry_run, o.force, o.restore, o.seconds);
    if (!o.list && !o.dump && !o.setmode && !o.request_access) o.list = 1;

    if (o.request_access) {
        section("请求 Input Monitoring 授权（IOHIDRequestAccess）");
        print_permission_state();
        log_line("调用 IOHIDRequestAccess(kIOHIDRequestTypeListenEvent) ...（如弹出系统对话框请允许）");
        bool granted = IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
        log_line("IOHIDRequestAccess 返回: %s", granted ? "true (已授权)" : "false (未授权)");
        print_permission_state();
    }

    int rc = 0;

    if (o.list) {
        int r = run_list(&o);
        if (r != 0) rc = r;
    }
    if (o.dump) {
        int r = run_dump(&o);
        if (r != 0) rc = r;
    }
    if (o.setmode) {
        int r = orchestrate_setmode(&o);
        if (r != 0) rc = r;
    }
    if (o.elements && !o.setmode) {
        section("Feature 元素 dump（不发送任何 report）");
        print_permission_state();
        DevSession s;
        if (ds_open(&s, &o)) {
            CFArrayRef els = copy_feature_elements(s.digitizer);
            dump_feature_elements(s.digitizer, els, NULL, NULL);
            if (els) CFRelease(els);
            ds_close(&s);
        } else rc = 4;
    }
    log_line("");
    log_line("完成 (exit=%d)。提示: --dump 查看实时报告；--setmode 进入多点模式；--help 查看全部选项。", rc);
    print_accessibility_note();
    log_close();
    return rc;
}
