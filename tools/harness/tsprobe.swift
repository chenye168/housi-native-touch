// tsprobe.swift — minimal, self-contained direct-touch delivery probe for macOS 27
// Purpose: find out whether HOUSI external touchscreen touches are delivered to an
// active AppKit app whose window is on the HOUSI display, and in which NSEvent form.
//
// build: swiftc -O -o tsprobe tsprobe.swift -framework AppKit
// run:   ./tsprobe --duration 40 [--logfile evidence/tsprobe.log] [--overlay]
import AppKit
import CoreGraphics

let HOUSI_UUID = "30D985CD-BE3F-4C0E-B055-C5E22CFF2E07"

// MARK: - logging
let dateFmt: DateFormatter = {
    let f = DateFormatter()
    f.locale = Locale(identifier: "en_US_POSIX")
    f.dateFormat = "yyyy-MM-dd'T'HH:mm:ss.SSSZZZZZ"
    return f
}()

var logHandle: FileHandle?
func log(_ tag: String, _ msg: String) {
    let line = "\(dateFmt.string(from: Date())) [\(tag)] \(msg)"
    print(line)
    fflush(stdout)
    if let h = logHandle { try? h.write(contentsOf: (line + "\n").data(using: .utf8)!) }
}

func displayID(of screen: NSScreen) -> CGDirectDisplayID {
    (screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value ?? 0
}
func screenUUID(_ screen: NSScreen?) -> String {
    guard let s = screen, displayID(of: s) != 0,
          let cf = CGDisplayCreateUUIDFromDisplayID(displayID(of: s)) else { return "nil" }
    return CFUUIDCreateString(nil, cf.takeRetainedValue()) as String
}
func touchCap(_ screen: NSScreen) -> UInt {
    if #available(macOS 27.0, *) { return UInt(screen.touchCapabilities.rawValue) }
    return 0
}

func touchTypeName(_ t: NSTouch) -> String {
    if #available(macOS 10.12.2, *) {
        switch t.type {
        case .direct: return "direct"
        case .indirect: return "indirect"
        @unknown default: return "unknown(\(t.type.rawValue))"
        }
    }
    return "unknown"
}

func describeTouch(_ t: NSTouch, index: Int) -> String {
    let n = t.normalizedPosition
    let ds = t.deviceSize
    var dev = "nil"
    if let d = t.device { dev = String(describing: d) }
    return String(format: "  touch[%d] type=%@ phase=%@ resting=%@ normalized=(%.5f,%.5f) deviceSize=(%.1f,%.1f) device=%@ identity=%@",
                  index, touchTypeName(t), "\(t.phase.rawValue)", t.isResting ? "YES" : "no",
                  n.x, n.y, ds.width, ds.height, dev, String(describing: t.identity))
}

func touchesOf(_ e: NSEvent) -> [NSTouch] {
    if e.type == .gesture { return Array(e.allTouches()) }
    if #available(macOS 10.12.2, *) {
        if e.type == .directTouch { return Array(e.touches(matching: .any, in: nil)) }
    }
    return Array(e.touches(matching: .any, in: nil))
}

// MARK: - window that can become key (borderless windows can't by default)
final class ProbeWindow: NSWindow {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }
}

// MARK: - touch view
final class TouchView: NSView {
    weak var app: AppDelegate?
    override var acceptsFirstResponder: Bool { true }
    override var acceptsTouchEvents: Bool {
        get { true }
        set { }
    }
    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsRestingTouches = true
        if #available(macOS 10.12.2, *) { allowedTouchTypes = [.direct, .indirect] }
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
    }
    required init?(coder: NSCoder) { fatalError() }
    override func draw(_ dirtyRect: NSRect) {
        NSColor.black.setFill(); dirtyRect.fill()
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.boldSystemFont(ofSize: 34),
            .foregroundColor: NSColor.systemYellow,
        ]
        let s = NSAttributedString(string: "touchprobe-v2\n请触摸 HOUSI 屏幕", attributes: attrs)
        let size = s.size()
        s.draw(at: NSPoint(x: (bounds.width - size.width) / 2, y: (bounds.height - size.height) / 2))
    }
    override func touchesBegan(with event: NSEvent) { app?.logEvent(event, source: "view.touchesBegan") }
    override func touchesMoved(with event: NSEvent) { app?.logEvent(event, source: "view.touchesMoved") }
    override func touchesEnded(with event: NSEvent) { app?.logEvent(event, source: "view.touchesEnded") }
    override func touchesCancelled(with event: NSEvent) { app?.logEvent(event, source: "view.touchesCancelled") }
    override func mouseDown(with event: NSEvent) { app?.logEvent(event, source: "view.mouseDown") }
}

// MARK: - app
final class AppDelegate: NSObject, NSApplicationDelegate {
    var window: NSWindow!
    var duration: Double = 300
    var monitor: Any?
    var counts: [UInt: Int] = [:]
    var directTouchCount = 0
    var directTouchesInGesture = 0
    var indirectTouchesInGesture = 0
    var touchBeganInView = 0
    var lastSig = ""
    var startedAt = Date()
    var timer: Timer?

    func applicationDidFinishLaunching(_ n: Notification) {
        for s in NSScreen.screens {
            log("SYS", "screen name=\(s.localizedName.isEmpty ? "<空>" : s.localizedName) uuid=\(screenUUID(s)) touchCapabilities=\(touchCap(s)) frame=\(s.frame)")
        }
        let screens = NSScreen.screens
        let target = screens.first { screenUUID($0).caseInsensitiveCompare(HOUSI_UUID) == .orderedSame } ?? screens.first!
        log("SYS", "target screen uuid=\(screenUUID(target)) frame=\(target.frame)")

        let rect = target.frame
        window = ProbeWindow(contentRect: rect, styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.setFrame(rect, display: true)
        window.level = .normal
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]
        window.isOpaque = true
        window.backgroundColor = .black
        window.ignoresMouseEvents = false
        let tv = TouchView(frame: NSRect(origin: .zero, size: rect.size))
        tv.app = self
        window.contentView = tv
        window.makeKeyAndOrderFront(nil)
        window.orderFrontRegardless()

        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)

        // local monitor sees every delivered event to this app
        monitor = NSEvent.addLocalMonitorForEvents(matching: .any) { [weak self] e in
            self?.logEvent(e, source: "monitor")
            return e
        }
        log("SYS", "monitors installed; isActive=\(NSApp.isActive) keyWindow=\(window.isKeyWindow)")

        for delay in [0.4, 1.2, 2.5] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
                NSApp.activate(ignoringOtherApps: true)
                self?.window.makeKeyAndOrderFront(nil)
                log("SYS", "activation retry +\(delay)s isActive=\(NSApp.isActive) keyWindow=\(self?.window.isKeyWindow ?? false)")
            }
        }
        let statusTimer = Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { [weak self] _ in
            log("STATUS", "isActive=\(NSApp.isActive) keyWindow=\(self?.window.isKeyWindow ?? false) mainWindow=\(NSApp.mainWindow?.windowNumber ?? -1)")
        }
        RunLoop.main.add(statusTimer, forMode: .common)
        timer = Timer.scheduledTimer(withTimeInterval: duration, repeats: false) { [weak self] _ in
            self?.finish()
        }
        RunLoop.main.add(timer!, forMode: .common)
        startedAt = Date()
    }

    func logEvent(_ e: NSEvent, source: String) {
        let ts = e.timestamp
        let sig = "\(source)|\(e.type.rawValue)|\(ts)|\(e.eventNumber)"
        if sig == lastSig { return }
        lastSig = sig
        counts[UInt(e.type.rawValue), default: 0] += 1
        var ts2: [NSTouch] = []
        if e.type == .gesture || e.type == .directTouch { ts2 = touchesOf(e) }
        var directCount = 0, indirectCount = 0
        for t in ts2 {
            if #available(macOS 10.12.2, *) {
                if t.type == .direct { directCount += 1 } else { indirectCount += 1 }
            }
        }
        if e.type == .directTouch { directTouchCount += 1 }
        if e.type == .gesture {
            directTouchesInGesture += directCount
            indirectTouchesInGesture += indirectCount
        }
        if source.hasPrefix("view.") && e.type == .gesture && directCount > 0 { touchBeganInView += 1 }

        var extra = ""
        if let w = e.window {
            extra += " window=\(w.windowNumber) winScreenUUID=\(screenUUID(w.screen))"
            let p = w.convertPoint(toScreen: e.locationInWindow)
            extra += " screenPt=(\(String(format: "%.1f", p.x)),\(String(format: "%.1f", p.y)))"
        }
        log("EVT", "appActive=\(NSApp.isActive) src=\(source) type=\(e.type.rawValue) subtype=\(e.subtype.rawValue) number=\(e.eventNumber) phase=\(e.phase.rawValue) locInWin=(\(String(format: "%.1f", e.locationInWindow.x)),\(String(format: "%.1f", e.locationInWindow.y))) touches=\(ts2.count) direct=\(directCount) indirect=\(indirectCount)\(extra)")
        for (i, t) in ts2.enumerated() { log("TOUCH", describeTouch(t, index: i)) }
    }

    func finish() {
        log("SYS", "=== SUMMARY ===")
        log("SYS", "duration=\(String(format: "%.1f", Date().timeIntervalSince(startedAt)))s directTouchEvents=\(directTouchCount) gestureDirectTouches=\(directTouchesInGesture) gestureIndirectTouches=\(indirectTouchesInGesture) viewDirectTouches=\(touchBeganInView)")
        log("SYS", "eventTypes: " + counts.sorted { $0.key < $1.key }.map { "type\($0.key)=\($0.value)" }.joined(separator: " "))
        if let m = monitor { NSEvent.removeMonitor(m); monitor = nil }
        logHandle?.synchronizeFile()
        exit(0)
    }

    func applicationWillTerminate(_ n: Notification) {
        if let m = monitor { NSEvent.removeMonitor(m); monitor = nil }
        logHandle?.synchronizeFile()
    }
}

// MARK: - main
var args = CommandLine.arguments.dropFirst().makeIterator()
var duration: Double = 300
var logfile = "/tmp/tsprobe_default.log"
while let a = args.next() {
    switch a {
    case "--duration": if let v = args.next(), let d = Double(v) { duration = d }
    case "--logfile": if let v = args.next() { logfile = v }
    default: break
    }
}
let dir = URL(fileURLWithPath: logfile).deletingLastPathComponent()
try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
FileManager.default.createFile(atPath: logfile, contents: nil)
logHandle = FileHandle(forWritingAtPath: logfile)
logHandle?.seekToEndOfFile()

let app = NSApplication.shared
let delegate = AppDelegate()
delegate.duration = duration
app.delegate = delegate
app.run()
