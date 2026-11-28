import AppKit
import SceneKit

private extension NSColor {
    convenience init(rgb: Int) {
        self.init(srgbRed: CGFloat(rgb >> 16 & 255)/255, green: CGFloat(rgb >> 8 & 255)/255,
                  blue: CGFloat(rgb & 255)/255, alpha: 1)
    }
}

final class Court: SCNView {
    private let world = SCNScene()
    private let ballNode = SCNNode()
    private let ballShadow = SCNNode()
    private let courtCamera = SCNNode()
    private let practiceCamera = SCNNode()
    private let racket = SCNNode()
    private let receiver = SCNNode()
    private let bounceMarker = SCNNode()
    private var playerRim: SCNMaterial?
    private var rimLit = false
    private let ballRadius = CGFloat(RacketFace.ballRadius)
    var onSwing: (() -> Void)?

    init() {
        super.init(frame: .zero, options: nil)
        scene = world
        antialiasingMode = .multisampling4X
        backgroundColor = NSColor(rgb: 0x142e37)
        world.background.contents = backgroundColor
        world.fogColor = backgroundColor
        world.fogStartDistance = 35
        world.fogEndDistance = 72
        let camera = courtCamera
        camera.camera = SCNCamera()
        camera.camera?.projectionDirection = .vertical
        camera.camera?.fieldOfView = 43
        camera.position = SCNVector3(0, 8.0, 16.6)
        camera.look(at: SCNVector3(0, 1.0, 4.75))
        world.rootNode.addChildNode(camera)
        practiceCamera.camera = SCNCamera()
        practiceCamera.camera?.projectionDirection = .vertical
        practiceCamera.camera?.fieldOfView = 50
        practiceCamera.position = SCNVector3(0, 1.95, 12.5)
        practiceCamera.look(at: SCNVector3(0, 1.95, 7.5))
        world.rootNode.addChildNode(practiceCamera)
        pointOfView = practiceCamera
        buildCourt()
        _ = buildRack(receiver, color: 0xf8ae75)
        playerRim = buildRack(racket, color: 0xe4fb92)
        receiver.simdScale = SIMD3(repeating: RacketFace.matchScale)
        ballNode.geometry = painted(SCNSphere(radius: ballRadius), 0xf1ff60)
        ballNode.geometry?.firstMaterial?.emission.contents = NSColor(rgb: 0x848f16)
        ballShadow.geometry = painted(SCNCylinder(radius: ballRadius, height: 0.012), 0x0d3538)
        bounceMarker.geometry = painted(SCNTorus(ringRadius: 0.51, pipeRadius: 0.048), 0x99fcf2)
        for n in [ballNode, ballShadow, racket, receiver, bounceMarker] { world.rootNode.addChildNode(n) }
        let light = SCNNode()
        light.light = SCNLight()
        light.light?.type = .directional
        light.light?.intensity = 1050
        light.eulerAngles = SCNVector3(-0.98, -0.45, 0)
        world.rootNode.addChildNode(light)
        let fill = SCNNode()
        fill.light = SCNLight()
        fill.light?.type = .ambient
        fill.light?.intensity = 610
        fill.light?.color = NSColor(rgb: 0xd7eded)
        world.rootNode.addChildNode(fill)
    }

    required init?(coder: NSCoder) { fatalError() }
    override func layout() {
        super.layout()
        guard bounds.width > 0, bounds.height > 0, let camera = courtCamera.camera else { return }
        // Keep the near sidelines visible when the player narrows the window.
        let verticalTangent = max(tan(43 * CGFloat.pi / 360), 0.57 * bounds.height / bounds.width)
        camera.fieldOfView = atan(verticalTangent) * 360 / .pi
        let practiceTangent = max(tan(50 * CGFloat.pi / 360), 0.41 * bounds.height / bounds.width)
        practiceCamera.camera?.fieldOfView = atan(practiceTangent) * 360 / .pi
    }

    override var acceptsFirstResponder: Bool { true }
    override func keyDown(with event: NSEvent) {
        if event.keyCode == 49 {
            if !event.isARepeat { onSwing?() }
        } else { super.keyDown(with: event) }
    }
    override func mouseDown(with event: NSEvent) { window?.makeFirstResponder(self) }

    private func painted(_ shape: SCNGeometry, _ rgb: Int) -> SCNGeometry {
        let mat = SCNMaterial()
        mat.diffuse.contents = NSColor(rgb: rgb)
        mat.lightingModel = .lambert
        shape.materials = [mat]
        return shape
    }

    @discardableResult
    private func box(_ x: CGFloat, _ y: CGFloat, _ z: CGFloat, _ w: CGFloat, _ h: CGFloat,
                     _ d: CGFloat, _ color: Int, in parent: SCNNode? = nil) -> SCNNode {
        let geo = SCNBox(width: w, height: h, length: d, chamferRadius: 0)
        let node = SCNNode(geometry: painted(geo, color))
        node.position = SCNVector3(x,y,z)
        (parent ?? world.rootNode).addChildNode(node)
        return node
    }

    private func buildCourt() {
        box(0,-0.16,0,100,0.20,90,0x163d40)
        box(0,-0.035,0,13,0.07,24,0x34877c)
        box(0,0.004,0,9.0,0.012,18,0x277e83)
        let chalk = 0xe0efdb
        for x: CGFloat in [-4.5, -3.5, 3.5, 4.5] { box(x,0.018,0,0.055,0.018,18, chalk) }
        for z: CGFloat in [-9, 9] { box(0,0.018,z,9.05,0.018,0.065,chalk) }
        for z: CGFloat in [-5.6, 5.6] { box(0,0.018,z,7,0.018,0.055,chalk) }
        box(0,0.018,0,0.055,0.018,11.2,chalk)
        box(0, 0.24, -12.6, 16, 0.48, 0.55, 0x234c4e)
        for side: CGFloat in [-1,1] {
            box(side*5,0.58,0,0.10,1.16,0.10,0xf3e8cd)
        }
        for row in 1...6 { box(0,CGFloat(row)*0.15,0,10,0.016,0.018,0x143b42) }
        for col in -25...25 { box(CGFloat(col)/5,0.5,0,0.013,0.95,0.018,0x143b42) }
        box(0,1.02,0,10.1,0.065,0.065,chalk)
    }

    private func buildRack(_ root: SCNNode, color: Int) -> SCNMaterial? {
        let head = SCNNode(geometry: painted(SCNTorus(ringRadius: CGFloat(RacketFace.ringRadius), pipeRadius: CGFloat(RacketFace.rim)), color))
        head.eulerAngles.x = .pi/2
        head.scale.z = CGFloat(RacketFace.stretch)
        root.addChildNode(head)
        for idx in -3...3 {
            let n = CGFloat(idx)*0.12
            let extent = sqrt(0.49*0.49-n*n)
            box(n,0,0,0.015,extent*2.56,0.018,0xdeebd7,in: root)
            box(0,n*1.28,0,extent*2,0.014,0.018,0xdeebd7,in: root)
        }
        box(0,-0.87,0,0.105,0.51,0.105,0xe0ba8d,in: root)
        // Two faces of a small string dampener make front/back visible during a twist.
        box(0,-0.34,0.025,0.28,0.105,0.026,0xa8ff65,in: root)
        box(0,-0.34,-0.025,0.28,0.105,0.026,0xff8257,in: root)
        return head.geometry?.firstMaterial
    }

    func showPractice(_ practice: Bool) {
        pointOfView = practice ? practiceCamera : courtCamera
        receiver.isHidden = practice
        bounceMarker.isHidden = true
        ballNode.isHidden = practice
        ballShadow.isHidden = practice
    }

    func draw(_ game: Rally, face: RacketFace, practice: Bool) {
        let pos = game.ball
        let visible = !practice && game.phase != .missed && game.phase != .finished
        ballNode.isHidden = !visible
        ballShadow.isHidden = !visible
        ballNode.simdPosition = pos.center
        ballShadow.position = SCNVector3(pos.x,0.04,pos.z)
        racket.simdScale = SIMD3(repeating: face.scale)
        racket.simdOrientation = face.rotation
        racket.simdPosition = face.center
        let approaching = !practice && game.approaching(face)
        if approaching != rimLit {
            rimLit = approaching
            playerRim?.emission.contents = approaching ? NSColor(rgb: 0x99f080) : NSColor.black
        }
        receiver.position = SCNVector3(game.farX+0.36,1.15,-8.75)
        receiver.eulerAngles.z = 0.36
        bounceMarker.isHidden = practice || game.phase != .incoming || game.progress > Rally.bounceProgress + 0.07
        if !bounceMarker.isHidden {
            let bounce = game.incomingBounce
            bounceMarker.position = SCNVector3(bounce.x, 0.056, bounce.z)
        }
    }
}
