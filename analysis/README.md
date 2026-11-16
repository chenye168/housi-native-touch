# 复现步骤（analysis/README.md）

任务：证明 WindowServer 为什么丢弃外接 HOUSI 触控屏的 Digitizer 事件
（日志 `ignoring digitizer event for display <main> from unsupported service: (<BKIOHIDService ...>)`），
并给出"要让 WindowServer 接受需要满足什么条件"。

结论写在 **[acceptance-check.md](acceptance-check.md)**。本文件只讲怎么复现出那些证据。

---

## 0. 前置条件（本次分析时已具备）

| 项 | 值 |
|---|---|
| OS | macOS 27.2.1 (26B131), Apple M1 Pro, SIP on |
| 工具链 | Command Line Tools：`clang` / `llvm-objdump` / `llvm-nm` / `otool` / `nm`（无 Xcode） |
| LLVM binutils 路径 | `/Library/Developer/CommandLineTools/usr/bin/`（不在默认 PATH，需加） |
| 触控屏 | HOUSI USB 触控，VID `0x32D7` PID `0x0001`，Digitizer UsagePage `0x0D` Usage `0x04`，产品名 `USB2IIC_CTP_CONTROL` |
| 缓存 | x86_64 (Rosetta) 缓存：`/System/Volumes/Preboot/Cryptexes/Rosetta/System/Library/dyld/dyld_shared_cache_x86_64*`（含 `.map` 文本） |
| 已提取 | `tools/x86cache/`（lead 用 `/usr/lib/dsc_extractor.bundle` 提取，`tools/dscx.c`/`tools/dscx` 是驱动） |
| 日志证据 | `evidence/touchevents_6h.txt`、`evidence/diag1.log`、`evidence/ioreg_hideventservice.txt` |

> 本次分析**全程只读**：没有修改系统、没有 sudo、没有动 SIP。
> 唯一写入的是本仓库 `analysis/` 目录（以及 `/tmp` 的中间文件）。

---

## 1. 提取目录里到底有什么（先看这张表，别 cat 大文件）

```
tools/x86cache/System/Library/PrivateFrameworks/BackBoardHIDTouchEventProcessor.framework/Versions/A/BackBoardHIDTouchEventProcessor   (~604 KB)
tools/x86cache/System/Library/PrivateFrameworks/BackBoardHIDEventFoundation.framework/Versions/A/BackBoardHIDEventFoundation             (~624 KB)
tools/x86cache/System/Library/PrivateFrameworks/BackBoardServices.framework/Versions/A/BackBoardServices
tools/x86cache/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight                                                  (~8.7 MB)
tools/x86cache/usr/lib/libobjc.A.dylib
```

这些是**从 dyld shared cache 提取的裸 dylib**，性质很特殊，读之前必须知道：

1. Mach-O 段/节的 vmaddr **与运行时缓存地址一致**
   （例：`__TEXT` 0x7ffd00901000、`__oslogstring` 0x7ffd00952190；SkyLight 在 0x7ff8... 主缓存）。
   因此 `strings -t x` 得到的**文件偏移**要借 `otool -l` 的 `offset`/`addr` 换算成 vmaddr。
2. 数据段里的指针**没有**被 dsc_extractor 重定位，仍是缓存内的 chained-fixup 编码值：
   `低 32 位 == 目标缓存地址的低 32 位`，高位是链式/签名元数据。
   实测：`0x0000020023d82b9f` → `0x7ff800000000|0x23d82b9f` = 选择器池里的 `_queue_digitizerStateForService:`。
3. `LC_DYLD_CHAINED_FIXUPS`/绑定信息被剥掉了 ⇒ `llvm-objdump --bind/--rebase` 输出为空，
   `__stubs` 的 indirect symbols 变成 `LOCAL ABSOLUTE`。
   **但** 局部符号表（`llvm-nm -a`）保留了，所以函数名/方法名照样能查。
4. ObjC 选择器不能直接看出来（`movq 0x...(%rip),%rsi` 只是加载 selref 槽）——本仓库的
   `annotate.py` 会把它还原成名字（见第 3 节）。

---

## 2. 定位"谁打印了这行日志"

```bash
export PATH=/Library/Developer/CommandLineTools/usr/bin:$PATH
BIN=tools/x86cache/System/Library/PrivateFrameworks/BackBoardHIDTouchEventProcessor.framework/Versions/A/BackBoardHIDTouchEventProcessor

# (a) 哪个二进制里有这个字符串
grep -rl "ignoring digitizer" tools/x86cache/System/Library/PrivateFrameworks/

# (b) 字符串的文件偏移
strings -t x $BIN | grep -E "ignoring digitizer|digitizer attached|displayUUID"
#   4b395 displayUUID
#   520c0 digitizer attached (sender %llX) service:%{public}@
#   52500 ignoring digitizer event for display %{public}@ from unsupported service: (%{public}@):%{public}@

# (c) 段表 -> 换算 vmaddr（0x52500 -> 0x7ffd00953500）
otool -l $BIN | grep -A6 __oslogstring     # addr 0x7ffd00952190 offset 0x51190

# (d) 谁引用这个 vmaddr
llvm-objdump -d --no-show-raw-insn $BIN | grep 0x7ffd00953500
#   7ffd0091ea8b: leaq 0x34a6e(%rip),%rcx ## 0x7ffd00953500

# (e) 这条指令属于哪个函数（用 nm 找前后符号）
llvm-nm -an --defined-only $BIN | sort | awk '$1<="00007ffd0091ea8b"' | tail -3
#   -> ___66-[BKHIDDirectTouchEventProcessor _handleDirectTouchEvent:service:]_block_invoke (0x7ffd0091e898 .. 0x7ffd0091eadf)
```

同样的手法在 **SkyLight** 里定位 `handle_digitizer_added`：

```bash
S=tools/x86cache/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight
strings -t x $S | grep -E "handle_digitizer_added|digitizer added for display|no usable displayUUID"
#   5b9e2e handle_digitizer_added
#   600790 %s: digitizer service has no usable displayUUID property; ...
#   600850 %s: digitizer added for display %s
# SkyLight 的 __oslogstring: addr 0x7ff805f92750 offset 0x5e1750  => 0x600790 -> 0x7ff805fb1790
analysis/scripts/xrefs.py $S 0x7ff805fb1790,0x7ff805fb1850,0x7ff805f6ae2e
#   insn 0x7ff805e02475 -> 0x7ff805fb1790 ...   （handle_digitizer_added 被内联进 start_iohid_event_system_client 的 block）
```

---

## 3. 可读的反汇编（`analysis/scripts/annotate.py`）

裸 `llvm-objdump -d` 对这类 dylib 只能看到 `movq 0x...( %rip),%rsi`，不知道是哪个选择器。
`annotate.py` 做的事：

* `__objc_selrefs` 槽 → 选择器名（规则见上：`0x7ff800000000 | (raw & 0xffffffff)`，863 槽里 863 个可解析，862 个给出合法 ObjC 标识符、1 个存疑）
* `__stubs` → 导入符号名（`jmp *GOT(%rip)` → 解出 GOT 内容 → 用 `.map` 找所属 dylib → `llvm-nm` 查符号）
* `__cstring` / `__oslogstring` → 字符串原文
* `__cfstring` → CFString 内容（flags ⇒ ASCII/UTF-16，+0x10 是字符串指针）
* `__objc_selrefs` / `__const` / `__got` 等其它节 → 标注节名

用法：

```bash
cd analysis/scripts
./annotate.py <dylib> --addr 0x7ffd0091e898 --stop 0x7ffd0091eadf         # 指定区间
./annotate.py <dylib> --symbol '-[BKHIDDirectTouchEventProcessor ...]'      # 指定方法名
./annotate.py <dylib>                                                      # 整个 __text（大文件慎用）
```

本次生成的节选都在 `analysis/disasm/`：

| 文件 | 内容 |
|---|---|
| `01-locate-string-xref.txt` | 从字符串到 vmaddr 到 xref 到符号的全过程命令与输出 |
| `02-block-invoke-unsupported.ann.txt` | **打印该日志的 block**（含 accepted / unsupported 两条分支） |
| `03-handleDirectTouchEvent.ann.txt` | 入口方法（dispatch_sync 到 `_queue`，block 捕获 self/service/event） |
| `04-displayUUIDForDigitizerService.ann.txt` | **判定谓词本体**（displayUUID / feature flag / isBuiltIn / eventSource / usage / chamois） |
| `05-digitizerStateForService.ann.txt` | displayUUID → perDisplayInfo → digitizers[senderID] |
| `06-displayInfoForDisplay.ann.txt` | `"<main>"` → `+[BKSHIDEventDisplay builtinDisplay]`；其它 → `+[BKSHIDEventDisplay displayWithHardwareIdentifier:]` |
| `07-addDigitizerState.ann.txt` | 创建 `BKDirectTouchState`，label `%llX-%@` |
| `08-BKIOHIDService-setUpInitialProperties.ann.txt` | **`displayUUID` 属性到底从哪读**（`propertyForKey:`→`IOHIDServiceGetProperty`→`copy` 到 ivar 0x78） |
| `08b-BKIOHIDService-propertyForKey.ann.txt` | `propertyForKey:` 及其 block（`_IOHIDServiceGetProperty`） |
| `09-skylight-handle-digitizer-added.ann.txt` | SkyLight 侧：`IOHIDServiceClientCopyProperty("displayUUID")` → CFString → CFUUID → 显示器映射 |
| `10-selrefs-resolved.txt` | selref 槽 → 选择器名的解析证据（含 raw/low32） |

---

## 4. 校验点（自己重跑时怎么确认没跑偏）

1. `annotate.py` 对 `_queue_displayUUIDForDigitizerService:` 的输出里必须出现
   `IMPORT _os_feature_enabled_impl`、`-> __cstring 'backboardd'`、`-> __cstring 'SupportExternalTouchscreen'`、
   selref `isBuiltIn` / `eventSource` / `primaryUsagePage` / `primaryUsage`、`0xd8(%r14)`。
2. block_invoke 里必须能读到 `cmpb $0x1`(isBuiltIn)、`addl $0xfff2ffff`(`-0xD0001`)、`cmpl $0x1f`、`btl` 掩码 `0x80000009`。
3. SkyLight 侧必须能看到 `_IOHIDServiceClientCopyProperty`、`_CFGetTypeID`、`_CFStringGetTypeID`、
   `_CFUUIDCreateFromString`、`_CFUUIDGetUUIDBytes`、`_CFStringGetCString` 这几个 import，
   以及 `__cstring 'handle_digitizer_added'` 被当作 `%s` 参数压栈。
4. 日志证据交叉验证：
   * `grep -c "unsupported service" evidence/touchevents_6h.txt` → 2322
   * `grep -c "digitizer added for display" evidence/*.txt` → 0
   * `grep -c -i displayuuid evidence/ioreg_hideventservice.txt` → 0（该 HID 服务没有这个属性）

---

## 5. 已知限制 / 下一步

* 只分析了 **x86_64 (Rosetta)** 缓存里的二进制；arm64e 缓存（`/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e.*`）
  对应同一份源码，本次用 x86_64 是为了能借助 `dsc_extractor` + 局部符号表 + 选择器池反解。若需要，可对 arm64e 缓存
  重复第 2 节（lead 已在 `.01`/`.52` 上用 `strings` 定位过相同字符串）。
* 未展开：`_queue_chamoisDisplayUUID` 的设置者、`BKDirectTouchState` 之后的 hit-test/touch-stream 层、
  SkyLight `notify_touch_support_changed` 之后的行为。
* 未做任何运行时修改/注入实验（本任务只做静态分析）；实验建议见 `acceptance-check.md` 第 5 节。
