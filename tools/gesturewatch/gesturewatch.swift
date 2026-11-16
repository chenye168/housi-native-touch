// gesturewatch.swift — macOS 27 触控/手势验证旁路记录器
//
// 目的：在用户于 HOUSI 外接触控屏（或内建触控板）上逐项做手势时，
// 不打扰操作地记录“系统实际发生了什么”，为每一手势提供客观证据：
//   1. 事件流：NSEvent 全局监听（scrollWheel / gesture / magnify / swipe / rotate /
//      smartMagnify / beginGesture / endGesture）+ CGEvent tap（原始 29~32 类型与 scroll）；
//   2. 窗口布局：每 0.5s 采样 CGWindowList，检测窗口增删/位移/缩放
//      → Mission Control、App Exposé、调度中心/台前调度、平铺、缩放、显示桌面；
//   3. 前台 App 与“空间切换”通知 → 三指左右轻扫切空间；
//   4. 系统日志旁路（由外层 shell 另行启动 log stream）。
//
// 输出：NDJSON（每行一个 JSON 对象），默认 evidence/gesturewatch-<ts>.ndjson
//
// 编译：swiftc -O -o build/gesturewatch gesturewatch.swift -framework Cocoa
// 运行：./build/gesturewatch --out ../evidence/gesturewatch.ndjson --duration 7200
//
// 依赖权限：辅助功能 + 输入监控（本机已授权：AXIsProcessTrusted=1、
// CGPreflightListenEventAccess=1、CGPreflightScreenCaptureAccess=1）。

import Cocoa
import CoreGraphics

// MARK: - 基础

let startedAt = Date()
let iso = ISO8601DateFormatter()
iso.formatOptions = [.withInternetDateTime, .withFractionalSeconds]

var outHandle: FileHandle?
var outPath: String = ""
var logQueue = DispatchQueue(label: "gesturewatch.log")

func emit(_ kind: String, _ fields: [String: Any] = [:]) {
    var d: [String: Any] = [
        "t": Double(round(Date().timeIntervalSince(startedAt) * 1000) / 1000),
        "ts": iso.string(from: Date()),
        "kind": kind,
    ]
    for (k, v) in fields { d[k] = v }
    guard let data = try? JSONSerialization.data(withJSONObject: d, options: []),
          var line = String(data: data, encoding: .utf8) else { return }
    line += "\n"
    logQueue.async {
        outHandle?.write(line.data(using: .utf8)!)
    }
}

func phaseName(_ p: NSEvent.Phase) -> String {
    if p.isEmpty { return "none" }
    var parts: [String] = []
    if p.contains(.began) { parts.append("began") }
    if p.contains(.stationary) { parts.append("stationary") }
    if p.contains(.changed) { parts.append("changed") }
    if p.contains(.ended) { parts.append("ended") }
    if p.contains(.cancelled) { parts.append("cancelled") }
    if p.contains(.mayBegin) { parts.append("mayBegin") }
    return parts.isEmpty ? "\(p.rawValue)" : parts.joined(separator: "+")
}

func typeName(_ t: NSEvent.EventType) -> String {
    switch t {
    case .gesture: return "gesture(29)"
    case .magnify: return "magnify(30)"
    case .swipe: return "swipe(31)"
    case .rotate: return "rotate(18)"
    case .smartMagnify: return "smartMagnify(32)"
    case .beginGesture: return "beginGesture(19)"
    case .endGesture: return "endGesture(20)"
    case .scrollWheel: return "scrollWheel(22)"
    case .pressure: return "pressure(34)"
    case .directTouch: return "directTouch(37)"
    default: return "type(\(t.rawValue))"
    }
}

// MARK: - 命令行参数

var duration: Double = 0          // 0 = 一直运行
var pollInterval: Double = 0.5
var args = CommandLine.arguments.dropFirst()
var it = args.makeIterator()
while let a = it.next() {
    switch a {
    case "--out":
        if let v = it.next() { outPath = v }
    case "--duration":
        if let v = it.next(), let d = Double(v) { duration = d }
    case "--interval":
        if let v = it.next(), let d = Double(v) { pollInterval = d }
    case "--help", "-h":
        print("用法: gesturewatch [--out PATH] [--duration 秒] [--interval 秒]")
        exit(0)
    default:
        break
    }
}

let workRoot = FileManager.default.currentDirectoryPath
if outPath.isEmpty {
    outPath = (workRoot as NSString).appendingPathComponent("evidence/gesturewatch.ndjson")
}
let outURL = URL(fileURLWithPath: outPath)
try? FileManager.default.createDirectory(at: outURL.deletingLastPathComponent(),
                                         withIntermediateDirectories: true)
FileManager.default.createFile(atPath: outURL.path, contents: nil)
outHandle = try? FileHandle(forWritingTo: outURL)
outHandle?.seekToEndOfFile()

// MARK: - 元信息

var displayInfo: [[String: Any]] = []
for (i, s) in NSScreen.screens.enumerated() {
    var d: [String: Any] = [
        "index": i,
        "name": s.localizedName,
        "frame": "\(Int(s.frame.origin.x)),\(Int(s.frame.origin.y)),\(Int(s.frame.width)),\(Int(s.frame.height))",
        "scale": s.backingScaleFactor,
    ]
    if let num = s.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber {
        let did = CGDirectDisplayID(num.uint32Value)
        d["displayID"] = did
        if let uuid = CGDisplayCreateUUIDFromDisplayID(did)?.takeRetainedValue() {
            d["uuid"] = (CFUUIDCreateString(nil, uuid) as String?) ?? ""
        }
    }
    displayInfo.append(d)
}
emit("meta", [
    "pid": ProcessInfo.processInfo.processIdentifier,
    "os": ProcessInfo.processInfo.operatingSystemVersionString,
    "screens": displayInfo,
    "out": outPath,
    "axTrusted": AXIsProcessTrusted(),
    "listenAccess": CGPreflightListenEventAccess(),
    "screenCapture": CGPreflightScreenCaptureAccess(),
])

// MARK: - NSEvent 全局监听

// 全局 NSEvent 监听会为新二进制触发“辅助功能”TCC 授权弹窗，默认关闭；
// 需要时加 --nsevent 参数启用（会弹一次系统授权框）。
if CommandLine.arguments.contains("--nsevent") {
    let mask: NSEvent.EventTypeMask = [
        .gesture, .magnify, .swipe, .rotate, .smartMagnify,
        .beginGesture, .endGesture, .scrollWheel, .pressure,
    ]
    _ = NSEvent.addGlobalMonitorForEvents(matching: mask) { e in
        var f: [String: Any] = [
            "type": typeName(e.type),
            "subtype": e.subtype.rawValue,
            "phase": phaseName(e.phase),
            "momentum": phaseName(e.momentumPhase),
            "loc": "\(Int(e.locationInWindow.x)),\(Int(e.locationInWindow.y))",
            "win": e.windowNumber,
            "flags": e.modifierFlags.rawValue,
        ]
        if e.type == .scrollWheel {
            f["dx"] = round(e.scrollingDeltaX * 10) / 10
            f["dy"] = round(e.scrollingDeltaY * 10) / 10
            f["precise"] = e.hasPreciseScrollingDeltas
        } else if e.type == .magnify {
            f["mag"] = round(e.magnification * 1000) / 1000
        } else if e.type == .rotate {
            f["rot"] = round(e.rotation * 10) / 10
        } else if e.type == .swipe {
            f["dx"] = e.deltaX
            f["dy"] = e.deltaY
        }
        emit("nsevent", f)
    }
}

// MARK: - CGEvent tap（原始事件，含 29~32）

let rawMask: CGEventMask =
    (1 << 29) | (1 << 30) | (1 << 31) | (1 << 32) |
    (1 << CGEventType.scrollWheel.rawValue)

// 原始 29 类事件在真实触摸时会以约 90Hz 连发；做限流（10Hz/类型），
// 只在相位字段非零时立即记录，避免日志爆量。
var lastRawLog: [Int: Date] = [:]
var droppedRaw: [Int: Int] = [:]

let tapCallback: CGEventTapCallBack = { _, type, event, _ in
    let phase = event.getIntegerValueField(CGEventField(rawValue: 99)!)          // scroll phase
    let gest = event.getIntegerValueField(CGEventField(rawValue: 115)!)          // gesture phase?
    let now = Date()
    let key = Int(type.rawValue)
    let shouldLog = phase != 0 || gest != 0 ||
        now.timeIntervalSince(lastRawLog[key] ?? .distantPast) >= 0.1
    if shouldLog {
        var fields: [String: Any] = [
            "cgType": key,
            "field99": phase,
            "field115": gest,
            "field123": event.getIntegerValueField(CGEventField(rawValue: 123)!),
            "loc": "\(Int(event.location.x)),\(Int(event.location.y))",
            "flags": event.flags.rawValue,
        ]
        if let d = droppedRaw[key], d > 0 {
            fields["suppressed"] = d
            droppedRaw[key] = 0
        }
        emit("cgevent", fields)
        lastRawLog[key] = now
    } else {
        droppedRaw[key, default: 0] += 1
    }
    return Unmanaged.passUnretained(event)
}

var tapPort: CFMachPort?
if let tap = CGEvent.tapCreate(tap: .cgSessionEventTap,
                               place: .headInsertEventTap,
                               options: .listenOnly,
                               eventsOfInterest: rawMask,
                               callback: tapCallback,
                               userInfo: nil) {
    tapPort = tap
    let src = CFMachPortCreateRunLoopSource(nil, tap, 0)
    CFRunLoopAddSource(CFRunLoopGetMain(), src, .commonModes)
    CGEvent.tapEnable(tap: tap, enable: true)
    emit("meta", ["eventTap": "ok"])
} else {
    emit("meta", ["eventTap": "failed"])
}

// MARK: - 窗口布局采样

func windowSnapshot() -> [String: [String: Any]] {
    var out: [String: [String: Any]] = [:]
    let opts: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
    guard let arr = CGWindowListCopyWindowInfo(opts, kCGNullWindowID) as? [[String: Any]] else {
        return out
    }
    for w in arr {
        guard let num = w[kCGWindowNumber as String] as? Int else { continue }
        let owner = (w[kCGWindowOwnerName as String] as? String) ?? "?"
        let layer = (w[kCGWindowLayer as String] as? Int) ?? 0
        var b = "0,0,0,0"
        if let bd = w[kCGWindowBounds as String] as? [String: Any],
           let x = bd["X"] as? Double, let y = bd["Y"] as? Double,
           let ww = bd["Width"] as? Double, let hh = bd["Height"] as? Double {
            b = "\(Int(x)),\(Int(y)),\(Int(ww)),\(Int(hh))"
        }
        out["\(num)"] = ["owner": owner, "layer": layer, "bounds": b]
    }
    return out
}

var lastSnapshot: [String: [String: Any]] = [:]
var lastSignature = ""

func layoutSignature(_ snap: [String: [String: Any]]) -> String {
    snap.values.map { v in
        let o = v["owner"] as? String ?? "?"
        let l = v["layer"] as? Int ?? 0
        let b = v["bounds"] as? String ?? ""
        // 只把普通窗口纳入签名，避免菜单/工具提示等噪声
        return l == 0 ? "\(o)|\(b)" : ""
    }.filter { !$0.isEmpty }.sorted().joined(separator: ";")
}

let layoutTimer = Timer(timeInterval: pollInterval, repeats: true) { _ in
    let snap = windowSnapshot()
    let sig = layoutSignature(snap)
    if sig != lastSignature {
        var added: [String] = []
        var removed: [String] = []
        var moved: [String] = []
        for (k, v) in snap where lastSnapshot[k] == nil {
            if (v["layer"] as? Int ?? 0) == 0 {
                added.append("\(v["owner"] ?? "?")@\(v["bounds"] ?? "?")")
            }
        }
        for (k, v) in lastSnapshot where snap[k] == nil {
            if (v["layer"] as? Int ?? 0) == 0 {
                removed.append("\(v["owner"] ?? "?")@\(v["bounds"] ?? "?")")
            }
        }
        for (k, v) in snap {
            if let old = lastSnapshot[k],
               (v["bounds"] as? String) != (old["bounds"] as? String) {
                moved.append("\(v["owner"] ?? "?") \(old["bounds"] ?? "?")→\(v["bounds"] ?? "?")")
            }
        }
        var f: [String: Any] = [
            "normals": snap.values.filter { ($0["layer"] as? Int ?? 0) == 0 }.count,
            "total": snap.count,
        ]
        if !added.isEmpty { f["added"] = Array(added.prefix(6)) }
        if !removed.isEmpty { f["removed"] = Array(removed.prefix(6)) }
        if !moved.isEmpty { f["moved"] = Array(moved.prefix(6)) }
        emit("layout", f)
        lastSignature = sig
        lastSnapshot = snap
    }
}
RunLoop.main.add(layoutTimer, forMode: .common)

// MARK: - 前台 App / 空间

let wsCenter = NSWorkspace.shared.notificationCenter
wsCenter.addObserver(forName: NSWorkspace.didActivateApplicationNotification,
                     object: nil, queue: .main) { n in
    if let app = n.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication {
        emit("frontApp", ["bundle": app.bundleIdentifier ?? "?", "name": app.localizedName ?? "?"])
    }
}
wsCenter.addObserver(forName: NSWorkspace.activeSpaceDidChangeNotification,
                     object: nil, queue: .main) { _ in
    emit("spaceChange", [:])
}

// 初始状态
lastSnapshot = windowSnapshot()
lastSignature = layoutSignature(lastSnapshot)
emit("layout", [
    "normals": lastSnapshot.values.filter { ($0["layer"] as? Int ?? 0) == 0 }.count,
    "total": lastSnapshot.count,
    "initial": true,
])

// MARK: - 退出

func finish(_ code: Int32) -> Never {
    emit("end", ["reason": code == 0 ? "duration" : "signal"])
    logQueue.sync {}
    try? outHandle?.close()
    exit(code)
}

if duration > 0 {
    DispatchQueue.main.asyncAfter(deadline: .now() + duration) { finish(0) }
}

signal(SIGTERM, SIG_IGN)
signal(SIGINT, SIG_IGN)
let sigSrc = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
sigSrc.setEventHandler { finish(1) }
sigSrc.resume()
let intSrc = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
intSrc.setEventHandler { finish(1) }
intSrc.resume()

RunLoop.main.run()
