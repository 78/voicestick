import Foundation
import Network
import XCTest
@testable import VoiceStickApp

final class MotionWebBridgeTests: XCTestCase {
    func testLoopbackOriginHandshakesWithoutPrompting() throws {
        let httpPort = UInt16.random(in: 50000...59999)
        let wsPort = httpPort + 1
        let ble = BleCentral(pairedDeviceIDs: [])
        let bridge = MotionWebBridge(ble: ble, httpPort: httpPort, wsPort: wsPort)
        try bridge.start { false }
        defer { bridge.stop() }
        Thread.sleep(forTimeInterval: 0.15)

        let wsOptions = NWProtocolWebSocket.Options()
        wsOptions.setAdditionalHeaders([
            (name: "Origin", value: "http://127.0.0.1:\(httpPort)")
        ])
        let params = NWParameters.tcp
        params.defaultProtocolStack.applicationProtocols.insert(wsOptions, at: 0)
        let client = NWConnection(
            to: .url(URL(string: "ws://127.0.0.1:\(wsPort)/")!),
            using: params
        )
        defer { client.cancel() }

        let welcomed = expectation(description: "received welcome with loopback origin")
        var receivedOrigin: String?
        client.stateUpdateHandler = { state in
            if case .ready = state {
                client.receiveMessage { data, _, _, error in
                    XCTAssertNil(error)
                    if let data,
                       let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                       obj["type"] as? String == "welcome" {
                        receivedOrigin = obj["origin"] as? String
                        welcomed.fulfill()
                    }
                }
            }
        }
        client.start(queue: .global())

        wait(for: [welcomed], timeout: 5.0)
        XCTAssertEqual(receivedOrigin, "http://127.0.0.1:\(httpPort)")
    }
}
