import Foundation

final class SecondaryButtonClicks {
    private struct PendingClick {
        let releaseMs: UInt64
        let perform: () -> Void
    }

    private static let intervalMs = 180
    private var pendingClicks: [UUID: PendingClick] = [:]
    private var secondPresses: [UUID: PendingClick] = [:]

    func press(from peripheralID: UUID, at timestampMs: UInt64, canDoubleClick: Bool) {
        guard let first = pendingClicks.removeValue(forKey: peripheralID) else { return }
        if canDoubleClick, timestampMs >= first.releaseMs,
           timestampMs - first.releaseMs <= Self.intervalMs {
            secondPresses[peripheralID] = first
        } else {
            first.perform()
        }
    }

    func release(
        from peripheralID: UUID,
        at timestampMs: UInt64,
        heldFor durationMs: UInt64,
        canDoubleClick: @escaping () -> Bool,
        onSingleClick: @escaping () -> Void,
        onDoubleClick: () -> Void
    ) {
        if let first = secondPresses.removeValue(forKey: peripheralID) {
            if durationMs <= Self.intervalMs, canDoubleClick() {
                onDoubleClick()
                return
            }
            first.perform()
        }
        guard durationMs < 3_000 else { return }
        guard canDoubleClick() else {
            onSingleClick()
            return
        }

        pendingClicks[peripheralID] = PendingClick(releaseMs: timestampMs) {
            if canDoubleClick() { onSingleClick() }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(Self.intervalMs)) { [weak self] in
            guard let self, self.pendingClicks[peripheralID]?.releaseMs == timestampMs else { return }
            self.pendingClicks.removeValue(forKey: peripheralID)?.perform()
        }
    }

    func cancelAll() {
        pendingClicks.removeAll()
        secondPresses.removeAll()
    }

    func removeDisconnected(isConnected: (UUID) -> Bool) {
        pendingClicks = pendingClicks.filter { isConnected($0.key) }
        secondPresses = secondPresses.filter { isConnected($0.key) }
    }
}
