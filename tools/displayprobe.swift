import AppKit
import CoreGraphics
import Foundation

func uuidString(_ displayID: CGDirectDisplayID) -> String {
    guard let cf = CGDisplayCreateUUIDFromDisplayID(displayID) else { return "nil" }
    let uuid = cf.takeRetainedValue()
    let s = CFUUIDCreateString(nil, uuid) as String
    return s
}

print("=== NSScreen.screens ===")
for s in NSScreen.screens {
    let num = (s.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value ?? 0
    let uuid = uuidString(num)
    let frame = s.frame
    print("name=\(s.localizedName) id=\(num) uuid=\(uuid) touchCapabilities=\(s.touchCapabilities.rawValue) frame=\(frame) main=\(s == NSScreen.main)")
}

print("=== CGGetOnlineDisplayList ===")
var ids = [CGDirectDisplayID](repeating: 0, count: 16)
var count: UInt32 = 0
let err = CGGetOnlineDisplayList(16, &ids, &count)
print("err=\(err) count=\(count)")
for i in 0..<Int(count) {
    let id = ids[i]
    let b = CGDisplayBounds(id)
    print("id=\(id) uuid=\(uuidString(id)) main=\(CGDisplayIsMain(id)) builtin=\(CGDisplayIsBuiltin(id)) active=\(CGDisplayIsActive(id)) bounds=\(b)")
}
