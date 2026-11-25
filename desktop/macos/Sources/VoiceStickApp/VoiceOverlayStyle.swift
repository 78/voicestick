import AppKit

enum VoiceOverlayStyle {
    static let textFont = NSFont.systemFont(ofSize: 30)
    static let textColor = NSColor.black.withAlphaComponent(0.68)
    static let cornerRadius: CGFloat = 24
    static let maxWidth: CGFloat = 645
    static let screenMargin: CGFloat = 28

    static func frameOrigin(
        in visibleFrame: NSRect,
        windowSize: NSSize,
        position: OverlayPosition,
        stackIndex: Int = 0
    ) -> CGPoint {
        let gap: CGFloat = 14
        let stackStep = windowSize.height + gap
        let direction = stackDirection(for: stackIndex)
        let offset = CGFloat(abs(direction)) * stackStep * CGFloat(direction.signum())
        switch position {
        case .center:
            return CGPoint(
                x: visibleFrame.midX - windowSize.width / 2,
                y: clamp(
                    visibleFrame.midY - windowSize.height / 2 + offset,
                    min: visibleFrame.minY + screenMargin,
                    max: visibleFrame.maxY - screenMargin - windowSize.height
                )
            )
        case .topLeft:
            return CGPoint(
                x: visibleFrame.minX + screenMargin,
                y: visibleFrame.maxY - screenMargin - windowSize.height - CGFloat(stackIndex) * stackStep
            )
        case .topRight:
            return CGPoint(
                x: visibleFrame.maxX - screenMargin - windowSize.width,
                y: visibleFrame.maxY - screenMargin - windowSize.height - CGFloat(stackIndex) * stackStep
            )
        case .bottomLeft:
            return CGPoint(
                x: visibleFrame.minX + screenMargin,
                y: visibleFrame.minY + screenMargin + CGFloat(stackIndex) * stackStep
            )
        case .bottomRight:
            return CGPoint(
                x: visibleFrame.maxX - screenMargin - windowSize.width,
                y: visibleFrame.minY + screenMargin + CGFloat(stackIndex) * stackStep
            )
        }
    }

    private static func stackDirection(for index: Int) -> Int {
        guard index > 0 else { return 0 }
        let magnitude = (index + 1) / 2
        return index % 2 == 1 ? -magnitude : magnitude
    }

    private static func clamp(_ value: CGFloat, min minValue: CGFloat, max maxValue: CGFloat) -> CGFloat {
        Swift.max(minValue, Swift.min(maxValue, value))
    }

    static func backgroundColor(for color: OverlayThemeColor) -> NSColor {
        switch color {
        case .white:
            return NSColor.white.withAlphaComponent(0.86)
        case .pink:
            return NSColor(calibratedRed: 1.0, green: 0.84, blue: 0.9, alpha: 0.86)
        case .green:
            return NSColor(calibratedRed: 0.84, green: 0.95, blue: 0.84, alpha: 0.86)
        case .yellow:
            return NSColor(calibratedRed: 1.0, green: 0.94, blue: 0.72, alpha: 0.86)
        case .blue:
            return NSColor(calibratedRed: 0.82, green: 0.91, blue: 1.0, alpha: 0.86)
        case .purple:
            return NSColor(calibratedRed: 0.9, green: 0.84, blue: 1.0, alpha: 0.86)
        }
    }
}
