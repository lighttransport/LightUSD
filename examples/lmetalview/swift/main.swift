// SPDX-License-Identifier: Apache-2.0
//
// lmetalview - entry point.
//
// Usage:
//   lmetalview [file.usd[za|c]]
//   lmetalview --headless [--size WxH] [--screenshot out.png] [--time T]
//              [--animate-from T0] [--exposure E] [--no-shadows]
//              [--no-grid] [--wireframe] file
//
// The headless mode (also the CTest smoke test) compiles the MSL, loads the
// scene, renders one frame offscreen and optionally writes a PNG.

import AppKit
import Metal
import MetalKit
import simd
import LMV

private struct Arguments {
    var path: String?
    var headless = false
    var screenshot: String?
    var width = 960
    var height = 640
    var time: Double = 0
    // Load at this time, then exercise the in-place playback path to `time`.
    var animateFrom: Double? = nil
    var exposure: Float = 0
    var shadows = true
    var grid = true
    var wireframe = false
    var authoredCamera = false

    static func parse(_ argv: [String]) -> Arguments? {
        var args = Arguments()
        var index = 1
        while index < argv.count {
            let arg = argv[index]
            switch arg {
            case "--help", "-h":
                print("""
                usage: lmetalview [file.usd[za|c]]
                       lmetalview --headless [--size WxH] [--screenshot out.png]
                                  [--time T] [--animate-from T0] [--exposure E]
                                  [--no-shadows] [--no-grid] [--wireframe]
                                  [--authored-camera] file
                """)
                return nil
            case "--headless":
                args.headless = true
            case "--screenshot":
                index += 1
                guard index < argv.count else { return nil }
                args.screenshot = argv[index]
            case "--size":
                index += 1
                guard index < argv.count else { return nil }
                let parts = argv[index].lowercased().split(separator: "x")
                if parts.count == 2, let w = Int(parts[0]), let h = Int(parts[1]) {
                    args.width = max(w, 1)
                    args.height = max(h, 1)
                }
            case "--time":
                index += 1
                if index < argv.count, let value = Double(argv[index]) { args.time = value }
            case "--animate-from":
                index += 1
                if index < argv.count, let value = Double(argv[index]) {
                    args.animateFrom = value
                }
            case "--exposure":
                index += 1
                if index < argv.count, let value = Float(argv[index]) { args.exposure = value }
            case "--no-shadows":
                args.shadows = false
            case "--no-grid":
                args.grid = false
            case "--wireframe":
                args.wireframe = true
            case "--authored-camera":
                args.authoredCamera = true
            default:
                if !arg.hasPrefix("-") { args.path = arg }
            }
            index += 1
        }
        return args
    }
}

private func progressMessage(_ user: UnsafeMutableRawPointer?,
                             _ progress: UnsafePointer<lmv_progress>?) -> Int32 {
    guard let progress else { return 1 }
    let p = progress.pointee
    let message = p.message.map { String(cString: $0) } ?? ""
    if p.phase != Int32(LMV_PHASE_DONE.rawValue) || !message.isEmpty {
        FileHandle.standardError.write(Data("lmetalview: [\(p.phase)] \(message)\n".utf8))
    }
    return 1
}

private func runHeadless(_ args: Arguments) -> Int32 {
    guard let rawPath = args.path else {
        FileHandle.standardError.write(Data("lmetalview: --headless needs a USD file\n".utf8))
        return 2
    }
    let path = InputPath.resolve(rawPath)
    if path != rawPath {
        FileHandle.standardError.write(
            Data("lmetalview: resolved \(rawPath) -> \(path)\n".utf8))
    }
    guard let device = MTLCreateSystemDefaultDevice() else {
        FileHandle.standardError.write(Data("lmetalview: no Metal device; skipping\n".utf8))
        return 77
    }
    guard let renderer = Renderer(device: device) else { return 1 }
    if let error = renderer.loadLibrary() {
        FileHandle.standardError.write(Data("lmetalview: \(error)\n".utf8))
        return 1
    }
    if let error = renderer.validateLayouts() {
        FileHandle.standardError.write(Data("lmetalview: \(error)\n".utf8))
        return 1
    }
    guard let dummy = makeWhiteTexture(device: device) else { return 1 }
    let builder = SceneBuilder(device: device, queue: renderer.queue, dummy: dummy)

    var options = lmv_load_options()
    lmv_load_options_init(&options)
    options.time_code = args.animateFrom ?? args.time
    options.progress = progressMessage
    var scene: OpaquePointer?
    var error = [CChar](repeating: 0, count: 1024)
    let status = path.withCString { cPath in
        lmv_load(cPath, &options, &scene, &error, error.count)
    }
    guard status == LMV_OK, let scene else {
        let message = error.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
        FileHandle.standardError.write(Data("lmetalview: load failed: \(message)\n".utf8))
        return 1
    }
    defer { lmv_scene_free(scene) }
    guard var gpu = builder.build(scene: scene, path: path,
                                  exposure: args.exposure, shadows: args.shadows) else {
        FileHandle.standardError.write(Data("lmetalview: GPU scene build failed\n".utf8))
        return 1
    }
    guard let camera = CameraController() else { return 1 }
    camera.setUpAxis(zUp: gpu.upAxisZ)
    if args.authoredCamera, let authored = gpu.authoredCamera {
        camera.useAuthoredCamera(authored)
    } else {
        camera.frameBounds(min: gpu.boundsMin, max: gpu.boundsMax)
    }
    camera.setAspect(Float(args.width) / Float(args.height))

    var settings = RenderSettings()
    settings.exposure = args.exposure
    settings.shadows = args.shadows
    settings.grid = args.grid
    settings.wireframe = args.wireframe
    renderer.settings = settings
    renderer.camera = camera

    // Reproduce the GUI playback path: load at one time, then update node
    // transforms / skin palettes / morph weights in place to another time.
    if let animateFrom = args.animateFrom, animateFrom != args.time {
        var updateError = [CChar](repeating: 0, count: 512)
        let updateStatus = lmv_scene_update_animation(scene, args.time,
                                                      &updateError,
                                                      updateError.count)
        if updateStatus == LMV_OK,
           let animation = builder.buildAnimation(scene: scene, base: gpu) {
            gpu.animation = animation
            gpu.timeCode = args.time
            if args.authoredCamera, let authored = animation.camera {
                camera.useAuthoredCamera(authored)
            }
        } else {
            // The in-place path does not own every channel (animated material
            // inputs, topology, ...). Match the GUI fallback.
            let status = lmv_scene_set_time(scene, args.time, &updateError,
                                            updateError.count)
            guard status == LMV_OK,
                  let rebuilt = builder.build(scene: scene, path: path,
                                              exposure: args.exposure,
                                              shadows: args.shadows) else {
                let message = updateError.withUnsafeBufferPointer {
                    String(cString: $0.baseAddress!)
                }
                FileHandle.standardError.write(
                    Data("lmetalview: animation fallback failed: \(message)\n".utf8))
                return 1
            }
            gpu = rebuilt
            camera.setUpAxis(zUp: gpu.upAxisZ)
            if args.authoredCamera, let authored = gpu.authoredCamera {
                camera.useAuthoredCamera(authored)
            } else {
                camera.frameBounds(min: gpu.boundsMin, max: gpu.boundsMax)
            }
            camera.setAspect(Float(args.width) / Float(args.height))
        }
    }
    renderer.install(scene: gpu)

    for warning in gpu.warnings {
        FileHandle.standardError.write(Data("lmetalview: warning: \(warning)\n".utf8))
    }
    print(String(format: "lmetalview: %@ — %d meshes, %d draws, %d triangles, %d materials, %d lights%@",
                 (path as NSString).lastPathComponent,
                 gpu.meshes.count, gpu.draws.count, gpu.triangleCount,
                 gpu.materials.count, gpu.lights.count,
                 gpu.envTexture != nil ? ", IBL" : ", studio"))

    if let screenshot = args.screenshot {
        let size = SIMD2<Int>(args.width, args.height)
        guard let image = renderer.renderScreenshot(scene: gpu, camera: camera,
                                                    size: size, settings: settings) else {
            FileHandle.standardError.write(Data("lmetalview: render failed\n".utf8))
            return 1
        }
        if !Renderer.writePNG(image, to: URL(fileURLWithPath: screenshot)) {
            FileHandle.standardError.write(Data("lmetalview: could not write \(screenshot)\n".utf8))
            return 1
        }
        print("lmetalview: wrote \(screenshot) (\(args.width)x\(args.height))")
    }
    return 0
}

guard let arguments = Arguments.parse(CommandLine.arguments) else {
    exit(2)
}

if arguments.headless {
    exit(runHeadless(arguments))
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
let delegate = AppDelegate(initialPath: arguments.path)
app.delegate = delegate
app.run()
