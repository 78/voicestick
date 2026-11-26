import AppKit

final class OverlayController {
    private enum Phase {
        case preview, countdown, confirmation
    }

    private let window: EditableOverlayPanel
    private let surface = NSView()
    private let container = NSView()
    private let indicator = RecognitionIndicatorView()
    private let textLabel = NSTextField(labelWithString: "")
    private let hintLabel = NSTextField(labelWithString: "")
    private let outputModeLabel = NSTextField(labelWithString: "")
    private let draftScroll = NSScrollView()
    private let draftEditor = OverlayTextView(frame: .zero)
    private lazy var confirmationActions = ConfirmationActions(frame: .zero)
    private lazy var confirmationActionsHeight = confirmationActions.heightAnchor.constraint(
        equalToConstant: ConfirmationActions.height
    )
    private var bodyHeight: NSLayoutConstraint?
    private var phase = Phase.preview
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
    private let minHeight: CGFloat = 112
    private let textLineHeightMultiple: CGFloat = 1.1
    private var themeColor: OverlayThemeColor = .white
    private var position: OverlayPosition = .center
    private var stackIndex = 0

    init() {
        window = EditableOverlayPanel(
            contentRect: NSRect(x: 0, y: 0, width: 720, height: 132),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: false
        )
        window.isOpaque = false
        window.backgroundColor = .clear
        window.hasShadow = true
        window.ignoresMouseEvents = true
        window.becomesKeyOnlyIfNeeded = true
        window.hidesOnDeactivate = false
        window.appearance = NSAppearance(named: .aqua)
        window.level = .screenSaver
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient]
        window.animationBehavior = .utilityWindow
        window.contentView = surface
        window.alphaValue = 0

        surface.wantsLayer = true
        if let layer = surface.layer {
            layer.backgroundColor = VoiceOverlayStyle.backgroundColor(for: themeColor).cgColor
            layer.cornerRadius = VoiceOverlayStyle.cornerRadius
            layer.cornerCurve = .continuous
            layer.masksToBounds = true
            layer.borderWidth = 0
        }
        container.translatesAutoresizingMaskIntoConstraints = false
        surface.addSubview(container)
        let bodyHeight = container.heightAnchor.constraint(equalToConstant: 132)
        self.bodyHeight = bodyHeight

        textLabel.font = VoiceOverlayStyle.textFont
        textLabel.textColor = VoiceOverlayStyle.textColor
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

        outputModeLabel.font = .systemFont(ofSize: 10)
        outputModeLabel.textColor = NSColor.black.withAlphaComponent(0.36)
        outputModeLabel.alignment = .right
        outputModeLabel.lineBreakMode = .byTruncatingTail
        outputModeLabel.usesSingleLineMode = true
        outputModeLabel.translatesAutoresizingMaskIntoConstraints = false
        outputModeLabel.isHidden = true
        surface.addSubview(outputModeLabel)

        draftScroll.borderType = .noBorder
        draftScroll.drawsBackground = false
        draftScroll.hasVerticalScroller = true
        draftScroll.autohidesScrollers = true
        draftScroll.scrollerStyle = .overlay
        draftScroll.translatesAutoresizingMaskIntoConstraints = false
        draftScroll.isHidden = true
        draftEditor.isRichText = false
        draftEditor.importsGraphics = false
        draftEditor.allowsUndo = true
        draftEditor.isEditable = false
        draftEditor.isSelectable = false
        draftEditor.drawsBackground = false
        draftEditor.isVerticallyResizable = true
        draftEditor.autoresizingMask = [.width]
        draftEditor.textContainer?.widthTracksTextView = true
        draftEditor.textContainer?.lineFragmentPadding = 0
        draftEditor.setAccessibilityLabel("Recognized text")
        draftEditor.onChange = { [weak self] text in
            self?.confirmationActions.updateDraft(text)
        }
        draftScroll.documentView = draftEditor
        window.initialFirstResponder = draftEditor

        indicator.translatesAutoresizingMaskIntoConstraints = false
        container.addSubview(indicator)
        container.addSubview(textLabel)
        container.addSubview(hintLabel)
        container.addSubview(draftScroll)

        let textCenterYConstraint = textLabel.centerYAnchor.constraint(equalTo: container.centerYAnchor)
        self.textCenterYConstraint = textCenterYConstraint

        NSLayoutConstraint.activate([
            outputModeLabel.leadingAnchor.constraint(greaterThanOrEqualTo: surface.leadingAnchor, constant: 23),
            outputModeLabel.trailingAnchor.constraint(equalTo: surface.trailingAnchor, constant: -11),
            outputModeLabel.bottomAnchor.constraint(equalTo: surface.bottomAnchor, constant: -11),
            container.topAnchor.constraint(equalTo: surface.topAnchor),
            container.leadingAnchor.constraint(equalTo: surface.leadingAnchor),
            container.trailingAnchor.constraint(equalTo: surface.trailingAnchor),
            bodyHeight,
            indicator.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: horizontalPadding),
            indicator.centerYAnchor.constraint(equalTo: container.centerYAnchor),
            indicator.widthAnchor.constraint(equalToConstant: indicatorWidth),
            indicator.heightAnchor.constraint(equalToConstant: indicatorWidth),
            textLabel.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: textLeadingInset),
            textLabel.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -horizontalPadding),
            textCenterYConstraint,
            hintLabel.leadingAnchor.constraint(equalTo: textLabel.leadingAnchor),
            hintLabel.trailingAnchor.constraint(equalTo: textLabel.trailingAnchor),
            hintLabel.topAnchor.constraint(equalTo: textLabel.bottomAnchor, constant: 8),
            draftScroll.topAnchor.constraint(equalTo: container.topAnchor),
            draftScroll.bottomAnchor.constraint(equalTo: container.bottomAnchor),
            draftScroll.leadingAnchor.constraint(equalTo: textLabel.leadingAnchor),
            draftScroll.trailingAnchor.constraint(equalTo: textLabel.trailingAnchor),
        ])
    }

    func setThemeColor(_ color: OverlayThemeColor) {
        guard themeColor != color else { return }
        themeColor = color
        surface.layer?.backgroundColor = VoiceOverlayStyle.backgroundColor(for: color).cgColor
    }

    func setPosition(_ position: OverlayPosition) {
        guard self.position != position else { return }
        self.position = position
        if window.isVisible && phase != .confirmation {
            reposition(for: textLabel.stringValue)
        }
    }

    func setStackIndex(_ index: Int) {
        guard stackIndex != index else { return }
        stackIndex = index
        if window.isVisible && phase != .confirmation {
            reposition(for: textLabel.stringValue)
        }
    }

    func showListening(text: String, outputMode: (text: String, accessibilityLabel: String)?) {
        show(mode: .listening, text: text.isEmpty ? "Listening..." : text, outputMode: outputMode, autoHideAfter: nil)
    }

    func showFinal(text: String, duration: TimeInterval, onHidden: (() -> Void)? = nil) {
        show(
            mode: .countdown(duration: duration),
            text: text.isEmpty ? "No speech" : text,
            hint: "",
            autoHideAfter: duration,
            onHidden: onHidden
        )
    }

    func showConfirmation(
        text: String,
        shortcuts: ConfirmationShortcuts,
        onSend: @escaping (Bool) -> Void,
        onCancel: @escaping () -> Void
    ) {
        hideWorkItem?.cancel()
        hideWorkItem = nil
        pendingHideCompletion = nil
        presentationGeneration += 1
        setOutputMode(nil)

        // Manual confirmation can arrive without an existing countdown.
        if phase != .countdown || !window.isVisible {
            phase = .countdown
            prepareDraft(text)
            reposition(for: text)
            indicator.setMode(.paused)
        }

        indicator.pauseCountdown()
        phase = .confirmation
        window.hasTakenKeyboardFocus = window.isKeyWindow
        let actions = confirmationActions
        if actions.superview == nil {
            actions.translatesAutoresizingMaskIntoConstraints = false
            surface.addSubview(actions)
            NSLayoutConstraint.activate([
                actions.topAnchor.constraint(equalTo: container.bottomAnchor),
                actions.leadingAnchor.constraint(equalTo: surface.leadingAnchor),
                actions.trailingAnchor.constraint(equalTo: surface.trailingAnchor),
                confirmationActionsHeight,
            ])
        }
        confirmationActionsHeight.constant = ConfirmationActions.height
        actions.onSend = onSend
        actions.onCancel = onCancel
        actions.reset(shortcuts: shortcuts)
        actions.updateDraft(draftEditor.string)
        actions.isHidden = false
        draftEditor.isSelectable = true
        draftEditor.isEditable = true
        window.ignoresMouseEvents = false
        expandWindowDownward(by: ConfirmationActions.height)
        window.alphaValue = 1
        window.orderFrontRegardless()
    }

    var confirmationDraft: String { draftEditor.string }
    var confirmationTookKeyboardFocus: Bool { window.hasTakenKeyboardFocus }

    func updateConfirmationShortcuts(_ shortcuts: ConfirmationShortcuts) {
        guard phase == .confirmation else { return }
        confirmationActions.updateShortcuts(shortcuts)
    }

    func takeDraftForSending() -> String? {
        guard phase == .confirmation else { return nil }
        draftEditor.unmarkText()
        let text = draftEditor.string
        guard !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return nil }
        window.orderOut(nil)
        return text
    }

    func showConfirmationSendFailure() {
        guard phase == .confirmation else { return }
        confirmationActions.showSendFailure()
        let extraHeight = ConfirmationActions.sendFailureHeight - confirmationActionsHeight.constant
        if extraHeight > 0 {
            confirmationActionsHeight.constant = ConfirmationActions.sendFailureHeight
            expandWindowDownward(by: extraHeight)
        }
        window.orderFrontRegardless()
    }

    private func expandWindowDownward(by height: CGFloat) {
        let frame = window.frame
        window.setFrame(
            NSRect(x: frame.minX, y: frame.minY - height, width: frame.width, height: frame.height + height),
            display: true
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
        window.ignoresMouseEvents = true
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.12
            window.animator().alphaValue = 0
        } completionHandler: {
            guard self.presentationGeneration == hideGeneration else { return }
            self.window.orderOut(nil)
            self.largestVisibleHeight = nil
            self.resetConfirmation()
            self.phase = .preview
            completion?()
        }
    }

    private func show(
        mode: RecognitionIndicatorView.Mode,
        text: String,
        hint: String = "",
        outputMode: (text: String, accessibilityLabel: String)? = nil,
        autoHideAfter delay: TimeInterval?,
        onHidden: (() -> Void)? = nil
    ) {
        let update = {
            self.hideWorkItem?.cancel()
            self.presentationGeneration += 1
            self.pendingHideCompletion = onHidden
            self.indicator.setMode(mode)
            self.resetConfirmation()
            if case .countdown = mode {
                self.phase = .countdown
                self.prepareDraft(text)
            } else {
                self.phase = .preview
            }
            self.applyText(text, shouldWrap: true)
            self.hintLabel.stringValue = hint
            self.hintLabel.isHidden = hint.isEmpty
            self.textCenterYConstraint?.constant = hint.isEmpty ? 0 : -10
            self.setOutputMode(outputMode)
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

    private func setOutputMode(_ mode: (text: String, accessibilityLabel: String)?) {
        outputModeLabel.stringValue = mode?.text ?? ""
        outputModeLabel.setAccessibilityLabel(mode?.accessibilityLabel)
        outputModeLabel.isHidden = mode == nil
    }

    private func prepareDraft(_ text: String) {
        draftScroll.isHidden = false
        textLabel.isHidden = true
        hintLabel.isHidden = true
        textCenterYConstraint?.constant = 0
        let attributes = textAttributes(font: VoiceOverlayStyle.textFont, shouldWrap: true)
        draftEditor.textStorage?.setAttributedString(NSAttributedString(string: text, attributes: attributes))
        draftEditor.typingAttributes = attributes
        draftEditor.undoManager?.removeAllActions()
        draftEditor.setSelectedRange(NSRange(location: 0, length: 0))
        draftScroll.contentView.scroll(to: .zero)
    }

    private func resetConfirmation() {
        window.ignoresMouseEvents = true
        if phase == .confirmation {
            confirmationActions.isHidden = true
            confirmationActions.onSend = nil
            confirmationActions.onCancel = nil
        }
        draftEditor.isEditable = false
        draftEditor.isSelectable = false
        draftScroll.isHidden = true
        textLabel.isHidden = false
    }

    private func reposition(for text: String) {
        guard let screen = NSScreen.main ?? NSScreen.screens.first else { return }
        let visibleFrame = screen.visibleFrame
        let availableMaxWidth = min(VoiceOverlayStyle.maxWidth, visibleFrame.width - 48)
        let measuredTextWidth = measuredSingleLineWidth(
            text: text,
            font: textLabel.font ?? VoiceOverlayStyle.textFont
        )
        let sideChromeWidth = textLeadingInset + horizontalPadding
        let maxTextWidth = availableMaxWidth - sideChromeWidth
        let desiredTextWidth = min(measuredTextWidth, maxTextWidth)
        let desiredWidth = desiredTextWidth + sideChromeWidth
        let width = min(max(minWidth, desiredWidth), availableMaxWidth)
        let textWidth = min(maxTextWidth, max(1, width - sideChromeWidth))
        let shouldWrap = measuredTextWidth > maxTextWidth || text.contains("\n")
        textLabel.maximumNumberOfLines = shouldWrap ? 0 : 1
        textLabel.lineBreakMode = shouldWrap ? .byWordWrapping : .byClipping
        textLabel.usesSingleLineMode = !shouldWrap
        applyText(text, shouldWrap: shouldWrap)
        textLabel.preferredMaxLayoutWidth = textWidth
        textLabel.invalidateIntrinsicContentSize()

        let font = textLabel.font ?? VoiceOverlayStyle.textFont
        let textHeight = shouldWrap
            ? measuredHeight(text: text, font: font, width: textWidth)
            : ceil(font.boundingRectForFont.height)
        let footerHeight = phase == .countdown ? ConfirmationActions.sendFailureHeight : 0
        let maxHeight = visibleFrame.height - max(120, 2 * VoiceOverlayStyle.screenMargin + footerHeight)
        let hintHeight = hintLabel.isHidden ? 0 : hintLabel.intrinsicContentSize.height + 8
        let desiredHeight = verticalPadding * 2 + max(textHeight + hintHeight, indicatorWidth)
        let measuredHeight = min(max(minHeight, desiredHeight), maxHeight)
        let height = min(maxHeight, max(measuredHeight, largestVisibleHeight ?? 0))
        largestVisibleHeight = height
        bodyHeight?.constant = height
        if phase == .countdown {
            draftEditor.textContainerInset = NSSize(width: 0, height: max(verticalPadding, (height - textHeight) / 2))
        }

        let windowSize = NSSize(width: width, height: height)
        let origin = VoiceOverlayStyle.frameOrigin(
            in: visibleFrame,
            windowSize: windowSize,
            position: position,
            stackIndex: stackIndex
        )
        let x = origin.x
        let y = max(origin.y, visibleFrame.minY + VoiceOverlayStyle.screenMargin + footerHeight)
        let frame = NSRect(origin: NSPoint(x: x, y: y), size: windowSize)
        window.setFrame(frame, display: true)
    }

    private var textLeadingInset: CGFloat {
        horizontalPadding + indicatorWidth + contentSpacing
    }

    private func measuredSingleLineWidth(text: String, font: NSFont) -> CGFloat {
        let attributed = NSAttributedString(string: text.isEmpty ? " " : text, attributes: [.font: font])
        return ceil(attributed.size().width)
    }

    private func applyText(_ text: String, shouldWrap: Bool) {
        let font = textLabel.font ?? VoiceOverlayStyle.textFont
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
            .foregroundColor: textLabel.textColor ?? VoiceOverlayStyle.textColor,
            .paragraphStyle: paragraphStyle
        ]
    }

    private func lineSpacing(for font: NSFont) -> CGFloat {
        let naturalLineHeight = ceil(font.boundingRectForFont.height)
        return max(0, ceil(naturalLineHeight * textLineHeightMultiple) - naturalLineHeight)
    }
}

private final class EditableOverlayPanel: NSPanel {
    var hasTakenKeyboardFocus = false

    override var canBecomeKey: Bool { !ignoresMouseEvents }

    override func becomeKey() {
        hasTakenKeyboardFocus = true
        super.becomeKey()
    }
}

private final class OverlayTextView: NSTextView {
    var onChange: ((String) -> Void)?

    override var needsPanelToBecomeKey: Bool { isEditable }

    override func didChangeText() {
        super.didChangeText()
        onChange?(string)
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
            showRing()
        case .error:
            showErrorCircle()
        }
    }

    func pauseCountdown() {
        let progress = ringLayer.presentation()?.strokeEnd ?? ringLayer.strokeEnd
        ringLayer.removeAnimation(forKey: "countdown")
        ringLayer.strokeEnd = progress
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

    private func showRing() {
        stopAnimating()
        for bar in barLayers {
            bar.isHidden = true
        }
        setXLayersHidden(true)

        ringLayer.removeAnimation(forKey: "countdown")
        ringLayer.isHidden = false
        ringLayer.strokeEnd = 1
    }

    private func showCountdown(duration: TimeInterval) {
        showRing()
        let animation = CABasicAnimation(keyPath: "strokeEnd")
        animation.fromValue = 1
        animation.toValue = 0
        animation.duration = duration
        animation.timingFunction = CAMediaTimingFunction(name: .linear)
        animation.fillMode = .forwards
        animation.isRemovedOnCompletion = false
        ringLayer.add(animation, forKey: "countdown")
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
