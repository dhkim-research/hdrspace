#!/usr/bin/swift
// Deterministic, code-native "fisheye" mark: a 180-degree sky frame with the sun
// above the horizon, on the charcoal tile. No generated image dependency.
// Keep in sync with assets/branding/hdrspace.svg and HdrspaceBrandMark (Hdrspace.cpp).
import AppKit
import CoreGraphics

let output = CommandLine.arguments[1]
let dimension = 1024
let colorSpace = CGColorSpaceCreateDeviceRGB()
let context = CGContext(data: nil, width: dimension, height: dimension,
                        bitsPerComponent: 8, bytesPerRow: dimension * 4,
                        space: colorSpace, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
func color(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> CGColor {
    CGColor(colorSpace: colorSpace, components: [r / 255, g / 255, b / 255, a])!
}
let background = color(27, 28, 32)
let border = color(53, 54, 59)
let amber = color(244, 191, 79) // UI accent
let tile = CGPath(roundedRect: CGRect(x: 64, y: 64, width: 896, height: 896),
                  cornerWidth: 196, cornerHeight: 196, transform: nil)
context.addPath(tile)
context.setFillColor(background)
context.fillPath()
context.addPath(tile)
context.setStrokeColor(border)
context.setLineWidth(3)
context.strokePath()

// CoreGraphics bitmap coordinates point up: "+y" is towards the top of the icon.
let center = CGPoint(x: 512, y: 512)
let ringRadius: CGFloat = 300
let ringWidth: CGFloat = 28
let inner = ringRadius - ringWidth / 2
let sun = CGPoint(x: center.x + 0.30 * inner, y: center.y + 0.36 * inner)
let frame = CGRect(x: center.x - inner, y: center.y - inner, width: 2 * inner, height: 2 * inner)

context.saveGState()
context.addEllipse(in: frame)
context.clip()
context.setFillColor(color(24, 22, 21))
context.fill(frame)
let glow = CGGradient(colorsSpace: colorSpace,
                      colors: [color(255, 214, 140, 0.95), color(214, 150, 70, 0.55),
                               color(110, 70, 32, 0.25), color(24, 22, 21, 0)] as CFArray,
                      locations: [0.0, 0.22, 0.55, 1.0])!
context.drawRadialGradient(glow, startCenter: sun, startRadius: 0, endCenter: sun,
                           endRadius: 1.15 * inner, options: [])
// The ground of a 180-degree frame bows slightly upwards.
let horizonY = center.y - 0.34 * inner
let ground = CGMutablePath()
ground.move(to: CGPoint(x: center.x - inner - 4, y: horizonY))
ground.addQuadCurve(to: CGPoint(x: center.x + inner + 4, y: horizonY),
                    control: CGPoint(x: center.x, y: center.y - 0.10 * inner))
ground.addLine(to: CGPoint(x: center.x + inner + 4, y: center.y - inner - 4))
ground.addLine(to: CGPoint(x: center.x - inner - 4, y: center.y - inner - 4))
ground.closeSubpath()
context.saveGState()
context.addPath(ground)
context.clip()
let soil = CGGradient(colorsSpace: colorSpace, colors: [color(30, 29, 30), color(16, 16, 18)] as CFArray,
                      locations: [0.0, 1.0])!
context.drawLinearGradient(soil, start: CGPoint(x: center.x, y: center.y - 0.10 * inner),
                           end: CGPoint(x: center.x, y: center.y - inner), options: [])
context.restoreGState()
let disc = CGGradient(colorsSpace: colorSpace,
                      colors: [color(255, 250, 236), color(255, 240, 205), color(255, 226, 160, 0)] as CFArray,
                      locations: [0.0, 0.62, 1.0])!
context.drawRadialGradient(disc, startCenter: sun, startRadius: 0, endCenter: sun,
                           endRadius: 0.17 * inner, options: [])
context.restoreGState()

context.addEllipse(in: CGRect(x: center.x - ringRadius, y: center.y - ringRadius,
                              width: 2 * ringRadius, height: 2 * ringRadius))
context.setStrokeColor(amber)
context.setLineWidth(ringWidth)
context.strokePath()

let bitmap = NSBitmapImageRep(cgImage: context.makeImage()!)
let data = bitmap.representation(using: .png, properties: [:])!
try data.write(to: URL(fileURLWithPath: output))
