// The installer's icon, drawn here (no image file in the repository): a rounded square in forest greens with a plain white ocarina (a body,
// a mouthpiece, four finger holes) and two sound waves going out. Writes a 1024x1024 PNG; build_app.sh makes the .icns from it (sips, iconutil).
// usage: swift make_icon.swift <out.png>
import AppKit

let size: CGFloat = 1024
let img = NSImage(size: NSSize(width: size, height: size))
img.lockFocus()
let ctx = NSGraphicsContext.current!.cgContext
let inset: CGFloat = 100, rect = CGRect(x: inset, y: inset, width: size - 2 * inset, height: size - 2 * inset)
let shape = CGPath(roundedRect: rect, cornerWidth: 185, cornerHeight: 185, transform: nil)
ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -12), blur: 30, color: NSColor.black.withAlphaComponent(0.35).cgColor)
ctx.addPath(shape); ctx.setFillColor(NSColor.black.cgColor); ctx.fillPath()
ctx.restoreGState()
ctx.saveGState()
ctx.addPath(shape); ctx.clip()
let green = NSColor(calibratedRed: 0.05, green: 0.33, blue: 0.27, alpha: 1)
let colors = [NSColor(calibratedRed: 0.36, green: 0.72, blue: 0.38, alpha: 1).cgColor, green.cgColor] as CFArray
let grad = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: colors, locations: [0, 1])!
ctx.drawLinearGradient(grad, start: CGPoint(x: size / 2, y: rect.maxY), end: CGPoint(x: size / 2, y: rect.minY), options: [])
ctx.restoreGState()

// the ocarina, tilted up to the right
ctx.saveGState()
ctx.translateBy(x: 470, y: 440); ctx.rotate(by: .pi / 9)
ctx.setFillColor(NSColor.white.cgColor)
ctx.addPath(CGPath(roundedRect: CGRect(x: -330, y: -38, width: 150, height: 76), cornerWidth: 30, cornerHeight: 30, transform: nil)); ctx.fillPath()   // the mouthpiece
ctx.fillEllipse(in: CGRect(x: -250, y: -150, width: 500, height: 300))   // the body
ctx.setFillColor(green.cgColor)
for x in [-110.0, -10.0, 90.0] { ctx.fillEllipse(in: CGRect(x: CGFloat(x) - 26, y: 28, width: 52, height: 52)) }   // finger holes
ctx.fillEllipse(in: CGRect(x: 10 - 22, y: -78, width: 44, height: 44))
ctx.restoreGState()

// two sound waves going out to the upper right
ctx.setStrokeColor(NSColor.white.cgColor); ctx.setLineCap(.round)
let c = CGPoint(x: 600, y: 560)
for (r, a) in [(170.0, 0.95), (265.0, 0.65)] {
    ctx.setLineWidth(38); ctx.setAlpha(CGFloat(a))
    ctx.addArc(center: c, radius: CGFloat(r), startAngle: .pi * 0.08, endAngle: .pi * 0.42, clockwise: false); ctx.strokePath()
}
img.unlockFocus()
let rep = NSBitmapImageRep(data: img.tiffRepresentation!)!
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
