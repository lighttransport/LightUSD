// SPDX-License-Identifier: Apache-2.0
//
// lmetalview - document/view coordination.
//
// Loading and time re-conversion run on one serial queue. Each finished result
// is published to the main thread as a fully independent SceneGPU, so the
// renderer never reads the LMV scene while it is being mutated.

import AppKit
import Metal
import MetalKit
import simd
import UniformTypeIdentifiers
import LMV

final class LoadProgress {
    private let lock = NSLock()
    private var _phase: Int32 = 0
    private var _fraction: Float = 0
    private var _message: String = ""
    private var _cancelled = false

    var cancelled: Bool {
        get { lock.lock(); defer { lock.unlock() }; return _cancelled }
        set { lock.lock(); _cancelled = newValue; lock.unlock() }
    }

    func update(_ phase: Int32, fraction: Float, message: String) {
        lock.lock()
        _phase = phase
        _fraction = fraction
        _message = message
        lock.unlock()
    }

    func snapshot() -> (Int32, Float, String) {
        lock.lock(); defer { lock.unlock() }
        return (_phase, _fraction, _message)
    }
}

final class ViewerController {
    let renderer: Renderer
    let camera: CameraController
    let hud = HUDView()
    weak var metalView: MetalView?

    private let builder: SceneBuilder
    private let loadQueue = DispatchQueue(label: "lmetalview.load", qos: .userInitiated)
    private var sceneHandle: OpaquePointer?
    private var currentPath: String?
    private var generation = 0
    private var loading = false
    // Keep following an animated authored camera until the user navigates.
    private var followAuthoredCamera = false
    private var timeInFlight = false
    private var pendingTime: Double?
    private var playing = false
    private var playTimer: Timer?
    // Wall-clock anchor so playback runs at the authored fps regardless of the
    // timer cadence (24 fps content on a 30 Hz timer used to play 1.25x fast).
    private var playStartWall: TimeInterval = 0
    private var playStartTime: Double = 0
    private var looping = true
    private var playbackSpeed: Double = 1.0
    private var hudTimer: Timer?
    private var progress = LoadProgress()
    private var settings = RenderSettings()

    init?(device: MTLDevice) {
        guard let renderer = Renderer(device: device),
              let camera = CameraController() else { return nil }
        self.renderer = renderer
        self.camera = camera
        // 1x1 white fallback texture for unbound material slots.
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: 1, height: 1, mipmapped: false)
        descriptor.usage = [.shaderRead]
        guard let dummy = device.makeTexture(descriptor: descriptor) else { return nil }
        var white: [UInt8] = [255, 255, 255, 255]
        dummy.replace(region: MTLRegionMake2D(0, 0, 1, 1), mipmapLevel: 0,
                      withBytes: &white, bytesPerRow: 4)
        self.builder = SceneBuilder(device: device, queue: renderer.queue, dummy: dummy)
        renderer.camera = camera
        renderer.settings = settings
        camera.onUserNavigate = { [weak self] in
            self?.followAuthoredCamera = false
        }
        hud.onTimeChanged = { [weak self] time in self?.setTime(time) }
        hud.onPlayToggle = { [weak self] in self?.togglePlay() }
    }

    deinit {
        // Scene access is serialized on loadQueue; wait for any in-flight
        // re-conversion before releasing the handle.
        loadQueue.sync {
            if let sceneHandle { lmv_scene_free(sceneHandle) }
        }
    }

    // MARK: - Files

    static func isSupported(path: String) -> Bool {
        let ext = (path as NSString).pathExtension.lowercased()
        return ext == "usd" || ext == "usda" || ext == "usdc" || ext == "usdz"
    }

    func open(url: URL) {
        guard Self.isSupported(path: url.path) else { return }
        load(path: url.path)
    }

    func openPanel() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [
            UTType(filenameExtension: "usd") ?? .data,
            UTType(filenameExtension: "usda") ?? .data,
            UTType(filenameExtension: "usdc") ?? .data,
            UTType(filenameExtension: "usdz") ?? .data
        ]
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        if panel.runModal() == .OK, let url = panel.url {
            open(url: url)
        }
    }

    func reload() {
        if let path = currentPath { load(path: path) }
    }

    func load(path rawPath: String) {
        // Relative CLI/Finder paths: resolve against the shell PWD / executable
        // ancestors, because a LaunchServices-launched .app starts at "/".
        let path = InputPath.resolve(rawPath)
        generation += 1
        let gen = generation
        currentPath = path
        pendingTime = nil
        followAuthoredCamera = false
        loading = true
        playing = false
        playTimer?.invalidate()
        hud.setPlaying(false)
        hud.setTitle((path as NSString).lastPathComponent)
        hud.setStatus("Loading…")
        hud.configureTimeline(start: 0, end: 1, value: 0, hasAnimation: false,
                              playing: false)
        let reporter = LoadProgress()
        progress = reporter
        startHUDTimer()
        metalView?.window?.title = (path as NSString).lastPathComponent

        loadQueue.async { [weak self] in
            guard let self else { return }
            var options = lmv_load_options()
            lmv_load_options_init(&options)
            options.max_memory_bytes = 2 << 30
            options.max_texture_bytes = 768 << 20
            options.max_texture_edge = 2048
            options.max_triangles = 32 << 20
            options.load_payloads = 1
            options.progress = ViewerController.progressCallback
            options.progress_user = Unmanaged.passUnretained(reporter).toOpaque()

            var scene: OpaquePointer?
            var error = [CChar](repeating: 0, count: 1024)
            let status = path.withCString { cPath in
                lmv_load(cPath, &options, &scene, &error, error.count)
            }
            guard status == LMV_OK, let scene else {
                let message = error.withUnsafeBufferPointer {
                    String(cString: $0.baseAddress!)
                }
                DispatchQueue.main.async {
                    guard gen == self.generation else { return }
                    self.loading = false
                    self.stopHUDTimer()
                    self.hud.setStatus("Failed to load")
                    self.presentError("Could not open \((path as NSString).lastPathComponent)",
                                      detail: message.isEmpty ? "Unknown error (\(status.rawValue))" : message)
                }
                return
            }

            let gpu = self.builder.build(scene: scene, path: path,
                                         exposure: self.settings.exposure,
                                         shadows: self.settings.shadows)
            DispatchQueue.main.async {
                guard gen == self.generation else {
                    lmv_scene_free(scene)
                    return
                }
                guard let gpu else {
                    lmv_scene_free(scene)
                    self.loading = false
                    self.stopHUDTimer()
                    self.hud.setStatus("Failed to build GPU scene")
                    self.presentError("GPU upload failed",
                                      detail: "The scene converted, but Metal resources could not be created.")
                    return
                }
                self.finishLoad(scene: scene, gpu: gpu, path: path)
            }
        }
    }

    private func finishLoad(scene: OpaquePointer, gpu: SceneGPU, path: String) {
        // The previous handle may still be in use by an in-flight time
        // re-conversion; release it on the loader queue so it runs after that
        // job (scene access is serialized there).
        if let old = sceneHandle {
            sceneHandle = nil
            loadQueue.async { lmv_scene_free(old) }
        }
        sceneHandle = scene
        currentPath = path
        loading = false
        stopHUDTimer()
        renderer.install(scene: gpu)
        camera.setUpAxis(zUp: gpu.upAxisZ)
        camera.frameBounds(min: gpu.boundsMin, max: gpu.boundsMax)
        // Some layers start at frame 1 (or outside 0..end). Clamp the initial
        // playhead so the first playback tick does not wrap backwards.
        let initialTime = min(max(gpu.timeCode, gpu.startTime), gpu.endTime)
        gpu.timeCode = initialTime
        hud.configureTimeline(start: gpu.startTime, end: gpu.endTime,
                              value: initialTime,
                              hasAnimation: gpu.hasAnimation, playing: false)
        hud.updateTimeLabel(initialTime, fps: gpu.fps)
        hud.setTitle((path as NSString).lastPathComponent)
        hud.setStatus(statusText(gpu))
        metalView?.window?.title = (path as NSString).lastPathComponent
        if !gpu.warnings.isEmpty {
            NSLog("lmetalview: %d conversion warning(s):\n%@", gpu.warnings.count,
                  gpu.warnings.joined(separator: "\n"))
        }
        requestRedraw()
    }

    private func statusText(_ scene: SceneGPU) -> String {
        var parts: [String] = []
        if scene.triangleCount == 0 {
            parts.append("no renderable geometry")
        } else {
            parts.append("\(scene.triangleCount) tris")
        }
        parts.append("\(scene.draws.count) draws")
        parts.append("\(scene.materials.count) materials")
        if scene.envTexture != nil { parts.append("IBL") }
        if scene.usesStudioRig { parts.append("studio lights") }
        if !scene.warnings.isEmpty {
            parts.append("\(scene.warnings.count) warning(s)")
        }
        return parts.joined(separator: " · ")
    }

    // MARK: - Time

    func setTime(_ time: Double) {
        guard sceneHandle != nil else { return }
        if playing {
            // A scrub/step far from the wall-clock prediction re-anchors
            // playback instead of snapping back on the next tick.
            let now = ProcessInfo.processInfo.systemUptime
            let predicted =
                playStartTime + (now - playStartWall) * playbackSpeed
            if abs(time - predicted) > 0.25 {
                playStartTime = time
                playStartWall = now
            }
        }
        pendingTime = time
        pumpTime()
    }

    private func pumpTime() {
        guard !timeInFlight, let time = pendingTime, let scene = sceneHandle else { return }
        pendingTime = nil
        timeInFlight = true
        let path = currentPath ?? ""
        let gen = generation
        let base = renderer.scene
        loadQueue.async { [weak self] in
            var error = [CChar](repeating: 0, count: 512)

            // Fast path: update node transforms, skin palettes and blend-shape
            // weights in place. Geometry buffers are untouched.
            let fastStatus = lmv_scene_update_animation(scene, time, &error,
                                                        error.count)
            if fastStatus == LMV_OK, let self, let base,
               let animation = self.builder.buildAnimation(scene: scene,
                                                           base: base) {
                DispatchQueue.main.async {
                    self.timeInFlight = false
                    if gen == self.generation {
                        base.animation = animation
                        base.timeCode = time
                        self.applyAuthoredCamera(animation.camera)
                        self.hud.updateTimeLabel(time, fps: base.fps)
                        self.requestRedraw()
                    }
                    self.pumpTime()
                }
                return
            }

            // Fallback: full re-conversion for anything the fast path does not
            // cover (animated topology, textures, visibility, ...).
            let status = lmv_scene_set_time(scene, time, &error, error.count)
            var gpu: SceneGPU?
            if status == LMV_OK, let self {
                gpu = self.builder.build(scene: scene, path: path,
                                         exposure: self.settings.exposure,
                                         shadows: self.settings.shadows)
            }
            DispatchQueue.main.async {
                guard let self else { return }
                self.timeInFlight = false
                if status == LMV_OK, let gpu, gen == self.generation {
                    self.renderer.install(scene: gpu)
                    self.applyAuthoredCamera(gpu.authoredCamera)
                    self.hud.updateTimeLabel(time, fps: gpu.fps)
                    self.requestRedraw()
                } else if status != LMV_ERR_CANCELLED && status != LMV_OK {
                    let message = error.withUnsafeBufferPointer {
                        String(cString: $0.baseAddress!)
                    }
                    NSLog("lmetalview: time re-conversion failed: %@", message)
                }
                self.pumpTime()
            }
        }
    }

    func togglePlay() {
        guard let scene = renderer.scene, scene.hasAnimation else { return }
        if !playing, !looping,
           scene.timeCode >= scene.endTime - 1e-9 {
            // Restart a non-looping clip that ended.
            hud.timeline.doubleValue = scene.startTime
            setTime(scene.startTime)
        }
        playing.toggle()
        hud.setPlaying(playing)
        playTimer?.invalidate()
        if playing {
            anchorPlaybackClock()
            playTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0,
                                             repeats: true) { [weak self] _ in
                self?.advancePlayback()
            }
        }
    }

    private func anchorPlaybackClock() {
        guard let scene = renderer.scene else { return }
        playStartWall = ProcessInfo.processInfo.systemUptime
        playStartTime = min(max(hud.timeline.doubleValue,
                                scene.startTime), scene.endTime)
    }

    private func stopPlayback() {
        playing = false
        hud.setPlaying(false)
        playTimer?.invalidate()
        playTimer = nil
    }

    private func advancePlayback() {
        guard let scene = renderer.scene, scene.hasAnimation else { return }
        let elapsed =
            (ProcessInfo.processInfo.systemUptime - playStartWall) * playbackSpeed
        let span = scene.endTime - scene.startTime
        var time = min(max(playStartTime, scene.startTime), scene.endTime) + elapsed
        if time > scene.endTime && !looping {
            hud.timeline.doubleValue = scene.endTime
            hud.updateTimeLabel(scene.endTime, fps: scene.fps)
            setTime(scene.endTime)
            stopPlayback()
            return
        }
        if span > 1e-9 {
            let offset = (time - scene.startTime)
                .truncatingRemainder(dividingBy: span)
            time = scene.startTime + (offset < 0 ? offset + span : offset)
        } else {
            time = scene.timeCode
        }
        hud.timeline.doubleValue = time
        hud.updateTimeLabel(time, fps: scene.fps)
        setTime(time)
    }

    // MARK: - Camera

    func frameAll() {
        guard let scene = renderer.scene else { return }
        camera.frameBounds(min: scene.boundsMin, max: scene.boundsMax)
        requestRedraw()
    }

    func useAuthoredCamera() {
        guard let scene = renderer.scene,
              let authored = scene.animation?.camera ?? scene.authoredCamera else {
            return
        }
        camera.setUpAxis(zUp: scene.upAxisZ)
        camera.useAuthoredCamera(authored)
        followAuthoredCamera = true
        requestRedraw()
    }

    private func applyAuthoredCamera(_ authored: AuthoredCamera?) {
        guard followAuthoredCamera, let authored else { return }
        camera.useAuthoredCamera(authored)
    }

    // MARK: - Rendering settings

    func toggleWireframe() {
        settings.wireframe.toggle()
        renderer.settings = settings
        requestRedraw()
    }

    func toggleGrid() {
        settings.grid.toggle()
        renderer.settings = settings
        requestRedraw()
    }

    func toggleShadows() {
        settings.shadows.toggle()
        renderer.settings = settings
        requestRedraw()
    }

    func toggleBackground() {
        settings.backgroundColor.toggle()
        renderer.settings = settings
        requestRedraw()
    }

    func adjustExposure(_ delta: Float) {
        settings.exposure = max(-8, min(8, settings.exposure + delta))
        renderer.settings = settings
        hud.setStatus(String(format: "Exposure %+.2f", settings.exposure))
        requestRedraw()
    }

    func resetExposure() {
        settings.exposure = 0
        renderer.settings = settings
        requestRedraw()
    }

    private func setPlaybackSpeed(_ speed: Double) {
        playbackSpeed = min(max(speed, 0.125), 8.0)
        if playing { anchorPlaybackClock() }
        hud.setStatus(String(format: "Playback %.2fx", playbackSpeed))
    }

    func requestRedraw() {
        metalView?.setNeedsDisplay(metalView?.bounds ?? .zero)
    }

    // MARK: - Screenshot

    func saveScreenshot() {
        guard let scene = renderer.scene else { return }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.png]
        panel.nameFieldStringValue = "lmetalview.png"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let size = metalView.map { SIMD2<Int>(Int($0.drawableSize.width), Int($0.drawableSize.height)) }
            ?? SIMD2<Int>(1280, 720)
        guard let image = renderer.renderScreenshot(scene: scene, camera: camera,
                                                    size: size, settings: settings) else {
            presentError("Screenshot failed", detail: "Could not render an image.")
            return
        }
        if !Renderer.writePNG(image, to: url) {
            presentError("Screenshot failed", detail: "Could not write \(url.path).")
        }
    }

    func screenshotImage(size: SIMD2<Int>) -> CGImage? {
        guard let scene = renderer.scene else { return nil }
        return renderer.renderScreenshot(scene: scene, camera: camera, size: size,
                                         settings: settings)
    }

    // MARK: - Keyboard

    func handleKey(_ event: NSEvent) -> Bool {
        guard let characters = event.charactersIgnoringModifiers?.lowercased() else { return false }
        switch characters {
        case "f":
            frameAll()
            return true
        case "w":
            toggleWireframe()
            return true
        case "g":
            toggleGrid()
            return true
        case "s":
            toggleShadows()
            return true
        case " ":
            togglePlay()
            return true
        case "l":
            looping.toggle()
            hud.setStatus(looping ? "Loop on" : "Loop off")
            return true
        case ",":
            setPlaybackSpeed(playbackSpeed * 0.5)
            return true
        case ".":
            setPlaybackSpeed(playbackSpeed * 2.0)
            return true
        case "0":
            setPlaybackSpeed(1.0)
            return true
        case "[":
            adjustExposure(-0.25)
            return true
        case "]":
            adjustExposure(0.25)
            return true
        case "1", "2", "3", "4", "5", "6", "7":
            if let value = Int(characters) {
                camera.setPreset(value - 1)
                requestRedraw()
            }
            return true
        default:
            break
        }
        switch event.keyCode {
        case 123, 124:  // left / right arrow: step one frame
            guard let scene = renderer.scene, scene.hasAnimation else { return true }
            let step = (event.keyCode == 123 ? -1.0 : 1.0) / max(scene.fps, 1.0)
            var time = scene.timeCode + step
            time = min(max(time, scene.startTime), scene.endTime)
            hud.timeline.doubleValue = time
            setTime(time)
            return true
        default:
            return false
        }
    }

    // MARK: - HUD progress

    private func startHUDTimer() {
        hudTimer?.invalidate()
        hudTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            guard let self, self.loading else { return }
            let (phase, fraction, message) = self.progress.snapshot()
            let name: String
            switch phase {
            case Int32(LMV_PHASE_OPEN.rawValue): name = "Opening"
            case Int32(LMV_PHASE_COMPOSE.rawValue): name = "Composing"
            case Int32(LMV_PHASE_CONVERT.rawValue): name = "Converting"
            case Int32(LMV_PHASE_TEXTURES.rawValue): name = "Decoding textures"
            default: name = "Working"
            }
            if fraction > 0.001 {
                self.hud.setStatus(String(format: "%@… %.0f%%", name, fraction * 100))
            } else if !message.isEmpty {
                self.hud.setStatus("\(name)…")
            } else {
                self.hud.setStatus("\(name)…")
            }
        }
    }

    private func stopHUDTimer() {
        hudTimer?.invalidate()
        hudTimer = nil
    }

    // MARK: - Errors

    func presentError(_ message: String, detail: String) {
        let alert = NSAlert()
        alert.messageText = message
        alert.informativeText = detail
        alert.alertStyle = .warning
        if let window = metalView?.window {
            alert.beginSheetModal(for: window, completionHandler: nil)
        } else {
            alert.runModal()
        }
    }

    private static let progressCallback: @convention(c)
        (UnsafeMutableRawPointer?, UnsafePointer<lmv_progress>?) -> Int32 = { user, progress in
        guard let user, let progress else { return 1 }
        let reporter = Unmanaged<LoadProgress>.fromOpaque(user).takeUnretainedValue()
        if reporter.cancelled { return 0 }
        let message = progress.pointee.message.map { String(cString: $0) } ?? ""
        reporter.update(progress.pointee.phase, fraction: progress.pointee.fraction,
                        message: message)
        return 1
    }
}
