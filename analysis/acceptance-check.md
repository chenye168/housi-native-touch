# WindowServer 为什么拒绝 HOUSI 外接触控屏 —— 反汇编级结论

**对象**：macOS 27.2.1 (26B131), Apple M1 Pro, SIP on
**日志**：`subsystem com.apple.BackBoard, category TouchEvents`
**被分析的全部二进制**来自 x86_64 (Rosetta) dyld shared cache 的官方提取
（`/usr/lib/dsc_extractor.bundle` → `tools/x86cache/`），这与我机上的 arm64e 缓存是同一份源码。
所有地址都是**缓存 vmaddr**（提取出的 Mach-O 保留了原始段地址，已在 `tools/x86cache/...map` 中验证）。

| 逻辑层 | 二进制 | 关键符号 | 地址 |
|---|---|---|---|
| 触摸事件路由（真正丢弃事件的地方） | `BackBoardHIDTouchEventProcessor` | `-[BKHIDDirectTouchEventProcessor _handleDirectTouchEvent:service:]` | `0x7ffd0091e645` (~0x253 B) |
| 同上（block，**打印该日志的地方**） | 同上 | `___66-[BKHIDDirectTouchEventProcessor _handleDirectTouchEvent:service:]_block_invoke` | `0x7ffd0091e898` – `0x7ffd0091eadf` |
| **谓词本体**（服务→displayUUID） | 同上 | `-[BKHIDDirectTouchEventProcessor _queue_displayUUIDForDigitizerService:]` | `0x7ffd0091d627` – `0x7ffd0091d6fe` |
| 状态查找 / 建状态 | 同上 | `_queue_digitizerStateForService:` / `_queue_addDigitizerStateForService:` / `_queue_displayInfoForDisplay:createIfNeeded:` | `0x7ffd0091c380` / `0x7ffd0091b557` / `0x7ffd0091b3d3` |
| `[service displayUUID]` 的**来源** | `BackBoardHIDEventFoundation` | `-[BKIOHIDService _setUpInitialProperties]` → `-propertyForKey:` → `IOHIDServiceGetProperty` | `0x7ffa14fef67f` / `0x7ffa14ff0a35` |
| digitizer→display 注册（"digitizer added for display %s"） | `SkyLight` (WindowServer) | `WS::Displays::WSHIDDisplayEventListener::handle_digitizer_added(IOHIDServiceClient*)`（被内联进 `start_iohid_event_system_client` 的 block_invoke_2） | 谓词区 `0x7ff805e02200`–`0x7ff805e02300`，日志点 `0x7ff805e02463`/`0x7ff805e024f3`/`0x7ff805e02a26` |

---

## 0. 结论（先说）

1. 这行日志**不是**判某个"白名单/entitlement"，而是一个**两级条件**：
   `displayUUID = [service displayUUID]` 拿不到 **且** 服务不是"内置 + 已知 digitizer usage" → 丢弃，并打这条日志。
2. `[service displayUUID]` 的值来自 HID 服务的属性 **`displayUUID`**
   （`IOHIDServiceGetProperty`，即 IOHIDEventService 的属性表；**key 是小写 d 开头的 `displayUUID`**）。
3. 期望的值类型是 **CFString 形式的 UUID 文本**（如 `30D985CD-BE3F-4C0E-B055-C5E22CFF2E07`），
   **不是** CFUUID/CFData/16 字节 —— SkyLight 一侧拿到后先 `CFGetTypeID == CFStringGetTypeID()`
   才 `CFUUIDCreateFromString`，否则直接判 "no usable displayUUID property"。
4. 拿不到时：
   * BackBoard 侧的唯一 fallback 是**特性开关** `_os_feature_enabled_impl("backboardd","SupportExternalTouchscreen")`
     **且** 服务为外接(`!isBuiltIn`) **且** `eventSource==2` **且** `primaryUsagePage==0xD && primaryUsage==0x4`
     → 用 `self->_queue_chamoisDisplayUUID`（通常为 nil）。
   * SkyLight 侧**没有任何 fallback**：属性缺失/不是 CFString → 直接 `treating as no digitizer added`（所以日志里从来没出现 "digitizer added for display"）。
5. **entitlement/identity 检查：在这两条判定路径里不存在**。判定只用：`displayUUID` 属性、`isBuiltIn`、
   `eventSource`、`primaryUsagePage/Usage`、一个 os_feature 开关。entitlement（"not authorized"）出现在
   完全不同的地方（`BKTouchEventServer` 的 touch-stream 客户端连接）。
6. 因此「让 WindowServer 接受」的**最小条件集合**是：
   > **让该 HID 事件服务的属性 `displayUUID` = 外接显示器 UUID 的 CFString**
   > （且该 UUID 必须是 WindowServer/BackBoard 已注册的显示，例如用 `CGDisplayCreateUUIDFromDisplayID` 得到的那个）。
   单个条件即可同时满足两层：BackBoard 的 `_queue_displayUUIDForDigitizerService:` 会直接返回非 nil，
   SkyLight 的 `handle_digitizer_added` 会解析成功并注册。

---

## 1. 日志点定位（命令与偏移）

见 `disasm/01-locate-string-xref.txt`。要点：

```
$ grep -rl "ignoring digitizer" tools/x86cache/System/Library/PrivateFrameworks/
.../BackBoardHIDTouchEventProcessor.framework/Versions/A/BackBoardHIDTouchEventProcessor

$ strings -t x <bin> | grep -E "ignoring digitizer|digitizer attached|displayUUID"
4b395 displayUUID
52070 ignoring digitizer event from non-digitizer sender (%{public}@):%{public}@
520c0 digitizer attached (sender %llX) service:%{public}@
52500 ignoring digitizer event for display %{public}@ from unsupported service: (%{public}@):%{public}@
```

`__oslogstring` 段：vma `0x7ffd00952190` @ fileoff `0x51190`
→ `0x52500` ⇒ **vmaddr `0x7ffd00953500`**。

```
$ llvm-objdump -d --no-show-raw-insn <bin> | grep 0x7ffd00953500
7ffd0091ea8b:  leaq 0x34a6e(%rip), %rcx   ## 0x7ffd00953500 ...
```

唯一定位到 `___66-[BKHIDDirectTouchEventProcessor _handleDirectTouchEvent:service:]_block_invoke`
（符号区间 `0x7ffd0091e898`–`0x7ffd0091eadf`）。

> 注意：`digitizer attached (sender %llX) service:` 也在这个二进制里（`0x7ffd009530c0`），
> 说明日志里的 attach 与 ignore 是**同一条管道**；而 `digitizer added for display %s` 在 **SkyLight** 里
> （另一个消费者，见第 3 节）。

---

## 2. `unsupported service` 的判定谓词（伪代码 + 汇编）

### 2.1 入口：`-[BKHIDDirectTouchEventProcessor _handleDirectTouchEvent:service:]`（0x7ffd0091e645）

```objc
- (void)_handleDirectTouchEvent:(IOHIDEventRef)event service:(BKIOHIDService *)service {
    dispatch_assert_queue_not(self->_queue);                 // 0x7ffd0091e663
    if (!event) { ...assert/log... return; }                 // 0x7ffd0091e66c
    dispatch_sync(self->_queue, ^{                            // 0x7ffd0091e6bb, block @0x7ffd0091e898
        // block 捕获: +0x20=self  +0x28=service  +0x30=event   (0x7ffd0091e6af/e6b3/e6b7)
    });
}
```

### 2.2 block_invoke（0x7ffd0091e898）—— 真正判定处

```objc
// ---- 第一步：取 displayUUID ----
id displayUUID = [self _queue_displayUUIDForDigitizerService:service];   // 0x7ffd0091e8b2..e8f0
                                                                        // selref _queue_displayUUIDForDigitizerService: @0x7ffd401778e8
if (displayUUID == nil) {                    // 0x7ffd0091e8cd  je 0x7ffd0091e957
    // ---- 第二步：唯一允许的"无 UUID"分支：内置 + usage 白名单 ----
    if (![service isBuiltIn]) goto unsupported;              // 0x7ffd0091e957 movq 0x28(%r14),%r15 ; 0x7ffd0091e95b selref isBuiltIn ; 0x7ffd0091e96b cmpb $1 ; jne -> unsupported
    uint32_t pair = ([service primaryUsagePage] << 16) | [service primaryUsage];   // 0x7ffd0091e96f..e996
    pair -= 0x000D0001;                                      // 0x7ffd0091e999  addl $0xfff2ffff
    if (pair > 0x1F) goto unsupported;                       // 0x7ffd0091e99e  cmpl $0x1f ; ja
    if (!((0x80000009 >> pair) & 1)) goto unsupported;       // 0x7ffd0091e9a3  movl $0x80000009 ; btl ; jb -> accepted
    // 允许的 (page,usage) = { (0xD,1), (0xD,4), (0xD,0x20) }  ← 位 0/3/31
    goto accepted;                                           // 0x7ffd0091e9ab  jb 0x7ffd0091e8d3
}
accepted:
    id state = [self _queue_digitizerStateForService:service];       // 0x7ffd0091e8d3..e946
    if (!state) {
        // "*** unknown digitizer %{public}@ -- initializing now"  (fmt 0x7ffd00953570 @ 0x7ffd0091ea0c)
        state = [self _queue_addDigitizerStateForService:service];
    }
    [state handlePathCollectionEvent:event];                 // 0x7ffd0091e950  ->BKDIRTTOUCHSTATE 0x7ffd0092e238
    return;

unsupported:                                                  // 0x7ffd0091e9b1  (L4)
    if (os_log_type_enabled(BKLogTouchEvents(), 0x10 /*ERROR*/)) {
        // r12 = _BKSDisplayUUIDMainKey  (= @"<main>", 见 0x7ffd0091ea3f..ea4f)
        // fmt 0x7ffd00953500 @ 0x7ffd0091ea8b，__os_log_error_impl @ 0x7ffd0091eaa0
        os_log_error(..., "ignoring digitizer event for display %{public}@ from unsupported service: (%{public}@):%{public}@",
                     /*display*/ @"<main>", /*service*/ service, /*event*/ event);
    }
    return;                                                  // 事件被丢弃
}
```

**对应汇编（节选，完整见 `disasm/02-block-invoke-unsupported.ann.txt`）**

```
7ffd0091e8d3: movq 0x20(%r14), %rdi
7ffd0091e8d7: movq 0x28(%r14), %rdx          ; service
7ffd0091e8db: movq 0x7ffd40177878, %rsi      ; selref _queue_digitizerStateForService:
7ffd0091e8e2: callq *objc_msgSend
...
7ffd0091e957: movq 0x28(%r14), %r15          ; service
7ffd0091e95b: movq 0x7ffd401778d8, %rsi      ; selref isBuiltIn
7ffd0091e965: callq *objc_msgSend
7ffd0091e96b: cmpb $0x1, %al
7ffd0091e96d: jne  0x7ffd0091e9b1            ; !isBuiltIn -> unsupported
7ffd0091e96f: movq 0x7ffd401778c0, %rsi      ; selref primaryUsagePage
7ffd0091e979: callq *objc_msgSend
7ffd0091e97f: movl %eax, %r12d
7ffd0091e982: movq 0x7ffd40177890, %rsi      ; selref primaryUsage
7ffd0091e98c: callq *objc_msgSend
7ffd0091e992: shll $0x10, %r12d
7ffd0091e996: orl  %r12d, %eax
7ffd0091e999: addl $0xfff2ffff, %eax         ; -= 0xD0001
7ffd0091e99e: cmpl $0x1f, %eax
7ffd0091e9a1: ja   0x7ffd0091e9b1            ; outside allowlist window -> unsupported
7ffd0091e9a3: movl $0x80000009, %ecx
7ffd0091e9a8: btl  %eax, %ecx
7ffd0091e9ab: jb   0x7ffd0091e8d3            ; allowlist hit -> accepted
7ffd0091e9b1: callq BKLogTouchEvents         ; fall-through = "unsupported"
...
7ffd0091ea8b: leaq 0x7ffd00953500, %rcx      ; "ignoring digitizer event for display ... from unsupported service: (%@):%@"
7ffd0091eaa0: callq __os_log_error_impl
```

> `for display <main>` 里的 `<main>` **不是**真实显示器，是常量 `BKSDisplayUUIDMainKey`。
> 硬证据（`analysis/scripts/dscmap.py` 直接读缓存）：
> ```
> $ llvm-nm -an BackBoardServices | grep 00007ff84292a460
> 00007ff84292a460 S _BKSDisplayUUIDMainKey
> *(_BKSDisplayUUIDMainKey) = 0x000008004292d348  -> CFString @0x7ff84292d348 flags=0x7c8 len=6 -> "<main>"
> ```
> 这解释了为什么 2322 条日志全都是 `for display <main>`——这个分支里根本没有"当前显示器"可言。

### 2.3 谓词本体：`-[BKHIDDirectTouchEventProcessor _queue_displayUUIDForDigitizerService:]`（0x7ffd0091d627）

```objc
- (id)_queue_displayUUIDForDigitizerService:(BKIOHIDService *)service {
    id uuid = [[service displayUUID] copy];                  // 0x7ffd0091d637 selref displayUUID ; 0x7ffd0091d63e rdi=service
                                                             // 返回 [_displayUUID copy] (ivar 0x78)
    BOOL feature = _os_feature_enabled_impl("backboardd",    // 0x7ffd0091d652 "backboardd"
                                           "SupportExternalTouchscreen");   // 0x7ffd0091d659
    if (uuid == nil && feature) {                            // 0x7ffd0091d665 sete/and/cmp/jne
        if (![service isBuiltIn]                             // 0x7ffd0091d678..d683  jne -> nil
            && [service eventSource] == 2                    // 0x7ffd0091d685 selref eventSource ; cmp $2
            && [service primaryUsagePage] == 0xD             // 0x7ffd0091d69a ; cmp $0xd
            && [service primaryUsage] == 0x4)                // 0x7ffd0091d6af ; cmp $4
        {
            uuid = objc_retain(self->_queue_chamoisDisplayUUID);   // 0x7ffd0091d6c4  movq 0xd8(%r14),%rdi ; objc_retain
        } else {
            uuid = nil;                                      // 0x7ffd0091d6d6  xorl %ebx,%ebx
        }
    }
    return uuid;                                             // 0x7ffd0091d6d8..d6e5 (tail jmp objc_autoreleaseReturnValue)
}
```

**对应汇编（完整见 `disasm/04-displayUUIDForDigitizerService.ann.txt`）**

```
7ffd0091d631: movq %rdx, %r15                     ; service
7ffd0091d637: movq 0x7ffd40176ff0, %rsi           ; selref displayUUID
7ffd0091d63e: movq %rdx, %rdi
7ffd0091d641: callq *objc_msgSend                 ; [service displayUUID]
7ffd0091d652: leaq 0x7ffd0094cec4, %rdi           ; "backboardd"
7ffd0091d659: leaq 0x7ffd0094cecf, %rsi           ; "SupportExternalTouchscreen"
7ffd0091d660: callq _os_feature_enabled_impl
7ffd0091d665: testq %rbx, %rbx
7ffd0091d668: sete  %cl
7ffd0091d66b: andb  %cl, %al
7ffd0091d66d: cmpb  $0x1, %al
7ffd0091d66f: jne   0x7ffd0091d6d8                 ; 已有 UUID 或特性关闭 -> 直接返回
7ffd0091d671: movq 0x7ffd401778d8, %rsi           ; selref isBuiltIn
7ffd0091d67b: callq *objc_msgSend
7ffd0091d681: testb %al, %al
7ffd0091d683: jne   0x7ffd0091d6d6                 ; 内置 -> nil
7ffd0091d685: movq 0x7ffd40177090, %rsi           ; selref eventSource
7ffd0091d68f: callq *objc_msgSend
7ffd0091d695: cmpl  $0x2, %eax
7ffd0091d698: jne   0x7ffd0091d6d6
7ffd0091d69a: movq 0x7ffd401778c0, %rsi           ; selref primaryUsagePage
7ffd0091d6a4: callq *objc_msgSend
7ffd0091d6aa: cmpl  $0xd, %eax
7ffd0091d6ad: jne   0x7ffd0091d6d6
7ffd0091d6af: movq 0x7ffd40177890, %rsi           ; selref primaryUsage
7ffd0091d6b9: callq *objc_msgSend
7ffd0091d6bf: cmpl  $0x4, %eax
7ffd0091d6c2: jne   0x7ffd0091d6d6
7ffd0091d6c4: movq  0xd8(%r14), %rdi              ; self->_queue_chamoisDisplayUUID
7ffd0091d6cb: callq *objc_retain
```

* `0xd8` 已核对：`chamoisDisplayUUID` getter（`0x7ffd0091d321` 的 block）读的正是 `0xd8(%rax)`，
  setter（`0x7ffd0091d39e`）也用 `0xd8`。该 ivar 名 = `_queue_chamoisDisplayUUID`
  （`__objc_ivar` 表，见 `disasm/10-selrefs-resolved.txt` 与本文档第 6 节命令）。
* `eventSource == 2` 的含义（**中等置信度**，见第 7 节）：`NSStringFromBKSHIDEventSource`（BackBoardServices
  `0x7ff819b23ff8`）就是 `table[value]`，表在 `0x7ff84292b818`，实测解码：
  `0='builtin', 1='externalKeyboard', 2='externalAccessory', 3='verifiedExternalKeyboard', ... 13='manipulator'`。
  本机日志里该服务是 `eventSource: externalKeyboard`（⇒ 值 1），**不是 2**；
  也就是说即使打开特性开关，`eventSource==2` 这一条对本机也大概率不满足（这条 fallback 本来也不是可行路径）。
* 同类 ivar 还有 `_queue_serviceSenderIDToChamoisDisplayUUID` / `setChamoisDisplayUUID:`
  —— 即 "chamois"（外接触控）显示是由别的组件在运行时注入的；**在本二进制内没有找到调用
  `setChamoisDisplayUUID:` 的地方**，所以即使打开特性开关，若 `_queue_chamoisDisplayUUID` 为 nil 依然被拒。★见第 7 节置信度。

### 2.4 有无 entitlement / identity 检查？

| 检查 | 是否存在 | 证据 |
|---|---|---|
| os_feature 开关 `backboardd/SupportExternalTouchscreen` | 是（只在无 UUID 的 fallback 里） | `_os_feature_enabled_impl` @0x7ffd0091d660 |
| `isBuiltIn` | 是 | selref `isBuiltIn` 0x7ffd401778d8，两处使用 |
| `eventSource == 2` | 是（仅 fallback 分支） | 0x7ffd0091d695 `cmpl $0x2`；名称表 0x7ff84292b818：1=externalKeyboard、2=externalAccessory，本机设备是 externalKeyboard |
| `primaryUsagePage/Usage` 白名单 | 是：block 里 `{(0xD,1),(0xD,4),(0xD,0x20)}`；fallback 里只认 `(0xD,4)` | 0x7ffd0091e999 位掩码 `0x80000009`；0x7ffd0091d6aa/d6bf |
| allowlist of 设备 VID/PID/名称 | **否** | 该函数内无此类比较，只比较上面 4 个服务属性 |
| entitlement / code signature / teamID | **否** | 两条判定路径都没有 `SecTask*`/`cs_*`/`*Entitlement*` 调用（imports 只有 objc/os_log/IOHID/CF 基础 API，见 `disasm/04-*.ann.txt`） |
| 显示器"是否已注册" | 是（间接）：只有 UUID 解析成功**且**能在 WS 里找到映射到显示器，SkyLight 才 `digitizer added`；BackBoard 侧 `_queue_displayInfoForDisplay:` 用 `+[BKSHIDEventDisplay displayWithHardwareIdentifier:]`（类由 classref 0x7ffd4003e7d8 → 0x7ff8435b0d50 解出：`S _OBJC_CLASS_$_BKSHIDEventDisplay`；找不到则 display=nil，但仍建 info） | 0x7ffd0091b4a7 selref `displayWithHardwareIdentifier:` |

---

## 3. digitizer attach 时 `displayUUID` 到底从哪读、什么类型、拿不到的后果

### 3.1 BackBoard 一侧：`BKIOHIDService` 的属性缓存

`-[BKIOHIDService _setUpInitialProperties]`（`BackBoardHIDEventFoundation`，`0x7ffa14fef67f`）：

```objc
- (void)_setUpInitialProperties {
    id uuid = [self propertyForKey:@"displayUUID"];        // 0x7ffa14fef69a leaq cfstring 'displayUUID'
    self->_displayUUID = [uuid copy];                      // 0x7ffa14fef6c6  movq 0x78(%rbx) ; 0x7ffa14fef6ca movq %rax,0x78(%rbx)
    self->_primaryUsagePage = [[self propertyForKey:@"PrimaryUsagePage"] intValue];   // 0x7ffa14fef6de
    self->_primaryUsage     = [[self propertyForKey:@"PrimaryUsage"] intValue];       // 0x7ffa14fef712
    self->_product          = [self propertyOfClass:[NSString class] forKey:@"Product"];   // 0x7ffa14fef75d
    self->_transport        = [self propertyOfClass:[NSString class] forKey:@"Transport"]; // 0x7ffa14fef78f
    if ([prop(@"Authenticated") isKindOfClass:NSNumber.class]) self->_authenticated = ...;             // 0x7ffa14fef7b5
    if ([prop(@"Built-In") boolValue]) self->_builtIn = YES;                     // 0x7ffa14fef808
    ...
}
```

`-[BKIOHIDService displayUUID]` 就是**读 ivar 0x78**（`0x7ffa14ff265f: movq 0x78(%rdi),%rax; ret`），
`setDisplayUUID:` 写同一 ivar（`0x7ffa14ff2669`）。

`propertyForKey:` 的实际读取 API（`0x7ffa14ff0a35` → block `0x7ffa14ff0c1b`）：

```
7ffa14ff0c2c: movq 0x70(%rax), %rdi        ; self->_serviceRef  (IOHIDServiceRef)
7ffa14ff0c30: callq _IOHIDServiceGetProperty
7ffa14ff0c35: callq _objc_retainAutoreleasedReturnValue
```

**结论（BackBoard 侧）**：属性从 **IOHIDServiceRef 的属性表** 读（`IOHIDServiceGetProperty(serviceRef, key)`，
等价于 `IOHIDServiceClientCopyProperty` 看到的同一张属性表；不是 BK 自己的 property cache，也不是直接翻 IORegistry）。
值被原样 `copy` 存进 `_displayUUID`，**不做类型检查**。

### 3.2 SkyLight 一侧：`handle_digitizer_added`

见 `disasm/09-skylight-handle-digitizer-added.ann.txt`，关键步骤如下（地址为 SkyLight 缓存 vmaddr）：

```cpp
// WS::Displays::WSHIDDisplayEventListener::handle_digitizer_added(IOHIDServiceClientRef svc)
//   r14 = svc ; r12 = listener
// 1) 只认两类 digitizer usage：kDigitizerUsagePairs @0x7ff805f09710 = { (0xD,4), (0xD,1) }
for (int i = 0; i < 2; i++)                                   // 0x7ff805e02200
    if (IOHIDServiceClientConformsTo(svc, 0xD, pairs[i].usage)) goto known;
return;                                                       // 0x7ff805e02222 -> epilogue

known:
// 2) 读属性：CFString "displayUUID"
CFTypeRef v = IOHIDServiceClientCopyProperty(svc, CFSTR("displayUUID"));   // cfstring @0x7ff8402226c0；调用 @0x7ff805e02232
if (!v) goto no_usable_uuid;                                   // 0x7ff805e02242 je 0x7ff805e02434
// 3) 类型必须是 CFString
if (CFGetTypeID(v) != CFStringGetTypeID()) goto no_usable_uuid;             // 0x7ff805e02256 / 0x7ff805e0225e / 0x7ff805e02266
// 4) 必须是可解析的 UUID 文本
CFUUIDRef u = CFUUIDCreateFromString(NULL, (CFStringRef)v);     // 0x7ff805e02284
if (!u) goto malformed;                                        // 0x7ff805e02297 je 0x7ff805e024f3
// 5) 转 16 字节 UUID 并查 "UUID -> WSDisplay" 表
CFUUIDBytes bytes = CFUUIDGetUUIDBytes(u);                      // 0x7ff805e022af
... std::unordered_map<CFUUIDBytes, WSDisplay*> lookup ...      // 0x7ff805e02373.. (hash 0x9DDFEA08EB382D69)
if (found) os_log("... digitizer added for display %s", cstr);   // fmt 0x7ff805fb1850 @ 0x7ff805e02a43
if (!found / 未注册) 不打印 added

no_usable_uuid:   // 0x7ff805e02434
    os_log_error("%s: digitizer service has no usable displayUUID property; treating as no digitizer added",
                 "handle_digitizer_added");                     // fmt 0x7ff805fb1790, __func__ cstring 0x7ff805f6ae2e
    return;
malformed:        // 0x7ff805e024f3
    CFStringGetCString(v, buf, 0x40, kCFStringEncodingUTF8);    // 0x7ff805e022fb（失败则用 "UNKNOWN"）
    os_log_error("%s: digitizer service has malformed displayUUID '%s'; treating as no digitizer added", ...);  // fmt 0x7ff805fb17f0
```

**期望类型 / 值（结论）**：
* 类型：**CFString**（`CFGetTypeID == CFStringGetTypeID()`）。
* 值：能被 `CFUUIDCreateFromString` 解析的 UUID 文本，即 `XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX`（大小写均可）。
* **不是** CFUUID 对象、**不是** CFData 16 字节，也**不是** `"<main>"` 这种 sentinel
  （sentinel `BKSDisplayUUIDMainKey` 只用于 BackBoard 内部字典 key，若真塞给 SkyLight 会被判 malformed）。
* 且该 UUID 必须是 WindowServer 认识的显示器 UUID（`CGDisplayCreateUUIDFromDisplayID` 得到的那个）。

### 3.3 拿不到时的 fallback

| 层 | 拿不到时的行为 | 是否 fallback 到 main display |
|---|---|---|
| `_queue_displayUUIDForDigitizerService:` | 若特性开关开 & 外接 & eventSource==2 & (0xD,4) → 返回 `_queue_chamoisDisplayUUID`（**可能仍为 nil**）；否则 nil | 不 fallback 到 main；main 只用于日志文本 |
| `_queue_digitizerStateForService:` / `_queue_addDigitizerStateForService:` | 若 uuid 为空（length==0）→ 用 sentinel `BKSDisplayUUIDMainKey` 作为 perDisplayInfo 的 key；`_queue_displayInfoForDisplay:` 对该 sentinel 走 `+[BKSHIDEventDisplay builtinDisplay]`（selref 0x7ffd401778f0 → 0x7ffd0091b490） | 是（仅内置分支能走到这里） |
| SkyLight `handle_digitizer_added` | 直接 `treating as no digitizer added`，**无 fallback** | 否 |

> 这正是内建 trackpad（`isBuiltIn=YES`, usage=(0xD,5)?→ 注意 block 白名单是 (0xD,1)/(0xD,4)/(0xD,0x20)，
> trackpad 走的是 `_queue_addDigitizerStateForService:` 的 sentinel→builtinDisplay 路径）能工作、
> 而外接触控屏（`isBuiltIn=NO`, usage=(0xD,4)）被拒的分叉点。

---

## 4. 让 WindowServer 接受的最小条件集合

```
必要条件（唯一可行路径）：
  HID 事件服务属性 "displayUUID" == CFString("30D985CD-BE3F-4C0E-B055-C5E22CFF2E07")
        （= 外接 HOUSI 显示器的 CGDisplayCreateUUIDFromDisplayID 结果，大写带连字符）

满足后自动成立：
  BKIOHIDService->_displayUUID != nil            → _queue_displayUUIDForDigitizerService: 返回非 nil
  block_invoke 走 accepted 分支                  → 不再打印 "ignoring digitizer event ... unsupported service"
  _queue_displayInfoForDisplay: 用 +[BKSHIDEventDisplay displayWithHardwareIdentifier:UUID] 找到显示对象
  SkyLight handle_digitizer_added: CFString→CFUUID→WSDisplay 命中 → 打印 "digitizer added for display <uuid>"

不充分的替代路径（不要指望）：
  feature flag "SupportExternalTouchscreen"  仅影响 BackBoard 的 fallback，
  且要求 _queue_chamoisDisplayUUID != nil，且 SkyLight 侧依然无 fallback → 不会出现 "digitizer added"。
  另：/System/Library/FeatureFlags/Domain/ 下没有 backboardd.plist，全盘也搜不到该 flag 名字
     （`ls /System/Library/FeatureFlags/Domain | grep backboard` = 空；`grep -rl SupportExternalTouchscreen /System/Library/FeatureFlags` = 空）
     ⇒ 该 flag 在本机未被定义，`_os_feature_enabled_impl` 取其默认值（false）。
```

补充证据（本机实测，只读取）：

```
$ grep -c -i displayuuid evidence/ioreg_hideventservice.txt
0                                   # 该 USB HID 事件服务（AppleUserHIDEventService, USB2IIC_CTP_CONTROL,
                                    #  PrimaryUsagePage=13/PrimaryUsage=4, DeviceUsagePairs 含 {13,4}）没有 displayUUID 属性
$ grep -c "unsupported service" evidence/touchevents_6h.txt
2322
$ grep -c "digitizer added for display" evidence/*.txt
0                                   # SkyLight 侧从未注册成功
```

---

## 5. 下一步最小可验证实验（建议顺序）

1. **先验证属性确实缺失**（只读）：
   `ioreg -l -w0 -r -c AppleUserHIDEventService | grep -i displayuuid` → 期望无输出。
2. **低成本试探：从用户态给 IOHIDEventService 塞属性**
   写一个 ~40 行的小工具：`IOHIDEventSystemClientCreateWithType` → `IOHIDEventSystemClientSetMatching`
   (`{PrimaryUsagePage:13, PrimaryUsage:4}`) → 对匹配到的 service 调
   `IOHIDServiceClientSetProperty(svc, CFSTR("displayUUID"), CFSTR("<外接显示器 UUID>"))`。
   由于 `BKIOHIDService` 只在 `initWithHIDServiceRef:`/`_setUpInitialProperties` 读一次，
   需要在 WindowServer 匹配到该服务**之前**写入（拔插设备、先起 watcher 再插），或验证 BK 是否有属性变更回读
   （本次静态分析未发现回读路径 → 需要拔插重来是更稳的做法）。
   观察：`log stream --predicate 'subsystem == "com.apple.BackBoard" AND category == "TouchEvents"'`
   应出现 `digitizer added for display 30D985CD-...`，且 `ignoring digitizer event ... unsupported service` 消失。
3. **稳健路线（推荐，且不需要动系统/SIP）**：自己写一个 DriverKit HID event service dext
   （entitlement `com.apple.developer.driverkit.family.hid.eventservice`，本机 AppleUserHIDEventDriver 的
   `IOServiceDEXTEntitlements` 正是这个），匹配该 USB 设备的 HID interface 并发布 IOHIDEventService，
   在 `start()` 里 `IOHIDEventService::SetProperty("displayUUID", <CFString/UUID 字符串>)`。
   这条路是"设备自己声明 displayUUID"的正规做法（Sidecar/Apple 自家触摸设备就是这条路），
   且能同时喂饱 BackBoard 与 SkyLight 两个消费者。
4. 若仍不行，再往下一层看（本次未展开）：
   `BKDirectTouchState` 创建后的 hit-test regions / touch stream 客户端 /
   `BKCADisplayMonitor displayUUIDIsActive:`、`_queue_displayInfoForDisplay:` 找到的 `BKSHIDEventDisplay` 是否为 nil。

---

## 6. 复现（关键命令）

```bash
# 0) 提取（lead 已完成，勿重复）：/usr/lib/dsc_extractor.bundle -> tools/x86cache
# 1) 定位日志串
strings -t x tools/x86cache/.../BackBoardHIDTouchEventProcessor | grep "unsupported service"
otool -l tools/x86cache/.../BackBoardHIDTouchEventProcessor | grep -A6 __oslogstring   # 0x51190 -> vma 0x7ffd00952190
# 2) 找 xref（0x52500 -> 0x7ffd00953500）
llvm-objdump -d --no-show-raw-insn tools/x86cache/.../BackBoardHIDTouchEventProcessor | grep 0x7ffd00953500
# 3) 带 ObjC 选择器/符号注释的反汇编
analysis/scripts/annotate.py <dylib> --addr 0x7ffd0091e898 --stop 0x7ffd0091eadf
# 4) SkyLight 里大函数的 xref（9MB __text，别整体反汇编）
analysis/scripts/xrefs.py tools/x86cache/.../SkyLight 0x7ff805fb1790,0x7ff805fb17f0,0x7ff805fb1850
analysis/scripts/annotate.py tools/x86cache/.../SkyLight --addr 0x7ff805e021c5 --stop 0x7ff805e02b70
```

工具脚本在 `analysis/scripts/`：
* `dscmap.py`  —— 解析 dyld cache 头/mapping（address ↔ file offset），用于把缓存内指针还原成字符串
* `annotate.py` —— 反汇编 + 自动注释（`__objc_selrefs`→选择器名、`__stubs`→导入符号、`__cstring/__oslogstring/__cfstring`→字符串）
* `xrefs.py`   —— 在 `__text` 里按字节模式扫 RIP-relative 引用（快，适合 SkyLight）

---

## 7. 置信度与未决项

| 结论 | 置信度 | 依据 |
|---|---|---|
| 日志由 `_handleDirectTouchEvent:service:` 的 block 在 `displayUUID==nil 且 !(isBuiltIn && usage∈{(0xD,1),(0xD,4),(0xD,0x20)})` 时打印 | **高** | 完整反汇编 + 日志文本/参数与 2322 条实测日志逐项吻合（display=`<main>`=BKSDisplayUUIDMainKey、service 描述、event） |
| `displayUUID` 属性 key 就是 `displayUUID`（小写 d），值需 CFString UUID 文本 | **高** | 两个独立消费者（BB 0x7ffa14fef69a、SkyLight 0x7ff8402226c0）字面量一致；SkyLight 有 CFString/CFUUID 解析与 malformed 分支 |
| 该属性来自 IOHIDEventService 属性表（`IOHIDServiceGetProperty`/`IOHIDServiceClientCopyProperty`） | **高** | `propertyForKey:`→`_IOHIDServiceGetProperty`；SkyLight 直接 `IOHIDServiceClientCopyProperty` |
| 无 entitlement 检查 | **高** | 两条路径 imports/调用清单里没有相关 API；判定只用 4 个属性 + 1 个 os_feature |
| `eventSource==2` 名称表解码（0=builtin,1=externalKeyboard,2=externalAccessory,…） | **中** | 直接读了 `NSStringFromBKSHIDEventSource` 用的字符串表；未用运行时抓值交叉验证 |
| `SupportExternalTouchscreen` 当前为 false / 未定义 | **中高** | 磁盘上无定义（无 backboardd.plist、全盘搜不到该名）；运行时未取（可用 lldb/内部工具再验） |
| 谁在正常流程里给 `BKIOHIDService.displayUUID` 赋值 | **中** | 在 BackBoardHIDEventFoundation 内**没有** `setDisplayUUID:` 的调用者；推断由提供 HIDEventService 的一方（dext/虚拟设备，如 Sidecar）在属性里给出，而非 BK 代码注入 |
| `_queue_chamoisDisplayUUID` 由谁设置 | **低** | 静态未找到 `setChamoisDisplayUUID:` 调用者（可能在 SkyLight 侧或由配置流程设置），未展开 |
| 日志第三参数（event 的 `%{public}@`）在本机日志里恒为 71 个 `+` | **观察值** | 2322 条全部为 71 个 `+`；未深究其渲染逻辑（不影响判定结论） |
