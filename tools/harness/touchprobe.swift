// touchprobe.swift — AppKit 原生触控（NSEventTypeDirectTouch / gesture）验证探针
//
// 用途:
//   1. 打印所有 NSScreen 的 name / CGDirectDisplayID / UUID / touchCapabilities / frame，
//      并与 CGDisplayBounds（WindowServer 桌面坐标）对照，输出 HOUSI 屏的归一化坐标映射参考。
//   2. 监听 NSEventMaskDirectTouch（NSEventTypeDirectTouch = 37）与 NSEventMaskGesture，
//      把事件详情（eventNumber / type / subtype / timestamp / location / phase / NSTouch 集合）
//      实时打印到 stdout、窗口内文本视图，并追加写入 --logfile 指定的文件。
//   3. 支持 --duration N 秒后自动退出，便于自动化验证。
//
// 编译（仅需 Command Line Tools）:
//   swiftc -O -o build/touchprobe touchprobe.swift -framework AppKit
//
// 运行:
//   ./build/touchprobe --duration 2
//   ./build/touchprobe --duration 30 --logfile /tmp/touch.log
//
// 说明: 本程序只读取 AppKit 事件，不注入、不修改任何系统状态。

import AppKit
import CoreGraphics
import Foundation

// MARK: - 常量

/// 已知 HOUSI 外接触控屏的显示 UUID（lead 已实测）。用于自动选中覆盖窗口的屏幕。
let knownHousiUUID = "30D985CD-BE3F-4C0E-B055-C5E22CFF2E07"
/// HOUSI 屏在 WindowServer 桌面坐标中的已知 bounds（lead 实测），仅用于对照展示。
let knownHousiCGBounds = CGRect(x: -134, y: 848, width: 1600, height: 1066)
let knownHousiRotation = 180.0

// MARK: - 参数

struct Options {
    var logfile: String = ""
    var duration: Double = 0          // 0 = 一直运行
    var overlay: Bool = true          // 是否在 HOUSI 屏上铺一层触控接收窗口
    var allEvents: Bool = false       // 监听所有 AppKit 事件（调试用）
    var help: Bool = false
}

func executableRootLogPath() -> String {
    // 二进制位于 WORK/tools/harness/build/touchprobe 时，默认写到 WORK/evidence/touchprobe.log。
    let exe = Bundle.main.executablePath ?? CommandLine.arguments.first ?? ""
    if !exe.isEmpty {
        var url = URL(fileURLWithPath: exe).deletingLastPathComponent() // build
        for _ in 0..<3 {
            url = url.deletingLastPathComponent() // harness -> tools -> WORK
        }
        return url.appendingPathComponent("evidence/touchprobe.log").path
    }
    return FileManager.default.currentDirectoryPath + "/evidence/touchprobe.log"
}

func parseOptions() -> Options {
    var o = Options()
    o.logfile = executableRootLogPath()
    let args = Array(CommandLine.arguments.dropFirst())
    var i = 0
    func nextValue(_ name: String) -> String? {
        if i + 1 < args.count { i += 1; return args[i] }
        FileHandle.standardError.write(("缺参数: " + name + "\n").data(using: .utf8)!)
        return nil
    }
    while i < args.count {
        let a = args[i]
        if a == "--help" || a == "-h" { o.help = true }
        else if a == "--duration" || a == "--seconds" {
            if let v = nextValue(a), let d = Double(v) { o.duration = max(0, d) }
        } else if a.hasPrefix("--duration=") {
            if let d = Double(a.dropFirst("--duration=".count)) { o.duration = max(0, d) }
        } else if a == "--logfile" {
            if let v = nextValue(a) { o.logfile = v }
        } else if a.hasPrefix("--logfile=") {
            o.logfile = String(a.dropFirst("--logfile=".count))
        } else if a == "--no-overlay" { o.overlay = false }
        else if a == "--overlay" { o.overlay = true }
        else if a == "--all-events" { o.allEvents = true }
        else if a == "--" { /* ignore */ }
        else {
            FileHandle.standardError.write(("未知参数: " + a + "（--help 查看用法）\n").data(using: .utf8)!)
        }
        i += 1
    }
    return o
}

func printUsage() {
    print("""
    touchprobe — AppKit 原生触控验证探针

    用法:
      touchprobe [--duration 秒] [--logfile 路径] [--no-overlay] [--all-events]

    选项:
      --duration N     运行 N 秒后自动退出（默认 0 = 一直运行，Ctrl-C 退出）
      --logfile PATH   事件日志文件（追加写，默认 WORK/evidence/touchprobe.log）
      --no-overlay     不在 HOUSI 触控屏上创建全屏触控接收窗口
      --overlay        强制在 HOUSI 屏上创建覆盖窗口（默认行为）
      --all-events     额外监听 NSEventMaskAny（调试用，事件很多）
      --help/-h        显示本帮助

    行为:
      * 启动时打印 NSScreen / CGDisplay 信息，并把同样的信息写进日志；
      * 监听 NSEventMaskDirectTouch 与 NSEventMaskGesture（本地 + 全局）；
      * 主窗口浮于最前显示“请触摸 HOUSI 屏幕”；默认在 HOUSI 屏上覆盖一个
        接收 direct touch 的透明窗口（窗口内容显示提示文字）；
      * 每个事件写一行带 ISO8601 毫秒时间戳的日志（stdout + 日志文件 + 窗口文本）。
    """)
}

// MARK: - 日志

final class Logger {
    let path: String
    private var handle: FileHandle?
    private let formatter: DateFormatter
    private let iso: ISO8601DateFormatter
    private var fileErrorPrinted = false
    private(set) var lines = 0
    var textView: NSTextView?

    init(path: String) {
        self.path = path
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd'T'HH:mm:ss.SSSZZZZZ"
        formatter = f
        iso = ISO8601DateFormatter()
        iso.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        prepareFile()
    }

    private func prepareFile() {
        let url = URL(fileURLWithPath: path)
        let dir = url.deletingLastPathComponent()
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        } catch {
            warnFile("无法创建日志目录 \(dir.path): \(error)")
            return
        }
        if !FileManager.default.fileExists(atPath: path) {
            FileManager.default.createFile(atPath: path, contents: nil)
        }
        handle = FileHandle(forWritingAtPath: path)
        if handle == nil {
            warnFile("无法打开日志文件 \(path)，仅输出到 stdout/窗口")
        } else {
            handle?.seekToEndOfFile()
        }
    }

    private func warnFile(_ msg: String) {
        if !fileErrorPrinted {
            fileErrorPrinted = true
            FileHandle.standardError.write(("[touchprobe] " + msg + "\n").data(using: .utf8)!)
        }
    }

    private func writeRaw(_ line: String) {
        lines += 1
        print(line)
        fflush(stdout)
        if let h = handle {
            do { try h.write(contentsOf: (line + "\n").data(using: .utf8)!) }
            catch { warnFile("写日志失败: \(error)") }
        }
        appendToView(line)
    }

    private func appendToView(_ line: String) {
        guard let tv = textView else { return }
        let color: NSColor
        if line.contains(" [ERR ]") { color = .systemRed }
        else if line.contains(" [EVT ]") { color = .systemBlue }
        else if line.contains(" [TOUCH]") { color = .systemGreen }
        else if line.contains(" [GEST]") { color = .systemPurple }
        else { color = NSColor.labelColor }
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedSystemFont(ofSize: 11, weight: .regular),
            .foregroundColor: color,
        ]
        func apply() {
            guard let storage = tv.textStorage else { return }
            storage.append(NSAttributedString(string: line + "\n", attributes: attrs))
            tv.scrollRangeToVisible(NSRange(location: storage.length, length: 0))
        }
        if Thread.isMainThread { apply() } else { DispatchQueue.main.async(execute: apply) }
    }

    func log(_ tag: String, _ msg: String) {
        let ts = formatter.string(from: Date())
        writeRaw("\(ts) [\(tag.padding(toLength: 5, withPad: " ", startingAt: 0))] \(msg)")
    }

    func event(_ msg: String) { log("EVT", msg) }
    func touch(_ msg: String) { log("TOUCH", msg) }
    func gesture(_ msg: String) { log("GEST", msg) }
    func err(_ msg: String) { log("ERR", msg) }
    func sys(_ msg: String) { log("SYS", msg) }

    func close() {
        handle?.synchronizeFile()
        handle?.closeFile()
        handle = nil
    }
}

// MARK: - 屏幕信息

func screenUUID(_ screen: NSScreen) -> String {
    let id = displayID(of: screen)
    guard id != 0, let cf = CGDisplayCreateUUIDFromDisplayID(id) else { return "nil" }
    return CFUUIDCreateString(nil, cf.takeRetainedValue()) as String
}

func displayID(of screen: NSScreen) -> CGDirectDisplayID {
    (screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value ?? 0
}

func touchCapabilitiesString(_ screen: NSScreen) -> String {
    let raw = touchCapabilitiesRaw(of: screen)
    var names: [String] = []
    if raw & 1 != 0 { names.append("MultiTouch") }
    if raw == 0 { names.append("None") }
    return "\(raw)" + (names.isEmpty ? "" : " (\(names.joined(separator: "|")))")
}

@available(macOS 27.0, *)
func touchCapabilities27(_ screen: NSScreen) -> UInt {
    UInt(screen.touchCapabilities.rawValue)
}

func touchCapabilitiesRaw(of screen: NSScreen) -> UInt {
    if #available(macOS 27.0, *) { return touchCapabilities27(screen) }
    return 0
}

func rotationString(_ id: CGDirectDisplayID) -> String {
    let r = CGDisplayRotation(id)
    return String(format: "%.0f°", r)
}

/// 打印/返回所有屏幕信息与 HOUSI 坐标映射参考。
func screenReport() -> [String] {
    var out: [String] = []
    let screens = NSScreen.screens
    out.append("=== NSScreen.screens (\(screens.count)) ===")
    for (i, s) in screens.enumerated() {
        let id = displayID(of: s)
        let uuid = screenUUID(s)
        let f = s.frame
        let cb = id != 0 ? CGDisplayBounds(id) : .zero
        let isHousi = uuid.caseInsensitiveCompare(knownHousiUUID) == .orderedSame
        out.append(String(format: "[%d] name=%@ displayID=%u uuid=%@ touchCapabilities=%@ main=%@",
                          i, s.localizedName.isEmpty ? "<空>" : s.localizedName, id, uuid, touchCapabilitiesString(s),
                          (s == NSScreen.main) ? "YES" : "no"))
        out.append(String(format: "    NSScreen.frame=%@ visibleFrame=%@ backingScale=%.1f",
                          NSStringFromRect(f), NSStringFromRect(s.visibleFrame), s.backingScaleFactor))
        out.append(String(format: "    CGDisplayBounds=%@ rotation=%@",
                          NSStringFromRect(cb), rotationString(id)))
        out.append("    HOUSI? = \(isHousi ? "YES (UUID 匹配)" : "no")")
    }

    out.append("=== CGGetOnlineDisplayList ===")
    var ids = [CGDirectDisplayID](repeating: 0, count: 32)
    var count: UInt32 = 0
    let err = CGGetOnlineDisplayList(32, &ids, &count)
    out.append("CGGetOnlineDisplayList err=\(err) count=\(count)")
    for i in 0..<Int(count) {
        let id = ids[i]
        var uuid = "nil"
        if let cf = CGDisplayCreateUUIDFromDisplayID(id) {
            uuid = CFUUIDCreateString(nil, cf.takeRetainedValue()) as String
        }
        out.append(String(format: "  id=%u uuid=%@ main=%@ builtin=%@ active=%@ bounds=%@ rotation=%.0f°",
                          id, uuid, "\(CGDisplayIsMain(id))", "\(CGDisplayIsBuiltin(id))",
                          "\(CGDisplayIsActive(id))", NSStringFromRect(CGDisplayBounds(id)),
                          CGDisplayRotation(id)))
    }

    out.append("=== HOUSI 映射参考（已知 UUID=\(knownHousiUUID)） ===")
    if let housi = screens.first(where: { screenUUID($0).caseInsensitiveCompare(knownHousiUUID) == .orderedSame }) {
        let id = displayID(of: housi)
        let f = housi.frame
        out.append("NSScreen: \(housi.localizedName) id=\(id) frame=\(NSStringFromRect(f))")
        out.append("CGDisplayBounds: \(NSStringFromRect(CGDisplayBounds(id))) (lead 实测已知: \(NSStringFromRect(knownHousiCGBounds)))")
        out.append("rotation=\(rotationString(id)) (lead 实测已知: \(Int(knownHousiRotation))°)")
        out.append("注意: NSScreen 坐标系原点在主屏左下、y 向上；CGDisplayBounds 原点在主屏左上、y 向下，")
        out.append("      同一块屏的 frame 与 bounds 通过主屏高度互相换算（主屏 height=\(Int(NSScreen.screens.first(where: { $0 == NSScreen.main })?.frame.height ?? 0))）。")
        out.append("归一化触控坐标映射候选（NSTouch.normalizedPosition 以触控面板左下为 (0,0)）:")
        out.append("  normalized -> 直映射(NSScreen 坐标)            -> 按 rotation 180° 翻转候选")
        for (nx, ny) in [(0.0, 0.0), (0.5, 0.5), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)] {
            let direct = CGPoint(x: f.minX + nx * f.width, y: f.minY + ny * f.height)
            let rot = CGPoint(x: f.maxX - nx * f.width, y: f.maxY - ny * f.height)
            out.append(String(format: "  (%.2f,%.2f) -> (%.1f,%.1f) -> (%.1f,%.1f)", nx, ny,
                              direct.x, direct.y, rot.x, rot.y))
        }
    } else {
        out.append("未找到 UUID=\(knownHousiUUID) 的 NSScreen（共 \(screens.count) 块屏）。")
    }

    out.append("=== 事件监听配置 ===")
    out.append("NSEventTypeDirectTouch = \(NSEvent.EventType.directTouch.rawValue), NSEventTypeGesture = \(NSEvent.EventType.gesture.rawValue)")
    out.append("NSEventMaskDirectTouch raw = \(NSEvent.EventTypeMask.directTouch.rawValue)")
    return out
}

// MARK: - 事件描述

func eventTypeName(_ t: NSEvent.EventType) -> String {
    switch t {
    case .directTouch: return "directTouch(37)"
    case .gesture: return "gesture"
    case .magnify: return "magnify"
    case .swipe: return "swipe"
    case .rotate: return "rotate"
    case .beginGesture: return "beginGesture"
    case .endGesture: return "endGesture"
    case .scrollWheel: return "scrollWheel"
    case .smartMagnify: return "smartMagnify"
    case .pressure: return "pressure"
    case .mouseMoved: return "mouseMoved"
    case .leftMouseDown: return "leftMouseDown"
    case .leftMouseUp: return "leftMouseUp"
    case .leftMouseDragged: return "leftMouseDragged"
    case .rightMouseDown: return "rightMouseDown"
    case .rightMouseUp: return "rightMouseUp"
    case .keyDown: return "keyDown"
    case .keyUp: return "keyUp"
    case .flagsChanged: return "flagsChanged"
    case .appKitDefined: return "appKitDefined"
    case .systemDefined: return "systemDefined"
    case .applicationDefined: return "applicationDefined"
    case .periodic: return "periodic"
    case .cursorUpdate: return "cursorUpdate"
    case .tabletPoint: return "tabletPoint"
    case .tabletProximity: return "tabletProximity"
    case .otherMouseDown: return "otherMouseDown"
    case .otherMouseUp: return "otherMouseUp"
    case .otherMouseDragged: return "otherMouseDragged"
    case .mouseEntered: return "mouseEntered"
    case .mouseExited: return "mouseExited"
    default: return "type(\(t.rawValue))"
    }
}

func phaseName(_ p: NSEvent.Phase) -> String {
    if p.rawValue == 0 { return "none(0)" }
    var parts: [String] = []
    if p.contains(.began) { parts.append("began") }
    if p.contains(.stationary) { parts.append("stationary") }
    if p.contains(.changed) { parts.append("changed") }
    if p.contains(.ended) { parts.append("ended") }
    if p.contains(.cancelled) { parts.append("cancelled") }
    if p.contains(.mayBegin) { parts.append("mayBegin") }
    return "\(parts.joined(separator: "|"))(\(p.rawValue))"
}

func touchPhaseName(_ p: NSTouch.Phase) -> String {
    var parts: [String] = []
    if p.contains(.began) { parts.append("began") }
    if p.contains(.moved) { parts.append("moved") }
    if p.contains(.stationary) { parts.append("stationary") }
    if p.contains(.ended) { parts.append("ended") }
    if p.contains(.cancelled) { parts.append("cancelled") }
    if p.isEmpty { parts.append("none") }
    return parts.joined(separator: "|")
}

func touchTypeName(_ t: NSTouch) -> String {
    if #available(macOS 10.12.2, *) {
        switch t.type {
        case .direct: return "direct"
        case .indirect: return "indirect"
        @unknown default: return "type(\(t.type.rawValue))"
        }
    }
    return "?"
}

func describeTouch(_ touch: NSTouch, index: Int, screen: NSScreen?, logger: Logger) -> String {
    let np = touch.normalizedPosition
    let size = touch.deviceSize
    let identHash = (touch.identity as? NSObject)?.hashValue ?? 0
    var device = "nil"
    if let d = touch.device { device = String(describing: d); if device.count > 160 { device = String(device.prefix(160)) + "…" } }
    var loc = "?"
    if #available(macOS 10.12.2, *) {
        let p = touch.location(in: nil)
        let pp = touch.previousLocation(in: nil)
        loc = String(format: "(%.2f,%.2f) prev=(%.2f,%.2f)", p.x, p.y, pp.x, pp.y)
    }
    var mapInfo = ""
    if let s = screen {
        let f = s.frame
        let direct = CGPoint(x: f.minX + np.x * f.width, y: f.minY + np.y * f.height)
        let rot = CGPoint(x: f.maxX - np.x * f.width, y: f.maxY - np.y * f.height)
        mapInfo = String(format: " ->HOUSI直接(%.1f,%.1f) rot180(%.1f,%.1f)", direct.x, direct.y, rot.x, rot.y)
    }
    return String(format: "  touch[%d] idHash=%d phase=%@ type=%@ normalized=(%.5f,%.5f) resting=%@ deviceSize=(%.1f,%.1f) locRoot=%@%@ device=%@",
                  index, identHash, touchPhaseName(touch.phase), touchTypeName(touch),
                  np.x, np.y, touch.isResting ? "YES" : "no", size.width, size.height,
                  loc, mapInfo, device)
}

// MARK: - 触控视图

/// 接收 direct touch 的视图：覆盖 HOUSI 屏，或作为主窗口的 contentView。
final class TouchView: NSView {
    weak var app: AppDelegate?
    var hint: String = ""
    var hintLabel: NSTextField?

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        allowedTouchTypes = [.direct, .indirect]
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override var acceptsFirstResponder: Bool { true }

    override func touchesBegan(with event: NSEvent) {
        app?.handleViewTouches(event, phase: "Began")
    }
    override func touchesMoved(with event: NSEvent) {
        app?.handleViewTouches(event, phase: "Moved")
    }
    override func touchesEnded(with event: NSEvent) {
        app?.handleViewTouches(event, phase: "Ended")
    }
    override func touchesCancelled(with event: NSEvent) {
        app?.handleViewTouches(event, phase: "Cancelled")
    }
}

// MARK: - 覆盖窗口

final class OverlayWindow: NSWindow {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { false }
}

// MARK: - AppDelegate

final class AppDelegate: NSObject, NSApplicationDelegate {
    let options: Options
    let logger: Logger
    private var window: NSWindow?
    private var overlayWindow: OverlayWindow?
    private var textView: NSTextView?
    private var localMonitor: Any?
    private var globalMonitor: Any?
    private var signalSources: [DispatchSourceSignal] = []
    private var durationTimer: Timer?
    private let startedAt = Date()

    var directTouchCount = 0
    var gestureCount = 0
    var otherEventCount = 0
    var touchBeganCount = 0
    var touchEndedCount = 0
    var stationTouchBegins = 0
    private var lastViewSignature = ""
    private var lastMonitorSignature = ""
    private var targetScreen: NSScreen?
    private var targetScreenIsKnownHousi = false
    private var rawEventTypeCounts: [UInt: Int] = [:]
    private var summaryWritten = false

    init(options: Options) {
        self.options = options
        self.logger = Logger(path: options.logfile)
        super.init()
    }

    // 选中用于覆盖窗口的触控屏：优先已知 HOUSI UUID，其次非主屏且 touchCapabilities 非 0 的屏。
    func pickTargetScreen() {
        let screens = NSScreen.screens
        if let known = screens.first(where: {
            screenUUID($0).caseInsensitiveCompare(knownHousiUUID) == .orderedSame
        }) {
            targetScreen = known
            targetScreenIsKnownHousi = true
            return
        }
        let candidates = screens.filter { s in
            guard s != NSScreen.main else { return false }
            return touchCapabilitiesRaw(of: s) != 0
        }
        if let c = candidates.first ?? screens.last {
            targetScreen = c
        }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildUI()
        logger.sys("touchprobe start pid=\(ProcessInfo.processInfo.processIdentifier) os=\(ProcessInfo.processInfo.operatingSystemVersionString)")
        logger.sys("logfile=\(options.logfile)")
        let report = screenReport()
        for line in report { logger.sys(line) }
        logger.sys("提示: 请触摸 HOUSI 屏幕（外接触控屏），事件会实时写入上方日志与 \(options.logfile)")
        installMonitors()
        installSignalHandlers()
        scheduleDurationExit()
        if options.duration > 0 {
            logger.sys("将在 \(options.duration) 秒后自动退出")
        }
        NSApp.activate(ignoringOtherApps: true)
        window?.makeKeyAndOrderFront(nil)
        logWindowState(prefix: "启动时")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) { [weak self] in
            self?.logWindowState(prefix: "0.35s 后")
        }
    }

    /// 记录窗口是否真的显示（供 agent 从日志确认 GUI 正常创建）。
    func logWindowState(prefix: String) {
        let wins = NSApp.windows
        logger.sys("\(prefix) NSApp.windows=\(wins.count)")
        for (i, w) in wins.enumerated() {
            logger.sys(String(format: "  win[%d] number=%d title=%@ visible=%@ level=%d frame=%@ screen=%@",
                              i, w.windowNumber, w.title, w.isVisible ? "YES" : "no",
                              w.level.rawValue, NSStringFromRect(w.frame),
                              w.screen.flatMap { $0.localizedName.isEmpty ? nil : $0.localizedName } ?? "<空>"))
        }
    }

    private func buildUI() {
        pickTargetScreen()
        let contentRect = NSRect(x: 0, y: 0, width: 1000, height: 760)
        let w = NSWindow(contentRect: contentRect,
                         styleMask: [.titled, .closable, .resizable, .miniaturizable],
                         backing: .buffered, defer: false)
        w.title = "touchprobe — AppKit 原生触控验证（NSEventTypeDirectTouch=37）"
        w.level = .floating
        w.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        w.isReleasedWhenClosed = false
        w.center()

        let content = TouchView(frame: contentRect)
        content.app = self
        content.autoresizingMask = [.width, .height]
        w.contentView = content

        let hintLabel = NSTextField(labelWithString: "请触摸 HOUSI 屏幕（也可触摸内置屏）。事件将实时记录。")
        hintLabel.frame = NSRect(x: 12, y: contentRect.height - 34, width: contentRect.width - 24, height: 24)
        hintLabel.autoresizingMask = [.width, .minYMargin]
        hintLabel.font = NSFont.boldSystemFont(ofSize: 15)
        hintLabel.textColor = .systemYellow
        hintLabel.drawsBackground = true
        hintLabel.backgroundColor = NSColor(calibratedWhite: 0.12, alpha: 1)
        content.addSubview(hintLabel)

        let scroll = NSScrollView(frame: NSRect(x: 0, y: 0, width: contentRect.width, height: contentRect.height - 40))
        scroll.autoresizingMask = [.width, .height]
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.borderType = .bezelBorder
        let tv = NSTextView(frame: scroll.bounds)
        tv.isEditable = false
        tv.isSelectable = true
        tv.isVerticallyResizable = true
        tv.isHorizontallyResizable = false
        tv.autoresizingMask = [.width]
        tv.font = NSFont.monospacedSystemFont(ofSize: 11, weight: .regular)
        tv.textContainer?.widthTracksTextView = true
        tv.textContainer?.containerSize = NSSize(width: scroll.contentSize.width, height: .greatestFiniteMagnitude)
        tv.minSize = NSSize(width: 0, height: 0)
        tv.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        scroll.documentView = tv
        content.addSubview(scroll)
        textView = tv
        logger.textView = tv

        window = w
        w.makeKeyAndOrderFront(nil)

        // 在目标触控屏上创建覆盖窗口，保证 direct touch 事件落到本 App 的视图。
        if options.overlay, let s = targetScreen {
            logger.sys("overlay screen: \(s.localizedName) uuid=\(screenUUID(s)) frame=\(NSStringFromRect(s.frame))")
        } else {
            logger.sys("overlay disabled 或未找到目标触控屏（direct touch 仍可能由主窗口接收）")
        }
        if options.overlay, let s = targetScreen {
            // 注意: 指定 screen: 时 contentRect 使用该屏的本地坐标（左下为 0,0），
            // 若传全局 frame 会被再偏移一次，因此这里用 (0,0,size)。
            let ov = OverlayWindow(contentRect: NSRect(origin: .zero, size: s.frame.size),
                                   styleMask: [.borderless], backing: .buffered,
                                   defer: false, screen: s)
            ov.level = .floating
            ov.backgroundColor = NSColor(calibratedWhite: 0, alpha: 0.30)
            ov.isOpaque = false
            ov.hasShadow = false
            ov.ignoresMouseEvents = false
            ov.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]
            let tv = TouchView(frame: NSRect(origin: .zero, size: s.frame.size))
            tv.app = self
            tv.allowedTouchTypes = [.direct, .indirect]
            tv.wantsLayer = true
            tv.layer?.backgroundColor = NSColor(calibratedWhite: 0, alpha: 0.25).cgColor
            let label = NSTextField(labelWithString: "请触摸此屏幕（HOUSI 触控屏）\n触摸事件将记录到主窗口与日志文件")
            label.font = NSFont.boldSystemFont(ofSize: 28)
            label.textColor = .white
            label.alignment = .center
            label.drawsBackground = true
            label.backgroundColor = NSColor(calibratedWhite: 0.1, alpha: 0.6)
            label.frame = NSRect(x: 0, y: s.frame.height / 2 - 60, width: s.frame.width, height: 120)
            label.autoresizingMask = [.width, .minYMargin, .maxYMargin]
            tv.addSubview(label)
            tv.hintLabel = label
            ov.contentView = tv
            ov.makeKeyAndOrderFront(nil)
            ov.orderFrontRegardless()
            overlayWindow = ov
            window?.makeKeyAndOrderFront(nil)
            logger.sys("overlay window created on screen frame=\(NSStringFromRect(s.frame)) level=floating")
        }
    }

    private func installMonitors() {
        var mask: NSEvent.EventTypeMask = [.directTouch, .gesture]
        if options.allEvents { mask = .any }
        logger.sys("local monitor mask=\(mask.rawValue) types=[directTouch, gesture]\(options.allEvents ? " + all" : "")")
        localMonitor = NSEvent.addLocalMonitorForEvents(matching: mask) { [weak self] e in
            self?.handle(e, source: "local")
            return e
        }
        // 全局监听: 需要辅助功能/输入监控授权才可能收到其他 App 的事件；收不到属正常。
        globalMonitor = NSEvent.addGlobalMonitorForEvents(matching: mask) { [weak self] e in
            self?.handle(e, source: "global")
        }
        logger.sys("global monitor installed (需要 辅助功能/输入监控 权限才可能收到其他 app 的事件)")
    }

    private func installSignalHandlers() {
        for sig in [SIGINT, SIGTERM] {
            signal(sig, SIG_IGN)
            let src = DispatchSource.makeSignalSource(signal: sig, queue: .main)
            src.setEventHandler { [weak self] in
                self?.logger.sys("收到信号 \(sig)，退出")
                self?.writeSummaryAndTerminate()
            }
            src.resume()
            signalSources.append(src)
        }
    }

    private func scheduleDurationExit() {
        guard options.duration > 0 else { return }
        let t = Timer.scheduledTimer(withTimeInterval: options.duration, repeats: false) { [weak self] _ in
            self?.logger.sys("--duration \(self?.options.duration ?? 0)s 已到，退出")
            self?.writeSummaryAndTerminate()
        }
        RunLoop.main.add(t, forMode: .common)
        durationTimer = t
    }

    private func summaryText() -> [String] {
        var out: [String] = []
        out.append("=== SUMMARY ===")
        out.append("duration=\(String(format: "%.2f", Date().timeIntervalSince(startedAt)))s directTouchEvents=\(directTouchCount) gestureEvents=\(gestureCount) otherEvents=\(otherEventCount)")
        out.append("touchBegan=\(touchBeganCount) touchEnded=\(touchEndedCount) stationTouchBegins=\(stationTouchBegins)")
        if !rawEventTypeCounts.isEmpty {
            let parts = rawEventTypeCounts.sorted { $0.key < $1.key }.map { "type\($0.key)=\($0.value)" }
            out.append("eventTypeCounts: \(parts.joined(separator: " "))")
        }
        if directTouchCount == 0 && gestureCount == 0 {
            out.append("未收到 directTouch/gesture 事件。若你确实触摸了 HOUSI 屏，这通常意味着 WindowServer")
            out.append("在该 digitizer 服务上丢弃了事件（'ignoring digitizer event for display ... from unsupported service'），")
            out.append("可用 log show --last 2m --predicate 'category == \"TouchEvents\"' 交叉验证。")
        }
        out.append("logfile=\(options.logfile)")
        return out
    }

    func writeSummaryAndTerminate() {
        if !summaryWritten {
            summaryWritten = true
            for line in summaryText() { logger.sys(line) }
        }
        logger.close()
        DispatchQueue.main.async {
            NSApp.terminate(nil)
            // 兜底: terminate 后若仍存活则强制退出
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { exit(0) }
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        if !summaryWritten {
            summaryWritten = true
            for line in summaryText() { logger.sys(line) }
        }
        if let m = localMonitor { NSEvent.removeMonitor(m); localMonitor = nil }
        if let m = globalMonitor { NSEvent.removeMonitor(m); globalMonitor = nil }
        logger.close()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    // MARK: 事件处理

    private func touchSet(for event: NSEvent) -> Set<NSTouch> {
        if event.type == .gesture {
            return event.allTouches()
        }
        return event.touches(matching: .any, in: nil)
    }

    func handle(_ event: NSEvent, source: String) {
        let touches = touchSet(for: event)
        let sig = "\(event.type.rawValue)|\(event.timestamp)|\(touches.count)"
        if sig == lastMonitorSignature { return } // 同一事件的 monitor/view 重复回调去重
        lastMonitorSignature = sig

        rawEventTypeCounts[UInt(event.type.rawValue), default: 0] += 1
        let touchDesc = touches.enumerated().map { describeTouch($0.element, index: $0.offset, screen: targetScreen, logger: logger) }

        switch event.type {
        case .directTouch:
            directTouchCount += 1
            lastViewSignature = sig
            logger.event("src=\(source) \(baseEventLine(event)) touches=\(touches.count)")
            for l in touchDesc { logger.touch(l) }
            if touches.isEmpty {
                logger.touch("  (directTouch 事件没有携带 NSTouch 集合 — 可能来自非物理触摸设备/合成事件)")
            }
            for t in touches {
                if t.phase.contains(.began) { touchBeganCount += 1 }
                if t.phase.contains(.ended) || t.phase.contains(.cancelled) { touchEndedCount += 1 }
            }
        case .gesture:
            gestureCount += 1
            lastViewSignature = sig
            logger.gesture("src=\(source) \(baseEventLine(event)) allTouches=\(touches.count)")
            if let sub = gestureSubtypeName(event) { logger.gesture("  subtype=\(sub)") }
            for l in touchDesc { logger.gesture(l) }
        default:
            guard options.allEvents else { return }
            otherEventCount += 1
            logger.event("src=\(source) \(baseEventLine(event)) touches=\(touches.count)")
        }
    }

    /// 视图 touches* 回调 (事件已由 local monitor 记录过则跳过，避免重复)。
    func handleViewTouches(_ event: NSEvent, phase: String) {
        let touches = event.touches(matching: .any, in: nil)
        let sig = "\(event.type.rawValue)|\(event.timestamp)|\(touches.count)"
        if sig == lastMonitorSignature || sig == lastViewSignature {
            lastViewSignature = sig
            return
        }
        lastViewSignature = sig
        if event.type == .directTouch || touches.contains(where: { t in
            if #available(macOS 10.12.2, *) { return t.type == .direct } else { return false }
        }) {
            stationTouchBegins += (phase == "Began") ? 1 : 0
            directTouchCount += 1
            logger.touch("src=view touches\(phase) \(baseEventLine(event)) touches=\(touches.count)")
            for (i, t) in touches.enumerated() { logger.touch(describeTouch(t, index: i, screen: targetScreen, logger: logger)) }
        }
    }

    private func baseEventLine(_ e: NSEvent) -> String {
        var parts: [String] = []
        parts.append("type=\(eventTypeName(e.type))(\(e.type.rawValue))")
        parts.append("subtype=\(e.subtype.rawValue)")
        parts.append("eventNumber=\(e.eventNumber)")
        parts.append("timestamp=\(String(format: "%.6f", e.timestamp))")
        parts.append("phase=\(phaseName(e.phase))")
        let loc = e.locationInWindow
        parts.append("locationInWindow=(\(String(format: "%.2f", loc.x)),\(String(format: "%.2f", loc.y)))")
        if let w = e.window {
            parts.append("window=\(w.windowNumber)")
            parts.append("windowFrame=\(NSStringFromRect(w.frame))")
            parts.append("screen=\(w.screen?.localizedName ?? "nil")")
            let screenPt = w.convertPoint(toScreen: loc)
            parts.append("screenPoint=(\(String(format: "%.2f", screenPt.x)),\(String(format: "%.2f", screenPt.y)))")
        } else {
            parts.append("window=nil")
        }
        parts.append("modifierFlags=\(e.modifierFlags.rawValue)")
        return parts.joined(separator: " ")
    }

    private func gestureSubtypeName(_ e: NSEvent) -> String? {
        guard e.type == .gesture else { return nil }
        switch e.subtype.rawValue {
        case 0: return "mouseEvent(0)"
        case 3: return "applicationActivated(3)"
        case 9: return "touch(9)"
        case 32: return "smartMagnify(32)"
        default: return "subtype(\(e.subtype.rawValue))"
        }
    }
}

// MARK: - main

let options = parseOptions()
if options.help {
    printUsage()
    exit(0)
}
setvbuf(stdout, nil, _IONBF, 0)
setvbuf(stderr, nil, _IONBF, 0)

let app = NSApplication.shared
app.setActivationPolicy(.regular)
let delegate = AppDelegate(options: options)
app.delegate = delegate
app.run()
