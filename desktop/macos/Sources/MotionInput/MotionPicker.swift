import AppKit

/// Builds one input selector for each role requested by the game. For games
/// with several roles, submit the complete assignment in a single request.
public final class MotionPicker: NSStackView {
    private let roles: [MotionRole]
    private var menus: [MotionRole: NSPopUpButton] = [:]
    private var draft: [String: UUID] = [:]
    private var roster = InputRoster()
    private var listening: MotionRole?
    private var editing = false
    public var onRequest: ((InputRequest) -> Void)?
    public var onHint: ((String) -> Void)?

    public init(roles: [MotionRole]) {
        self.roles = roles
        super.init(frame: .zero)
        orientation = .horizontal
        for role in roles {
            let menu = NSPopUpButton()
            menu.target = self
            menu.action = #selector(choose(_:))
            menu.controlSize = .small
            menu.setAccessibilityLabel(role.title + "输入设备")
            menus[role] = menu
            addArrangedSubview(menu)
        }
        rebuild()
    }
    required init?(coder: NSCoder) { fatalError() }

    public func update(_ next: InputRoster) {
        let changed = roster.devices != next.devices || roster.assignments != next.assignments || roster.swingPrompt != next.swingPrompt
        roster = next
        if !editing { draft = next.assignments }
        if changed { rebuild() }
    }

    public func identify(_ device: UUID) {
        guard let role = listening, roster.devices.contains(where: { $0.id == device }) else { return }
        assign(device, to: role)
    }

    private func rebuild() {
        for role in roles {
            guard let menu = menus[role] else { continue }
            menu.removeAllItems()
            let id = draft[role.rawValue]
            let name = roster.devices.first { $0.id == id }?.name
            var label = name ?? (id == nil ? "选择设备" : "已断开")
            if roster.swingPrompt?.role == role && roster.swingPrompt?.phase == .listening { label = "请挥动" }
            if listening == role { label = "请按副键" }
            menu.addItem(withTitle: "\(role.title) · \(label)")
            menu.item(at: 0)?.isEnabled = false
            menu.menu?.addItem(.separator())
            for device in roster.devices {
                let item = NSMenuItem(title: device.name, action: nil, keyEquivalent: "")
                item.representedObject = device.id
                item.state = id == device.id ? .on : .off
                menu.menu?.addItem(item)
            }
            menu.menu?.addItem(.separator())
            menu.addItem(withTitle: "重新挥动识别")
            menu.lastItem?.tag = 3
            menu.addItem(withTitle: "按设备副键指定" + role.title)
            menu.lastItem?.tag = 1
            menu.addItem(withTitle: "释放全部设备 · 键盘练习")
            menu.lastItem?.tag = 2
            menu.selectItem(at: 0)
        }
    }

    @objc private func choose(_ sender: NSPopUpButton) {
        guard let role = roles.first(where: { menus[$0] === sender }), let item = sender.selectedItem else { return }
        if let id = item.representedObject as? UUID {
            assign(id, to: role)
        } else if item.tag == 1 {
            listening = role
            onHint?("请短按用作" + role.title + "的设备副键")
            rebuild()
        } else if item.tag == 3 {
            editing = false
            listening = nil
            onRequest?(InputRequest(roles: roles, swingToJoin: true))
        } else if item.tag == 2 {
            draft = [:]
            editing = false
            listening = nil
            onRequest?(InputRequest(roles: roles, assignments: [:]))
            rebuild()
        }
    }

    private func assign(_ id: UUID, to role: MotionRole) {
        draft = draft.filter { $0.value != id }
        draft[role.rawValue] = id
        listening = nil
        editing = draft.count < roles.count
        if editing, let missing = roles.first(where: { draft[$0.rawValue] == nil }) {
            onHint?("请选择" + missing.title + "的设备")
        } else {
            onRequest?(InputRequest(roles: roles, assignments: draft))
        }
        rebuild()
    }
}
