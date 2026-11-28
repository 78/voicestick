import AppKit
import Darwin
import MotionInput
import simd

final class TennisApp: NSObject, NSApplicationDelegate, NSWindowDelegate {
    private let inputs = MotionPicker(roles: [.racket])
    private var roster: InputRoster?
    private var reader: DispatchSourceRead?
    private let hostPID = getppid()
    private var window: NSWindow!
    private let court = Court()
    private let source = NSTextField(labelWithString: "")
    private let score = NSTextField(labelWithString: "")
    private let cue = NSTextField(labelWithString: "")
    private let drillHint = NSTextField(wrappingLabelWithString: "")
    private var drill = RacketDrill()
    private let recenterButton = NSButton(title: "握姿回正 R", target: nil, action: nil)
    private let modeButton = NSButton(title: "开始比赛", target: nil, action: nil)
    private let title = NSTextField(labelWithString: "自由试拍")
    private let command = NSButton(title: "模拟挥拍", target: nil, action: nil)
    private var model = Rally()
    private var pose = RacketPose()
    private var detector = SwingDetector()
    private var timer: Timer?
    private var lastTick = ProcessInfo.processInfo.systemUptime
    private var inMatch = false
    private var swings = 0
    private var lastStrokeTime = -10.0
    private var sampleCount = 0
    private var meterStart = ProcessInfo.processInfo.systemUptime
    private var samplesPerSecond = 0
    private var nextReadout = 0.0
    private var motionLive = false
    private var waiting = false
    private var sampleClock = SampleClock()
    private var previousContact: (time: Double, local: SIMD3<Float>)?
    private var keyboardStroke: Stroke?
    private var matchLog: MatchLog?
    private var feedNumber = 0

    private func endLog(_ reason: String) {
        matchLog?.write("end", ["reason": reason, "returns": model.returns, "best": model.best])
        matchLog?.close()
        matchLog = nil
    }

    private func logFeed() {
        feedNumber += 1
        matchLog?.write("feed", ["ball": feedNumber, "nearX": model.nearX, "farX": model.farX])
    }

    func applicationWillTerminate(_ notification: Notification) { endLog("window_closed") }

    private var keyboardSide = 1.0

    func applicationDidFinishLaunching(_ note: Notification) {
        let menu = NSMenu()
        let appItem = NSMenuItem()
        menu.addItem(appItem)
        let actions = NSMenu()
        let recenter = actions.addItem(withTitle: "校正握姿", action: #selector(recenterRacket), keyEquivalent: "r")
        recenter.target = self
        recenter.keyEquivalentModifierMask = []
        actions.addItem(withTitle: "关闭球场", action: #selector(closeCourt), keyEquivalent: "w").target = self
        actions.addItem(withTitle: "退出网球", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = actions
        NSApp.mainMenu = menu

        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 900, height: 620),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        window.title = "Voice Stick · 休息球场"
        window.appearance = NSAppearance(named: .darkAqua)
        window.minSize = NSSize(width: 660, height: 470)
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.backgroundColor = NSColor(srgbRed: 0.047, green: 0.13, blue: 0.15, alpha: 1)
        let content = window.contentView!
        court.showPractice(true)
        title.font = .systemFont(ofSize: 23, weight: .semibold)
        title.textColor = .white
        source.font = .systemFont(ofSize: 12, weight: .medium)
        source.textColor = NSColor(srgbRed: 0.68, green: 0.82, blue: 0.79, alpha: 1)
        source.stringValue = "键盘练习 · 空格挥拍"
        score.font = .monospacedDigitSystemFont(ofSize: 15, weight: .semibold)
        score.textColor = .white
        cue.font = .systemFont(ofSize: 15, weight: .medium)
        cue.textColor = .white
        command.target = self
        command.action = #selector(handleCommand)
        command.bezelStyle = .rounded
        modeButton.target = self
        modeButton.action = #selector(toggleMatch)
        recenterButton.target = self
        recenterButton.action = #selector(recenterRacket)
        drillHint.font = .systemFont(ofSize: 16, weight: .semibold)
        drillHint.textColor = .white
        drillHint.alignment = .center
        drillHint.drawsBackground = true
        drillHint.backgroundColor = NSColor(srgbRed: 0.05, green: 0.15, blue: 0.18, alpha: 0.92)
        drillHint.isHidden = true
        let controls = NSStackView(views: [recenterButton, command, modeButton])
        controls.spacing = 8
        let labels = NSStackView(views: [title, source])
        labels.orientation = .vertical
        labels.alignment = .leading
        labels.spacing = 3
        inputs.isHidden = !CommandLine.arguments.contains("--motion-stdin")
        for view in [labels, inputs, score, court, drillHint, cue, controls] as [NSView] {
            view.translatesAutoresizingMaskIntoConstraints = false
            content.addSubview(view)
        }
        NSLayoutConstraint.activate([
            labels.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 22),
            labels.centerYAnchor.constraint(equalTo: content.topAnchor, constant: 37),
            inputs.trailingAnchor.constraint(equalTo: score.leadingAnchor, constant: -18),
            inputs.centerYAnchor.constraint(equalTo: labels.centerYAnchor),
            labels.trailingAnchor.constraint(lessThanOrEqualTo: inputs.leadingAnchor, constant: -8),
            score.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -22),
            score.centerYAnchor.constraint(equalTo: labels.centerYAnchor),
            drillHint.topAnchor.constraint(equalTo: court.topAnchor, constant: 12),
            drillHint.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 16),
            drillHint.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -16),
            court.topAnchor.constraint(equalTo: content.topAnchor, constant: 74),
            court.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            court.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            court.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -54),
            cue.leadingAnchor.constraint(equalTo: labels.leadingAnchor),
            cue.centerYAnchor.constraint(equalTo: court.bottomAnchor, constant: 27),
            controls.centerYAnchor.constraint(equalTo: cue.centerYAnchor),
            controls.trailingAnchor.constraint(equalTo: score.trailingAnchor),
            cue.trailingAnchor.constraint(lessThanOrEqualTo: controls.leadingAnchor, constant: -12)
        ])
        court.onSwing = { [weak self] in self?.clickSwing() }
        let launchScreen = NSScreen.screens.first { $0.frame.contains(NSEvent.mouseLocation) } ?? NSScreen.main
        if let launchScreen { window.setFrame(launchScreen.visibleFrame, display: false) }
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(court)
        NSApp.activate(ignoringOtherApps: true)
        if CommandLine.arguments.contains("--motion-stdin") {
            waiting = true
            source.stringValue = "正在连接菜单栏 App…"
            listen()
        }
        render(dt: 0)
        let timer = Timer(timeInterval: 1/60, repeats: true) { [weak self] _ in self?.tick() }
        self.timer = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    @objc private func recenterRacket() {
        pose = RacketPose()
        detector = SwingDetector()
        previousContact = nil
    }

    @objc private func toggleMatch() {
        guard !waiting else { return }
        if !inMatch, !motionLive, let prompt = roster?.swingPrompt, prompt.phase != .joined {
            send(InputRequest(roles: [.racket], assignments: [:]))
        }
        setMatch(!inMatch)
        window.makeFirstResponder(court)
    }

    private func setMatch(_ enabled: Bool) {
        endLog(enabled ? "restart" : "practice")
        inMatch = enabled
        drill = RacketDrill()
        drillHint.isHidden = true
        model = Rally()
        if enabled {
            model.start()
            do { matchLog = try MatchLog() }
            catch { NSLog("Could not open tennis match log: %@", error.localizedDescription) }
            feedNumber = 0
            let id = roster?.assignments[MotionRole.racket.rawValue]
            let name = roster?.devices.first { $0.id == id }?.name
            matchLog?.write("start", ["input": motionLive ? (name ?? "imu") : "keyboard",
                                      "format": 2, "racketScale": RacketFace.matchScale])
            logFeed()
        }
        previousContact = nil
        keyboardStroke = nil
        detector = SwingDetector()
        court.showPractice(!enabled)
        title.stringValue = enabled ? "休息球场" : "自由试拍"
        modeButton.title = enabled ? "返回试拍" : "开始比赛"
        nextReadout = 0
        render(dt: 0)
    }

    @objc private func handleCommand() {
        if inMatch && model.phase == .finished {
            setMatch(true)
        } else if !inMatch && motionLive {
            if drill.advance() { recenterRacket() }
            nextReadout = 0
            render(dt: 0)
            window.makeFirstResponder(court)
        } else { clickSwing() }
    }

    @objc private func closeCourt() { window.close() }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    @objc private func clickSwing() {
        if !inMatch && roster?.swingPrompt?.phase == .timedOut {
            send(InputRequest(roles: [.racket], swingToJoin: true))
            return
        }
        guard !waiting, !motionLive else { return }
        if inMatch && model.phase == .finished { setMatch(true); return }
        keyboardStroke = Stroke(aim: keyboardSide * 0.72, strength: 0.55)
        keyboardSide *= -1
        recordSwing()
        window.makeFirstResponder(court)
    }

    private func recordSwing() {
        lastStrokeTime = ProcessInfo.processInfo.systemUptime
        if !inMatch { swings += 1; nextReadout = 0 }
        else { matchLog?.write("swing", ["ball": feedNumber, "phase": String(describing: model.phase),
                                         "progress": model.progress, "input": motionLive ? "imu" : "keyboard"]) }
    }

    private func racketFace(at now: Double) -> RacketFace {
        var orientation = pose.orientation
        if !motionLive, let keyboardStroke {
            let t = min(1, (now - lastStrokeTime) / 0.32)
            let side: Double = keyboardStroke.aim < 0 ? -1 : 1
            orientation = simd_quatf(angle: Float(sin(t * .pi) * side * 1.25), axis: SIMD3(0, 0, 1))
        }
        return RacketFace(pose: orientation, lane: model.receivingX, practice: !inMatch)
    }

    private func checkContact(_ stroke: Stroke?, face: RacketFace, at time: Double, delay: Double) {
        guard inMatch, model.phase == .incoming else { previousContact = nil; return }
        let local = model.ballRelative(to: face, delay: delay)
        let preceding = previousContact.flatMap { (0...0.055).contains(time - $0.time) ? $0.local : nil }
        previousContact = (time, local)
        if let stroke, model.tryReturn(stroke, face: face, from: preceding, delay: delay) {
            previousContact = nil
            matchLog?.write("hit", ["ball": feedNumber, "local": [local.x, local.y, local.z],
                                    "delayMs": delay * 1000, "returns": model.returns])
            if motionLive { detector.consume() }
        }
    }

    private func listen() {
        inputs.onRequest = { [weak self] request in
            self?.send(request)
        }
        inputs.onHint = { [weak self] hint in self?.source.stringValue = hint }
        let reader = DispatchSource.makeReadSource(fileDescriptor: STDIN_FILENO, queue: .main)
        reader.setEventHandler { [weak self] in
            while let packet = MotionWire.receive(on: STDIN_FILENO) { self?.receive(packet) }
        }
        self.reader = reader
        reader.resume()
        send(InputRequest(roles: [.racket], swingToJoin: true))
    }

    private func send(_ request: InputRequest) {
        if let data = try? JSONEncoder().encode(request), !MotionWire.send(data, on: STDIN_FILENO) {
            source.stringValue = "输入设备选择未送达，请重试。"
        }
    }

    private func receive(_ packet: Data) {
        if let (id, frame) = MotionWire.decodeSample(packet) {
            guard roster?.assignments[MotionRole.racket.rawValue] == id, let sample = IMUSample(frame) else { return }
            let now = ProcessInfo.processInfo.systemUptime
            guard let age = sampleClock.age(of: sample.time, receivedAt: now) else { return }
            sampleCount += 1
            pose.update(sample)
            let stroke = detector.sample(sample)
            drill.observe(pose: pose.orientation, sample: sample, stroke: stroke != nil)
            let delay = max(0, lastTick - (now - age))
            let face = racketFace(at: now)
            if let matchLog {
                let local = model.phase == .incoming ? model.ballRelative(to: face, delay: delay) : face.localBall(model.ball)
                let g = sample.rotationRate, a = sample.acceleration, q = face.rotation.vector
                matchLog.write("imu", ["ball": feedNumber, "deviceMs": sample.time,
                    "sequence": UInt16(frame[2]) | UInt16(frame[3]) << 8, "ageMs": age * 1000,
                    "phase": String(describing: model.phase), "progress": model.progress,
                    "gyro": [g.x,g.y,g.z], "accel": [a.x,a.y,a.z], "faceRotation": [q.x,q.y,q.z,q.w],
                    "local": [local.x,local.y,local.z], "active": detector.active != nil,
                    "recognized": stroke != nil, "fresh": age <= 0.10])
            }
            if age <= 0.10 {
                if stroke != nil { recordSwing() }
                checkContact(detector.active, face: face, at: now - age, delay: delay)
            } else { previousContact = nil }
        } else if packet.count == 18, packet[0] == 1, packet[1] == 0x13,
                  let id = MotionWire.sourceID(packet, offset: 2) {
            inputs.identify(id)
        } else if let next = try? JSONDecoder().decode(InputRoster.self, from: packet), next != roster {
            let key = MotionRole.racket.rawValue
            let id = next.assignments[key]
            let live = id.map(next.streaming.contains) == true
            if id != roster?.assignments[key] || live != motionLive {
                pose = RacketPose()
                detector = SwingDetector()
                sampleClock = SampleClock()
                previousContact = nil
                keyboardStroke = nil
                drill = RacketDrill()
                swings = 0
                sampleCount = 0
                samplesPerSecond = 0
                meterStart = ProcessInfo.processInfo.systemUptime
                if inMatch && motionLive { setMatch(false) }
            }
            roster = next
            inputs.update(next)
            motionLive = live
            waiting = false
            if let id {
                let name = next.devices.first { $0.id == id }?.name
                if name == nil { source.stringValue = "球拍已断开 · 可重选设备" }
                else if live { source.stringValue = "已连接 " + (name ?? "") + " · R 校正握姿" }
                else if next.unavailable.contains(id) { source.stringValue = "体感未响应 · 请选择设备重试" }
                else if next.busy { source.stringValue = "等待语音或固件操作完成" }
                else { source.stringValue = "正在开启体感…"; waiting = true }
            } else if next.devices.isEmpty {
                source.stringValue = "键盘练习 · 等待支持六轴的设备"
            } else {
                source.stringValue = "选择球拍设备，或按空格练习"
            }
            if let prompt = next.swingPrompt {
                switch prompt.phase {
                case .listening, .ambiguous:
                    source.stringValue = "正在识别" + prompt.role.title
                case .waiting:
                    if next.busy { source.stringValue = "等语音或固件操作完成后开始识别" }
                    else if !next.devices.isEmpty { source.stringValue = "更新固件可挥拍识别，也可手动选择球拍" }
                case .timedOut:
                    source.stringValue = "挥拍识别已结束"
                case .joined: break
                }
            }
            render(dt: 0)
        }
    }

    private func tick() {
        if reader != nil && getppid() != hostPID { NSApp.terminate(nil); return }
        let now = ProcessInfo.processInfo.systemUptime
        let dt = min(0.1, now-lastTick)
        lastTick = now
        if window.isMiniaturized { return }
        if inMatch {
            let prior = model.phase
            model.tick(dt)
            if model.phase != prior {
                switch model.phase {
                case .incoming: logFeed()
                case .missed: matchLog?.write("miss", ["ball": feedNumber])
                case .finished: endLog("time")
                default: break
                }
            }
            if !motionLive {
                let moving = now - lastStrokeTime < 0.30 ? keyboardStroke : nil
                checkContact(moving, face: racketFace(at: now), at: now, delay: 0)
            }
        }
        render(dt: dt)
    }

    private func render(dt: Double) {
        let now = ProcessInfo.processInfo.systemUptime
        let face = racketFace(at: now)
        court.draw(model, face: face, practice: !inMatch)
        if now - meterStart >= 1 {
            samplesPerSecond = Int((Double(sampleCount) / (now - meterStart)).rounded())
            sampleCount = 0
            meterStart = now
        }
        modeButton.isEnabled = !waiting
        recenterButton.isEnabled = motionLive
        command.isEnabled = !waiting
        command.isHidden = inMatch && motionLive && model.phase != .finished
        if !inMatch {
            if now < nextReadout && dt > 0 { return }
            nextReadout = now + 0.1
            let normal = pose.orientation.act(SIMD3(0, 0, 1))
            let turn = Int((atan2(normal.x, normal.z) * 180 / .pi).rounded())
            score.stringValue = motionLive ? "拍面 \(turn)°    挥拍 \(swings)    \(samplesPerSecond) Hz" : "挥拍 \(swings)"
            command.title = "模拟挥拍"
            drillHint.isHidden = drill.phase == .idle
            drillHint.stringValue = drill.prompt
            if motionLive { command.title = drill.button }
            if motionLive {
                cue.stringValue = now - lastStrokeTime < 0.65 ? "识别到挥拍" : "慢转手腕看拍面；绿色标记在正面。"
            } else {
                switch roster?.swingPrompt?.phase {
                case .listening: cue.stringValue = "拿稳后轻挥一次，绑定球拍。"
                case .ambiguous: cue.stringValue = "多台设备在动，请只挥动一台。"
                case .timedOut: command.title = "重新识别"; cue.stringValue = "还没选定球拍，可重新挥动识别。"
                default: cue.stringValue = waiting ? "正在等待体感数据…" : "选择球拍或用空格模拟挥拍。"
                }
            }
            return
        }
        let showResult = model.phase == .missed || (model.phase == .returning && model.progress < 0.32)
        drillHint.isHidden = !showResult
        if showResult { drillHint.stringValue = model.phase == .missed ? "漏球 · 对面重新发球" : "回球成功" }
        let seconds = Int(ceil(model.remaining))
        let tally = String(format: "连击 %d    最佳 %d     %d:%02d", model.streak, model.best, seconds/60, seconds%60)
        if score.stringValue != tally { score.stringValue = tally }
        switch model.phase {
        case .ready:
            command.title = "发球"
            cue.stringValue = "轻挥一下开球。"
        case .finished:
            command.title = "再来一局"
            cue.stringValue = "时间到！共回球 \(model.returns) 次，最高连击 \(model.best)。"
        case .missed:
            command.title = "挥拍"
            cue.stringValue = "漏球 · 对面即将重新发球。"
        case .returning:
            command.title = "挥拍"
            cue.stringValue = "回球成功！准备下一拍。"
        case .incoming:
            command.title = "挥拍"
            if model.approaching(face) {
                cue.stringValue = "现在挥拍！"
            } else if model.ball.center.z > face.center.z + 0.45 { cue.stringValue = "球已越过球拍。" }
            else { cue.stringValue = "对面来球 · 看落点圈，等球弹起。" }
        }
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
let delegate = TennisApp()
app.delegate = delegate
app.run()
