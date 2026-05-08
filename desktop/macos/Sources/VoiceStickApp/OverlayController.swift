import AppKit

final class OverlayController {
    private let window: NSPanel
    private let container = NSView()
    private let indicator = RecognitionIndicatorView()
    private let textLabel = NSTextField(labelWithString: "")
    private let hintLabel = NSTextField(labelWithString: "")
    private var textCenterYConstraint: NSLayoutConstraint?
    private var hideWorkItem: DispatchWorkItem?
    private var largestVisibleHeight: CGFloat?
    private var pendingHideCompletion: (() -> Void)?
    private var presentationGeneration = 0
    private let horizontalPadding: CGFloat = 32
    private let verticalPadding: CGFloat = 24
    private let indicatorWidth: CGFloat = 34
    private let contentSpacing: CGFloat = 16
    private let minWidth: CGFloat = 300
    private let maxWidth: CGFloat = 645
    private let minHeight: CGFloat = 112
    private let textLineHeightMultiple: CGFloat = 1.1
    private let positionMargin: CGFloat = 28
    private var themeColor: OverlayThemeColor = .white
    private var position: OverlayPosition = .center
    private var stackIndex = 0

    init() {
        window = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 720, height: 132),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: false
        )
        window.isOpaque = false
        window.backgroundColor = .clear
        window.hasShadow = true
        window.ignoresMouseEvents = true
        window.level = .screenSaver
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient]
        window.animationBehavior = .utilityWindow
        window.contentView = container
        window.alphaValue = 0

        container.wantsLayer = true
        if let layer = container.layer {
            layer.backgroundColor = Self.backgroundColor(for: themeColor).cgColor
            layer.cornerRadius = 24
            layer.cornerCurve = .continuous
            layer.masksToBounds = true
            layer.borderWidth = 0
        }

        textLabel.font = .systemFont(ofSize: 30, weight: .regular)
        textLabel.textColor = NSColor.black.withAlphaComponent(0.68)
        textLabel.alignment = .center
        textLabel.maximumNumberOfLines = 0
        textLabel.lineBreakMode = .byWordWrapping
        textLabel.usesSingleLineMode = false
        textLabel.translatesAutoresizingMaskIntoConstraints = false

        hintLabel.font = .systemFont(ofSize: 13, weight: .medium)
        hintLabel.textColor = NSColor.black.withAlphaComponent(0.42)
        hintLabel.alignment = .center
        hintLabel.lineBreakMode = .byClipping
        hintLabel.translatesAutoresizingMaskIntoConstraints = false

        indicator.translatesAutoresizingMaskIntoConstraints = false
        container.addSubview(indicator)
        container.addSubview(textLabel)
        container.addSubview(hintLabel)

        let textCenterYConstraint = textLabel.centerYAnchor.constraint(equalTo: container.centerYAnchor)
        self.textCenterYConstraint = textCenterYConstraint

        NSLayoutConstraint.activate([
            indicator.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: horizontalPadding),
            indicator.centerYAnchor.constraint(equalTo: container.centerYAnchor),
            indicator.widthAnchor.constraint(equalToConstant: indicatorWidth),
            indicator.heightAnchor.constraint(equalToConstant: indicatorWidth),
            textLabel.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: textLeadingInset),
            textLabel.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -horizontalPadding),
            textCenterYConstraint,
            hintLabel.leadingAnchor.constraint(equalTo: textLabel.leadingAnchor),
            hintLabel.trailingAnchor.constraint(equalTo: textLabel.trailingAnchor),
            hintLabel.topAnchor.constraint(equalTo: textLabel.bottomAnchor, constant: 8)
        ])
    }

    func setThemeColor(_ color: OverlayThemeColor) {
        guard themeColor != color else { return }
        themeColor = color
        container.layer?.backgroundColor = Self.backgroundColor(for: color).cgColor
    }

    func setPosition(_ position: OverlayPosition) {
        guard self.position != position else { return }
        self.position = position
        if window.isVisible {
            reposition(for: textLabel.stringValue)
        }
    }

    func setStackIndex(_ index: Int) {
        guard stackIndex != index else { return }
        stackIndex = index
        if window.isVisible {
            reposition(for: textLabel.stringValue)
        }
    }

    func showListening(text: String) {
        show(mode: .listening, text: text.isEmpty ? "Listening..." : text, hint: "", autoHideAfter: nil)
    }

    func showFinal(text: String, onHidden: (() -> Void)? = nil) {
        show(
            mode: .countdown(duration: 1.2),
            text: text.isEmpty ? "No speech" : text,
            hint: "",
            autoHideAfter: 1.2,
            onHidden: onHidden
        )
    }

    func showPaused(text: String) {
        show(
            mode: .paused,
            text: text.isEmpty ? "No speech" : text,
            hint: "Front: Send    Side: Cancel",
            autoHideAfter: nil
        )
    }

    func showError(_ text: String, onHidden: (() -> Void)? = nil) {
        show(
            mode: .error,
            text: text.isEmpty ? "Unknown ASR error" : text,
            hint: "ASR Error",
            autoHideAfter: 2,
            onHidden: onHidden
        )
    }

    func showStatus(_ text: String) {
        show(mode: .listening, text: text, hint: "", autoHideAfter: nil)
    }

    func hide(onHidden: (() -> Void)? = nil) {
        hideWorkItem?.cancel()
        hideWorkItem = nil
        let completion = onHidden ?? pendingHideCompletion
        pendingHideCompletion = nil
        presentationGeneration += 1
        let hideGeneration = presentationGeneration
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.12
            window.animator().alphaValue = 0
        } completionHandler: {
            guard self.presentationGeneration == hideGeneration else { return }
            self.window.orderOut(nil)
            self.largestVisibleHeight = nil
            completion?()
        }
    }

    private func show(
        mode: RecognitionIndicatorView.Mode,
        text: String,
        hint: String = "",
        autoHideAfter delay: TimeInterval?,
        onHidden: (() -> Void)? = nil
    ) {
        let update = {
            self.hideWorkItem?.cancel()
            self.presentationGeneration += 1
            self.pendingHideCompletion = onHidden
            self.indicator.setMode(mode)
            self.applyText(text, shouldWrap: true)
            self.hintLabel.stringValue = hint
            self.hintLabel.isHidden = hint.isEmpty
            self.textCenterYConstraint?.constant = hint.isEmpty ? 0 : -10
            self.reposition(for: text)

            if !self.window.isVisible {
                self.window.orderFrontRegardless()
            }
            NSAnimationContext.runAnimationGroup { context in
                context.duration = 0.12
                self.window.animator().alphaValue = 1
            }

            if let delay {
                let workItem = DispatchWorkItem { [weak self] in
                    self?.hide()
                }
                self.hideWorkItem = workItem
                DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: workItem)
            }
        }
        if Thread.isMainThread {
            update()
        } else {
            DispatchQueue.main.async(execute: update)
        }
    }

    private func reposition(for text: String) {
        guard let screen = NSScreen.main ?? NSScreen.screens.first else { return }
        let visibleFrame = screen.visibleFrame
        let availableMaxWidth = min(maxWidth, visibleFrame.width - 48)
        let measuredTextWidth = measuredSingleLineWidth(
            text: text,
            font: textLabel.font ?? .systemFont(ofSize: 30)
        )
        let sideChromeWidth = textLeadingInset + horizontalPadding
        let maxTextWidth = availableMaxWidth - sideChromeWidth
        let desiredTextWidth = min(measuredTextWidth, maxTextWidth)
        let desiredWidth = desiredTextWidth + sideChromeWidth
        let width = min(max(minWidth, desiredWidth), availableMaxWidth)
        let textWidth = min(maxTextWidth, max(1, width - sideChromeWidth))
        let shouldWrap = measuredTextWidth > maxTextWidth
        textLabel.maximumNumberOfLines = shouldWrap ? 0 : 1
        textLabel.lineBreakMode = shouldWrap ? .byWordWrapping : .byClipping
        textLabel.usesSingleLineMode = !shouldWrap
        applyText(text, shouldWrap: shouldWrap)
        textLabel.preferredMaxLayoutWidth = textWidth
        textLabel.invalidateIntrinsicContentSize()

        let font = textLabel.font ?? .systemFont(ofSize: 30)
        let textHeight = shouldWrap
            ? measuredHeight(text: text, font: font, width: textWidth)
            : ceil(font.boundingRectForFont.height)
        let maxHeight = visibleFrame.height - 120
        let hintHeight = hintLabel.stringValue.isEmpty ? 0 : hintLabel.intrinsicContentSize.height + 8
        let desiredHeight = verticalPadding * 2 + max(textHeight + hintHeight, indicatorWidth)
        let measuredHeight = min(max(minHeight, desiredHeight), maxHeight)
        let height = max(measuredHeight, largestVisibleHeight ?? 0)
        largestVisibleHeight = height

        let origin = frameOrigin(
            in: visibleFrame,
            windowSize: NSSize(width: width, height: height)
        )
        let x = origin.x
        let y = origin.y
        let frame = NSRect(x: x, y: y, width: width, height: height)
        window.setFrame(frame, display: true)
    }

    private func frameOrigin(in visibleFrame: NSRect, windowSize: NSSize) -> CGPoint {
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
                    min: visibleFrame.minY + positionMargin,
                    max: visibleFrame.maxY - positionMargin - windowSize.height
                )
            )
        case .topLeft:
            return CGPoint(
                x: visibleFrame.minX + positionMargin,
                y: visibleFrame.maxY - positionMargin - windowSize.height - CGFloat(stackIndex) * stackStep
            )
        case .topRight:
            return CGPoint(
                x: visibleFrame.maxX - positionMargin - windowSize.width,
                y: visibleFrame.maxY - positionMargin - windowSize.height - CGFloat(stackIndex) * stackStep
            )
        case .bottomLeft:
            return CGPoint(
                x: visibleFrame.minX + positionMargin,
                y: visibleFrame.minY + positionMargin + CGFloat(stackIndex) * stackStep
            )
        case .bottomRight:
            return CGPoint(
                x: visibleFrame.maxX - positionMargin - windowSize.width,
                y: visibleFrame.minY + positionMargin + CGFloat(stackIndex) * stackStep
            )
        }
    }

    private func stackDirection(for index: Int) -> Int {
        guard index > 0 else { return 0 }
        let magnitude = (index + 1) / 2
        return index % 2 == 1 ? -magnitude : magnitude
    }

    private func clamp(_ value: CGFloat, min minValue: CGFloat, max maxValue: CGFloat) -> CGFloat {
        Swift.max(minValue, Swift.min(maxValue, value))
    }

    private var textLeadingInset: CGFloat {
        horizontalPadding + indicatorWidth + contentSpacing
    }

    private func measuredSingleLineWidth(text: String, font: NSFont) -> CGFloat {
        let attributed = NSAttributedString(string: text.isEmpty ? " " : text, attributes: [.font: font])
        return ceil(attributed.size().width)
    }

    private func applyText(_ text: String, shouldWrap: Bool) {
        let font = textLabel.font ?? .systemFont(ofSize: 30)
        textLabel.attributedStringValue = NSAttributedString(
            string: text.isEmpty ? " " : text,
            attributes: textAttributes(font: font, shouldWrap: shouldWrap)
        )
    }

    private func measuredHeight(text: String, font: NSFont, width: CGFloat) -> CGFloat {
        let attributed = NSAttributedString(
            string: text.isEmpty ? " " : text,
            attributes: textAttributes(font: font, shouldWrap: true)
        )
        let rect = attributed.boundingRect(
            with: NSSize(width: width, height: .greatestFiniteMagnitude),
            options: [.usesLineFragmentOrigin, .usesFontLeading]
        )
        return ceil(rect.height)
    }

    private func textAttributes(font: NSFont, shouldWrap: Bool) -> [NSAttributedString.Key: Any] {
        let paragraphStyle = NSMutableParagraphStyle()
        paragraphStyle.lineBreakMode = shouldWrap ? .byWordWrapping : .byClipping
        paragraphStyle.alignment = .center
        paragraphStyle.lineSpacing = shouldWrap ? lineSpacing(for: font) : 0
        return [
            .font: font,
            .foregroundColor: textLabel.textColor ?? NSColor.black.withAlphaComponent(0.68),
            .paragraphStyle: paragraphStyle
        ]
    }

    private func lineSpacing(for font: NSFont) -> CGFloat {
        let naturalLineHeight = ceil(font.boundingRectForFont.height)
        return max(0, ceil(naturalLineHeight * textLineHeightMultiple) - naturalLineHeight)
    }

    private static func backgroundColor(for color: OverlayThemeColor) -> NSColor {
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

private final class RecognitionIndicatorView: NSView {
    enum Mode {
        case listening
        case countdown(duration: TimeInterval)
        case paused
        case error
    }

    private let barLayers = (0..<3).map { _ in CALayer() }
    private let ringLayer = CAShapeLayer()
    private let xLayers = (0..<2).map { _ in CAShapeLayer() }
    private var isAnimating = false

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        setup()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setup()
    }

    override func layout() {
        super.layout()
        layoutBars()
    }

    func setMode(_ mode: Mode) {
        switch mode {
        case .listening:
            showListeningBars()
        case .countdown(let duration):
            showCountdown(duration: duration)
        case .paused:
            showPausedRing()
        case .error:
            showErrorCircle()
        }
    }

    private func setup() {
        wantsLayer = true
        layer?.masksToBounds = false
        ringLayer.fillColor = NSColor.clear.cgColor
        ringLayer.strokeColor = NSColor.black.withAlphaComponent(0.34).cgColor
        ringLayer.lineCap = .round
        ringLayer.lineWidth = 3
        ringLayer.isHidden = true
        layer?.addSublayer(ringLayer)
        for xLayer in xLayers {
            xLayer.strokeColor = NSColor.black.withAlphaComponent(0.34).cgColor
            xLayer.lineCap = .round
            xLayer.lineWidth = 3
            xLayer.isHidden = true
            layer?.addSublayer(xLayer)
        }
        for bar in barLayers {
            bar.backgroundColor = NSColor.black.withAlphaComponent(0.34).cgColor
            bar.cornerRadius = 2
            layer?.addSublayer(bar)
        }
        layoutBars()
        startAnimating()
    }

    private func layoutBars() {
        let size = min(bounds.width, bounds.height)
        let ringBounds = CGRect(
            x: bounds.midX - size / 2,
            y: bounds.midY - size / 2,
            width: size,
            height: size
        )
        let ringInset: CGFloat = 5
        ringLayer.frame = bounds
        ringLayer.path = CGPath(
            ellipseIn: ringBounds.insetBy(dx: ringInset, dy: ringInset),
            transform: nil
        )
        for xLayer in xLayers {
            xLayer.frame = bounds
        }
        let xInset = size * 0.34
        let xRect = ringBounds.insetBy(dx: xInset, dy: xInset)
        let firstPath = CGMutablePath()
        firstPath.move(to: CGPoint(x: xRect.minX, y: xRect.minY))
        firstPath.addLine(to: CGPoint(x: xRect.maxX, y: xRect.maxY))
        xLayers[0].path = firstPath

        let secondPath = CGMutablePath()
        secondPath.move(to: CGPoint(x: xRect.minX, y: xRect.maxY))
        secondPath.addLine(to: CGPoint(x: xRect.maxX, y: xRect.minY))
        xLayers[1].path = secondPath

        let barWidth: CGFloat = 4
        let spacing: CGFloat = 4
        let heights: [CGFloat] = [12, 20, 15]
        let totalWidth = CGFloat(barLayers.count) * barWidth + CGFloat(barLayers.count - 1) * spacing
        let startX = bounds.midX - totalWidth / 2

        CATransaction.begin()
        CATransaction.setDisableActions(true)
        for (index, bar) in barLayers.enumerated() {
            let height = heights[index]
            let x = startX + CGFloat(index) * (barWidth + spacing)
            bar.frame = CGRect(x: x, y: bounds.midY - height / 2, width: barWidth, height: height)
            bar.anchorPoint = CGPoint(x: 0.5, y: 0.5)
        }
        CATransaction.commit()
    }

    private func showListeningBars() {
        ringLayer.removeAnimation(forKey: "countdown")
        ringLayer.isHidden = true
        setXLayersHidden(true)
        for bar in barLayers {
            bar.isHidden = false
        }
        startAnimating()
    }

    private func startAnimating() {
        guard !isAnimating else { return }
        isAnimating = true
        for (index, bar) in barLayers.enumerated() {
            bar.opacity = 1
            let animation = CABasicAnimation(keyPath: "transform.scale.y")
            animation.fromValue = 0.45
            animation.toValue = 1.15
            animation.duration = 0.52
            animation.autoreverses = true
            animation.repeatCount = .infinity
            animation.beginTime = CACurrentMediaTime() + Double(index) * 0.14
            animation.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
            bar.add(animation, forKey: "listening")
        }
    }

    private func showCountdown(duration: TimeInterval) {
        stopAnimating()
        for bar in barLayers {
            bar.isHidden = true
        }
        setXLayersHidden(true)

        ringLayer.isHidden = false
        ringLayer.strokeEnd = 1
        let animation = CABasicAnimation(keyPath: "strokeEnd")
        animation.fromValue = 1
        animation.toValue = 0
        animation.duration = duration
        animation.timingFunction = CAMediaTimingFunction(name: .linear)
        animation.fillMode = .forwards
        animation.isRemovedOnCompletion = false
        ringLayer.add(animation, forKey: "countdown")
    }

    private func showPausedRing() {
        stopAnimating()
        for bar in barLayers {
            bar.isHidden = true
        }
        ringLayer.removeAnimation(forKey: "countdown")
        ringLayer.isHidden = false
        ringLayer.strokeEnd = 1
        setXLayersHidden(true)
    }

    private func showErrorCircle() {
        stopAnimating()
        for bar in barLayers {
            bar.isHidden = true
        }
        ringLayer.removeAnimation(forKey: "countdown")
        ringLayer.isHidden = false
        ringLayer.strokeEnd = 1
        setXLayersHidden(false)
    }

    private func stopAnimating() {
        isAnimating = false
        for bar in barLayers {
            bar.removeAnimation(forKey: "listening")
            bar.opacity = 0.28
            bar.transform = CATransform3DIdentity
        }
    }

    private func setXLayersHidden(_ isHidden: Bool) {
        for xLayer in xLayers {
            xLayer.isHidden = isHidden
        }
    }
}
