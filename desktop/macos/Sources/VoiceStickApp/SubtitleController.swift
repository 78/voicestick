import AppKit

final class SubtitleController {
    private struct Lane {
        var text: String
        var color: OverlayThemeColor
        var generation: Int
    }

    private let window: NSPanel
    private let stack = NSStackView()
    private var lanes: [String: Lane] = [:]
    private var generation = 0
    private let holdSeconds: TimeInterval = 7
    private let textFont = NSFont.systemFont(ofSize: 46, weight: .semibold)
    private let minLaneWidth: CGFloat = 520
    private let maxLaneWidth: CGFloat = 1400
    private let laneChromeWidth: CGFloat = 148
    private let laneVerticalPadding: CGFloat = 26
    private let minLaneHeight: CGFloat = 76
    private let maxWindowHeightRatio: CGFloat = 0.36

    init() {
        window = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 960, height: 180),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: false
        )
        window.isOpaque = false
        window.backgroundColor = .clear
        window.hasShadow = false
        window.ignoresMouseEvents = true
        window.level = .screenSaver
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient]
        window.animationBehavior = .utilityWindow
        window.alphaValue = 0

        stack.orientation = .vertical
        stack.alignment = .centerX
        stack.distribution = .gravityAreas
        stack.spacing = 10
        stack.translatesAutoresizingMaskIntoConstraints = false

        let contentView = NSView()
        contentView.wantsLayer = true
        contentView.addSubview(stack)
        window.contentView = contentView
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: contentView.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: contentView.trailingAnchor),
            stack.topAnchor.constraint(equalTo: contentView.topAnchor),
            stack.bottomAnchor.constraint(equalTo: contentView.bottomAnchor)
        ])
    }

    func show(text: String, deviceID: String, color: OverlayThemeColor) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        DispatchQueue.main.async {
            self.generation += 1
            let generation = self.generation
            self.lanes[AppConfig.normalizedDeviceID(deviceID)] = Lane(
                text: trimmed,
                color: color,
                generation: generation
            )
            self.render()
            DispatchQueue.main.asyncAfter(deadline: .now() + self.holdSeconds) { [weak self] in
                self?.hideLane(deviceID: deviceID, generation: generation)
            }
        }
    }

    func hideAll() {
        DispatchQueue.main.async {
            self.lanes.removeAll()
            self.render()
        }
    }

    private func hideLane(deviceID: String, generation: Int) {
        let key = AppConfig.normalizedDeviceID(deviceID)
        guard lanes[key]?.generation == generation else { return }
        lanes.removeValue(forKey: key)
        render()
    }

    private func render() {
        stack.arrangedSubviews.forEach {
            stack.removeArrangedSubview($0)
            $0.removeFromSuperview()
        }

        for key in lanes.keys.sorted() {
            guard let lane = lanes[key] else { continue }
            stack.addArrangedSubview(makeLaneView(deviceID: key, lane: lane))
        }

        guard !lanes.isEmpty else {
            hideWindow()
            return
        }
        reposition()
        if !window.isVisible {
            window.orderFrontRegardless()
        }
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.12
            window.animator().alphaValue = 1
        }
    }

    private func makeLaneView(deviceID: String, lane: Lane) -> NSView {
        let container = NSView()
        container.wantsLayer = true
        container.translatesAutoresizingMaskIntoConstraints = false
        container.layer?.backgroundColor = NSColor.black.withAlphaComponent(0.62).cgColor
        container.layer?.cornerRadius = 14
        container.layer?.cornerCurve = .continuous

        let colorBar = NSView()
        colorBar.wantsLayer = true
        colorBar.translatesAutoresizingMaskIntoConstraints = false
        colorBar.layer?.backgroundColor = Self.nsColor(for: lane.color).cgColor
        colorBar.layer?.cornerRadius = 3

        let deviceLabel = NSTextField(labelWithString: deviceID)
        deviceLabel.font = .monospacedSystemFont(ofSize: 14, weight: .semibold)
        deviceLabel.textColor = Self.nsColor(for: lane.color).withAlphaComponent(0.95)
        deviceLabel.alignment = .center
        deviceLabel.translatesAutoresizingMaskIntoConstraints = false
        deviceLabel.setContentHuggingPriority(.required, for: .horizontal)

        let laneWidth = laneWidth(for: lane.text)
        let textWidth = max(1, laneWidth - laneChromeWidth)
        let shouldWrap = measuredSingleLineWidth(lane.text) > textWidth

        let textLabel = NSTextField(labelWithString: lane.text)
        textLabel.font = textFont
        textLabel.textColor = .white
        textLabel.alignment = .center
        textLabel.maximumNumberOfLines = shouldWrap ? 0 : 1
        textLabel.lineBreakMode = shouldWrap ? .byWordWrapping : .byClipping
        textLabel.usesSingleLineMode = !shouldWrap
        textLabel.preferredMaxLayoutWidth = textWidth
        textLabel.translatesAutoresizingMaskIntoConstraints = false
        textLabel.shadow = {
            let shadow = NSShadow()
            shadow.shadowColor = NSColor.black.withAlphaComponent(0.8)
            shadow.shadowBlurRadius = 3
            shadow.shadowOffset = NSSize(width: 0, height: -1)
            return shadow
        }()

        container.addSubview(colorBar)
        container.addSubview(deviceLabel)
        container.addSubview(textLabel)

        NSLayoutConstraint.activate([
            colorBar.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: 18),
            colorBar.topAnchor.constraint(equalTo: container.topAnchor, constant: 14),
            colorBar.bottomAnchor.constraint(equalTo: container.bottomAnchor, constant: -14),
            colorBar.widthAnchor.constraint(equalToConstant: 6),

            deviceLabel.leadingAnchor.constraint(equalTo: colorBar.trailingAnchor, constant: 12),
            deviceLabel.centerYAnchor.constraint(equalTo: container.centerYAnchor),
            deviceLabel.widthAnchor.constraint(greaterThanOrEqualToConstant: 42),

            textLabel.leadingAnchor.constraint(equalTo: deviceLabel.trailingAnchor, constant: 14),
            textLabel.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -26),
            textLabel.topAnchor.constraint(equalTo: container.topAnchor, constant: 12),
            textLabel.bottomAnchor.constraint(equalTo: container.bottomAnchor, constant: -14),
            container.widthAnchor.constraint(equalToConstant: laneWidth),
            container.heightAnchor.constraint(equalToConstant: laneHeight(for: lane.text, width: laneWidth))
        ])

        return container
    }

    private func reposition() {
        guard let screen = NSScreen.main ?? NSScreen.screens.first else { return }
        let visibleFrame = screen.visibleFrame
        let width = currentWindowWidth
        let measuredHeight = lanes.values.reduce(CGFloat(0)) { total, lane in
            total + laneHeight(for: lane.text, width: width)
        } + CGFloat(max(0, lanes.count - 1)) * stack.spacing
        let height = min(measuredHeight, visibleFrame.height * maxWindowHeightRatio)
        let x = visibleFrame.midX - width / 2
        let y = visibleFrame.minY + max(18, visibleFrame.height * 0.035)
        window.setFrame(NSRect(x: x, y: y, width: width, height: height), display: true)
    }

    private func hideWindow() {
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.12
            window.animator().alphaValue = 0
        } completionHandler: {
            self.window.orderOut(nil)
        }
    }

    private var currentWindowWidth: CGFloat {
        max(lanes.values.map { laneWidth(for: $0.text) }.max() ?? minLaneWidth, minLaneWidth)
    }

    private func laneWidth(for text: String) -> CGFloat {
        guard let screen = NSScreen.main ?? NSScreen.screens.first else { return 960 }
        let maxWidth = min(maxLaneWidth, screen.visibleFrame.width * 0.86)
        let measured = measuredSingleLineWidth(text)
        return min(maxWidth, max(minLaneWidth, measured + laneChromeWidth))
    }

    private func laneHeight(for text: String, width: CGFloat) -> CGFloat {
        let textWidth = max(1, width - laneChromeWidth)
        let attributed = NSAttributedString(string: text.isEmpty ? " " : text, attributes: [.font: textFont])
        let rect = attributed.boundingRect(
            with: NSSize(width: textWidth, height: CGFloat.greatestFiniteMagnitude),
            options: [.usesLineFragmentOrigin, .usesFontLeading]
        )
        return max(minLaneHeight, ceil(rect.height) + laneVerticalPadding)
    }

    private func measuredSingleLineWidth(_ text: String) -> CGFloat {
        let attributed = NSAttributedString(string: text.isEmpty ? " " : text, attributes: [.font: textFont])
        let rect = attributed.boundingRect(
            with: NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude),
            options: [.usesLineFragmentOrigin, .usesFontLeading]
        )
        return ceil(rect.width)
    }

    private static func nsColor(for color: OverlayThemeColor) -> NSColor {
        switch color {
        case .white:
            return NSColor.white
        case .pink:
            return NSColor(calibratedRed: 1.0, green: 0.42, blue: 0.62, alpha: 1)
        case .green:
            return NSColor(calibratedRed: 0.31, green: 0.84, blue: 0.55, alpha: 1)
        case .yellow:
            return NSColor(calibratedRed: 1.0, green: 0.78, blue: 0.26, alpha: 1)
        case .blue:
            return NSColor(calibratedRed: 0.38, green: 0.68, blue: 1.0, alpha: 1)
        case .purple:
            return NSColor(calibratedRed: 0.72, green: 0.55, blue: 1.0, alpha: 1)
        }
    }
}
