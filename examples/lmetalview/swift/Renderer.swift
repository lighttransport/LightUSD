// SPDX-License-Identifier: Apache-2.0
//
// lmetalview - Metal renderer.
//
// One pass graph: background -> shadow map (directional) -> opaque meshes ->
// blended meshes -> wireframe overlay -> ground grid. MSAA 4x when the device
// supports it. Shaders are compiled from Shaders.swift at startup.

import Foundation
import Metal
import MetalKit
import simd
import ImageIO
import CoreGraphics
import LMV
import UniformTypeIdentifiers

struct RenderSettings {
    var exposure: Float = 0.0
    var wireframe = false
    var grid = true
    var shadows = true
    var backgroundColor = true
}

final class Renderer: NSObject, MTKViewDelegate {
    let device: MTLDevice
    let queue: MTLCommandQueue
    private(set) var library: MTLLibrary?

    private var meshOpaque: MTLRenderPipelineState?
    private var meshBlend: MTLRenderPipelineState?
    private var wirePipeline: MTLRenderPipelineState?
    private var backgroundPipeline: MTLRenderPipelineState?
    private var gridPipeline: MTLRenderPipelineState?
    private var splatPipeline: MTLRenderPipelineState?
    private var linePipeline: MTLRenderPipelineState?
    private var shadowPipeline: MTLRenderPipelineState?
    private var depthWrite: MTLDepthStencilState?
    private var depthRead: MTLDepthStencilState?
    private var depthAlways: MTLDepthStencilState?
    private var pipelineKey: String = ""

    private var shadowMap: MTLTexture?
    private let shadowSize = 2048
    private var gridVertices: MTLBuffer?
    private var gridIndices: MTLBuffer?
    private var gridIndexCount = 0

    var scene: SceneGPU?
    var camera: CameraController?
    var settings = RenderSettings()
    var onFrameRendered: (() -> Void)?

    private var viewSize = SIMD2<Int>(1, 1)

    init?(device: MTLDevice) {
        guard let queue = device.makeCommandQueue() else { return nil }
        self.device = device
        self.queue = queue
        super.init()
    }

    // MARK: - Setup

    func loadLibrary() -> String? {
        do {
            library = try device.makeLibrary(source: Shaders.source, options: nil)
            return nil
        } catch {
            return "Metal shader compilation failed: \(error.localizedDescription)"
        }
    }

    private func makeDepthState(write: Bool, compare: MTLCompareFunction) -> MTLDepthStencilState? {
        let descriptor = MTLDepthStencilDescriptor()
        descriptor.isDepthWriteEnabled = write
        descriptor.depthCompareFunction = compare
        return device.makeDepthStencilState(descriptor: descriptor)
    }

    private func ensurePipelines(colorFormat: MTLPixelFormat, sampleCount: Int) {
        let key = "\(colorFormat.rawValue)-\(sampleCount)"
        if key == pipelineKey, meshOpaque != nil { return }
        guard let library else { return }
        pipelineKey = key

        func makePipeline(vertex: String, fragment: String,
                          blending: Bool, writeDepth: Bool,
                          depthCompare: MTLCompareFunction,
                          colorFormat: MTLPixelFormat,
                          sampleCount: Int) -> MTLRenderPipelineState? {
            let descriptor = MTLRenderPipelineDescriptor()
            descriptor.vertexFunction = library.makeFunction(name: vertex)
            descriptor.fragmentFunction = library.makeFunction(name: fragment)
            descriptor.colorAttachments[0].pixelFormat = colorFormat
            descriptor.depthAttachmentPixelFormat = .depth32Float
            descriptor.rasterSampleCount = sampleCount
            if blending, let attachment = descriptor.colorAttachments[0] {
                attachment.isBlendingEnabled = true
                attachment.rgbBlendOperation = .add
                attachment.alphaBlendOperation = .add
                attachment.sourceRGBBlendFactor = .sourceAlpha
                attachment.sourceAlphaBlendFactor = .one
                attachment.destinationRGBBlendFactor = .oneMinusSourceAlpha
                attachment.destinationAlphaBlendFactor = .oneMinusSourceAlpha
            }
            _ = writeDepth
            _ = depthCompare
            return try? device.makeRenderPipelineState(descriptor: descriptor)
        }

        meshOpaque = makePipeline(vertex: "meshVertex", fragment: "meshFragment",
                                  blending: false, writeDepth: true,
                                  depthCompare: .lessEqual, colorFormat: colorFormat,
                                  sampleCount: sampleCount)
        meshBlend = makePipeline(vertex: "meshVertex", fragment: "meshFragment",
                                 blending: true, writeDepth: false,
                                 depthCompare: .lessEqual, colorFormat: colorFormat,
                                 sampleCount: sampleCount)
        wirePipeline = makePipeline(vertex: "meshVertex", fragment: "wireFragment",
                                    blending: true, writeDepth: false,
                                    depthCompare: .lessEqual,
                                    colorFormat: colorFormat,
                                    sampleCount: sampleCount)
        backgroundPipeline = makePipeline(vertex: "backgroundVertex",
                                          fragment: "backgroundFragment",
                                          blending: false, writeDepth: false,
                                          depthCompare: .always,
                                          colorFormat: colorFormat,
                                          sampleCount: sampleCount)
        gridPipeline = makePipeline(vertex: "gridVertex", fragment: "gridFragment",
                                    blending: true, writeDepth: false,
                                    depthCompare: .lessEqual,
                                    colorFormat: colorFormat,
                                    sampleCount: sampleCount)
        splatPipeline = makePipeline(vertex: "splatVertex", fragment: "splatFragment",
                                     blending: true, writeDepth: false,
                                     depthCompare: .lessEqual,
                                     colorFormat: colorFormat,
                                     sampleCount: sampleCount)
        linePipeline = makePipeline(vertex: "lineVertex", fragment: "lineFragment",
                                    blending: true, writeDepth: false,
                                    depthCompare: .lessEqual,
                                    colorFormat: colorFormat,
                                    sampleCount: sampleCount)

        // Shadow pass: depth only.
        if let vertex = library.makeFunction(name: "shadowVertex"),
           let fragment = library.makeFunction(name: "shadowFragment") {
            let descriptor = MTLRenderPipelineDescriptor()
            descriptor.vertexFunction = vertex
            descriptor.fragmentFunction = fragment
            descriptor.colorAttachments[0].pixelFormat = .invalid
            descriptor.depthAttachmentPixelFormat = .depth32Float
            descriptor.rasterSampleCount = 1
            shadowPipeline = try? device.makeRenderPipelineState(descriptor: descriptor)
        }

        depthWrite = makeDepthState(write: true, compare: .lessEqual)
        depthRead = makeDepthState(write: false, compare: .lessEqual)
        depthAlways = makeDepthState(write: false, compare: .always)

        if shadowMap == nil {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .depth32Float, width: shadowSize,
                height: shadowSize, mipmapped: false)
            descriptor.usage = [.renderTarget, .shaderRead]
            descriptor.storageMode = .private
            shadowMap = device.makeTexture(descriptor: descriptor)
            shadowMap?.label = "lmv.shadowMap"
        }
    }

    func validateLayouts() -> String? {
        let checks: [(String, Int, Int)] = [
            ("Vertex", 56, MemoryLayout<Vertex>.stride),
            ("SlotParams", 112, MemoryLayout<SlotParamsSwift>.stride),
            ("MaterialUniforms", 1024, MemoryLayout<MaterialUniformsSwift>.stride),
            ("DrawUniforms", 144, MemoryLayout<DrawUniformsSwift>.stride),
            ("FrameUniforms", 848, MemoryLayout<FrameUniformsSwift>.stride),
            ("GPULight", 64, MemoryLayout<GPULightSwift>.stride)
        ]
        for (name, expected, actual) in checks where expected != actual {
            return "\(name) layout mismatch: Swift \(actual) bytes, MSL \(expected)"
        }
        return nil
    }

    // MARK: - Scene installation

    func install(scene: SceneGPU) {
        self.scene = scene
        rebuildGrid(scene: scene)
    }

    private func rebuildGrid(scene: SceneGPU) {
        let extent = scene.boundsMax - scene.boundsMin
        let radius = max(simd_length(extent) * 0.5, 0.05)
        let center = (scene.boundsMin + scene.boundsMax) * 0.5
        let spacing = max(radius * 0.25, 1e-4)
        let half = radius * 8
        var corners: [Vertex] = []
        let y = scene.upAxisZ ? center.y : center.y
        let z = scene.upAxisZ ? center.z : center.z
        if scene.upAxisZ {
            // Ground plane = XY at z = bounds bottom.
            let zz = scene.boundsMin.z
            let pts = [SIMD2<Float>(-half, -half), SIMD2<Float>(half, -half),
                       SIMD2<Float>(half, half), SIMD2<Float>(-half, half)]
            for p in pts {
                var v = Vertex()
                v.px = center.x + p.x
                v.py = center.y + p.y
                v.pz = zz
                corners.append(v)
            }
            _ = y
            _ = z
        } else {
            let yy = scene.boundsMin.y
            let pts = [SIMD2<Float>(-half, -half), SIMD2<Float>(half, -half),
                       SIMD2<Float>(half, half), SIMD2<Float>(-half, half)]
            for p in pts {
                var v = Vertex()
                v.px = center.x + p.x
                v.py = yy
                v.pz = center.z + p.y
                corners.append(v)
            }
        }
        let indices: [UInt32] = [0, 1, 2, 0, 2, 3]
        gridVertices = device.makeBuffer(bytes: corners,
                                         length: corners.count * MemoryLayout<Vertex>.stride,
                                         options: .storageModeShared)
        gridIndices = device.makeBuffer(bytes: indices, length: indices.count * 4,
                                        options: .storageModeShared)
        gridIndexCount = indices.count
        gridSpacing = spacing
        gridExtent = max(radius * 4.0, 0.5)
        gridCenterX = center.x
        gridCenterY = center.y
        gridCenterZ = center.z
    }

    private var gridSpacing: Float = 1
    private var gridExtent: Float = 10
    private var gridCenterX: Float = 0
    private var gridCenterY: Float = 0
    private var gridCenterZ: Float = 0

    // MARK: - MTKViewDelegate

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        viewSize = SIMD2<Int>(max(Int(size.width), 1), max(Int(size.height), 1))
        camera?.setAspect(Float(viewSize.x) / Float(viewSize.y))
    }

    func draw(in view: MTKView) {
        guard let scene, let camera else { return }
        let drawableSize = view.drawableSize
        let width = max(Int(drawableSize.width), 1)
        let height = max(Int(drawableSize.height), 1)
        viewSize = SIMD2<Int>(width, height)
        ensurePipelines(colorFormat: view.colorPixelFormat,
                        sampleCount: view.sampleCount)
        guard let descriptor = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable,
              let commandBuffer = queue.makeCommandBuffer() else { return }
        descriptor.colorAttachments[0].loadAction = .clear
        descriptor.colorAttachments[0].storeAction = view.sampleCount > 1 ? .multisampleResolve : .store
        descriptor.colorAttachments[0].clearColor = MTLClearColor(red: 0.08, green: 0.085, blue: 0.095, alpha: 1)
        descriptor.depthAttachment?.loadAction = .clear
        descriptor.depthAttachment?.clearDepth = 1.0
        descriptor.depthAttachment?.storeAction = .dontCare

        camera.setAspect(Float(width) / Float(height))
        var frame = camera.frame()
        encodeScene(scene: scene, frame: &frame, commandBuffer: commandBuffer,
                    descriptor: descriptor, width: width, height: height)
        commandBuffer.present(drawable)
        commandBuffer.commit()
        onFrameRendered?()
    }

    // MARK: - Encoding

    private func makeFrameUniforms(scene: SceneGPU, camera frame: lmv_camera_frame,
                                   width: Int, height: Int) -> FrameUniformsSwift {
        var uniforms = FrameUniformsSwift()
        do {
            var view = frame.view
            withUnsafePointer(to: &view) { pointer in
                pointer.withMemoryRebound(to: Float.self, capacity: 16) {
                    uniforms.view = mat4($0)
                }
            }
            var proj = frame.proj
            withUnsafePointer(to: &proj) { pointer in
                pointer.withMemoryRebound(to: Float.self, capacity: 16) {
                    uniforms.proj = mat4($0)
                }
            }
        }
        uniforms.invViewProj = (uniforms.proj * uniforms.view).inverse
        uniforms.cameraPos = SIMD4<Float>(frame.eye.0, frame.eye.1, frame.eye.2, 0)
        uniforms.toneParams = SIMD4<Float>(settings.exposure + frame.exposure, 0, 0, 0)
        uniforms.envParams = SIMD4<Float>(0, 1, 0, 0)
        uniforms.misc = SIMD4<Float>(Float(activeLights(scene).count),
                                     scene.envTexture != nil ? 1 : 0, 0, 0)
        uniforms.gridParams = SIMD4<Float>(gridSpacing, gridExtent,
                                           scene.upAxisZ ? 1 : 0,
                                           settings.grid ? 1 : 0)
        uniforms.gridCenter = SIMD4<Float>(gridCenterX, gridCenterY,
                                           gridCenterZ, 0)
        let matrix = scene.colorMatrix
        uniforms.cm0 = SIMD4<Float>(matrix.columns.0.x, matrix.columns.1.x,
                                    matrix.columns.2.x, 0)
        uniforms.cm1 = SIMD4<Float>(matrix.columns.0.y, matrix.columns.1.y,
                                    matrix.columns.2.y, 0)
        uniforms.cm2 = SIMD4<Float>(matrix.columns.0.z, matrix.columns.1.z,
                                    matrix.columns.2.z, 0)
        if scene.sh.count >= 9 {
            uniforms.sh0 = scene.sh[0]
            uniforms.sh1 = scene.sh[1]
            uniforms.sh2 = scene.sh[2]
            uniforms.sh3 = scene.sh[3]
            uniforms.sh4 = scene.sh[4]
            uniforms.sh5 = scene.sh[5]
            uniforms.sh6 = scene.sh[6]
            uniforms.sh7 = scene.sh[7]
            uniforms.sh8 = scene.sh[8]
        }
        let casters = shadowCasters(scene)
        let shadowEnabled = settings.shadows && !casters.isEmpty
        uniforms.shadowParams = SIMD4<Float>(shadowEnabled ? 1 : 0,
                                             1.0 / Float(shadowSize), 0.0015,
                                             shadowStrength)
        var matrices = (matrix_identity_float4x4, matrix_identity_float4x4,
                        matrix_identity_float4x4, matrix_identity_float4x4)
        var flags = SIMD4<Float>(0, 0, 0, 0)
        if shadowEnabled {
            for (slot, lightIndex) in casters.enumerated() {
                let matrix = shadowCasterMatrix(scene: scene, lightIndex: lightIndex)
                switch slot {
                case 0: matrices.0 = matrix; flags.x = 1
                case 1: matrices.1 = matrix; flags.y = 1
                case 2: matrices.2 = matrix; flags.z = 1
                default: matrices.3 = matrix; flags.w = 1
                }
            }
            uniforms.shadowMatrix = matrices.0
        }
        uniforms.shadowMatrices = matrices
        uniforms.shadowFlags = flags
        _ = width
        _ = height
        return uniforms
    }

    private var shadowStrength: Float = 0.65

    private func lightToLightDirection(_ light: GPULightSwift) -> SIMD3<Float> {
        let d = SIMD3<Float>(light.direction.x, light.direction.y, light.direction.z)
        let len = simd_length(d)
        return len > 1e-6 ? d / len : SIMD3<Float>(0, 1, 0)
    }

    private func shadowMatrix(scene: SceneGPU, lightIndex: Int) -> float4x4 {
        let center = (scene.boundsMin + scene.boundsMax) * 0.5
        let radius = max(simd_length(scene.boundsMax - scene.boundsMin) * 0.5, 0.01) * 1.25
        let dir = lightToLightDirection(activeLights(scene)[lightIndex])
        var up = SIMD3<Float>(0, 1, 0)
        if abs(simd_dot(dir, up)) > 0.99 { up = SIMD3<Float>(0, 0, 1) }
        let eye = center + dir * radius * 2.0
        let view = lookAt(eye: eye, target: center, up: up)
        let ortho = orthographic(halfExtent: radius, near: 0.0, far: radius * 4.0)
        return ortho * view
    }

    private func shadowCasters(_ scene: SceneGPU) -> [Int] {
        var result: [Int] = []
        for (i, light) in activeLights(scene).enumerated() {
            let type = Int(light.positionType.w)
            if type == 1 || type == 2 {
                result.append(i)
                if result.count == 4 { break }
            }
        }
        return result
    }

    private func shadowCasterMatrix(scene: SceneGPU, lightIndex: Int) -> float4x4 {
        let light = activeLights(scene)[lightIndex]
        if Int(light.positionType.w) == 1 {
            return shadowMatrix(scene: scene, lightIndex: lightIndex)
        }
        // Spot light: perspective from the light transform.
        let eye = SIMD3<Float>(light.positionType.x, light.positionType.y,
                               light.positionType.z)
        let dir = -lightToLightDirection(light)
        var up = SIMD3<Float>(0, 1, 0)
        if abs(simd_dot(dir, up)) > 0.99 { up = SIMD3<Float>(0, 0, 1) }
        let radius = max(simd_length(scene.boundsMax - scene.boundsMin) * 0.5, 0.1)
        let view = lookAt(eye: eye, target: eye + dir, up: up)
        let halfAngle = acos(max(min(light.colorIntensity.w, 0.9999), -0.9999))
        let fov = min(max(halfAngle * 2.0, 0.05), 3.0)
        let proj = perspectiveZeroOne(fovY: fov, aspect: 1.0,
                                      near: max(radius * 0.01, 0.01),
                                      far: radius * 4.0)
        return proj * view
    }

    private func perspectiveZeroOne(fovY: Float, aspect: Float, near: Float,
                                    far: Float) -> float4x4 {
        let f = 1.0 / tan(fovY * 0.5)
        return float4x4(columns: (
            SIMD4<Float>(f / aspect, 0, 0, 0),
            SIMD4<Float>(0, f, 0, 0),
            SIMD4<Float>(0, 0, far / (near - far), -1),
            SIMD4<Float>(0, 0, near * far / (near - far), 0)
        ))
    }

    private func lookAt(eye: SIMD3<Float>, target: SIMD3<Float>, up: SIMD3<Float>) -> float4x4 {
        let z = simd_normalize(eye - target)
        let x = simd_normalize(simd_cross(up, z))
        let y = simd_cross(z, x)
        return float4x4(columns: (
            SIMD4<Float>(x.x, y.x, z.x, 0),
            SIMD4<Float>(x.y, y.y, z.y, 0),
            SIMD4<Float>(x.z, y.z, z.z, 0),
            SIMD4<Float>(-simd_dot(x, eye), -simd_dot(y, eye), -simd_dot(z, eye), 1)
        ))
    }

    private func orthographic(halfExtent: Float, near: Float, far: Float) -> float4x4 {
        // Right-handed view space looks down -Z: map z = -near -> 0 and
        // z = -far -> 1 (Metal's 0..1 depth range).
        let sx = 1 / halfExtent
        let sy = 1 / halfExtent
        let sz = -1 / (far - near)
        return float4x4(columns: (
            SIMD4<Float>(sx, 0, 0, 0),
            SIMD4<Float>(0, sy, 0, 0),
            SIMD4<Float>(0, 0, sz, 0),
            SIMD4<Float>(0, 0, -near * sz, 1)
        ))
    }

    private func activeLights(_ scene: SceneGPU) -> [GPULightSwift] {
        scene.animation?.lights ?? scene.lights
    }

    private func drawVisible(_ scene: SceneGPU, index: Int) -> Bool {
        if let animation = scene.animation,
           index < animation.drawVisible.count {
            return animation.drawVisible[index]
        }
        return scene.draws[index].visible
    }

    private func drawMeshIndex(_ scene: SceneGPU, index: Int) -> Int {
        if let animation = scene.animation,
           index < animation.drawMeshes.count {
            return animation.drawMeshes[index]
        }
        return scene.draws[index].mesh
    }

    private func drawWorld(_ scene: SceneGPU, index: Int) -> float4x4 {
        if let animation = scene.animation,
           index < animation.drawWorlds.count {
            return animation.drawWorlds[index]
        }
        return scene.draws[index].world
    }

    private func makeDrawUniforms(_ scene: SceneGPU, index: Int) -> DrawUniformsSwift {
        let meshIndex = drawMeshIndex(scene, index: index)
        let mesh = scene.meshes[meshIndex]
        var uniforms = DrawUniformsSwift()
        uniforms.world = drawWorld(scene, index: index)
        uniforms.normalMat = normalMatrix(uniforms.world)
        let state = scene.animation?.meshes[meshIndex]
        let morphTargets = state?.morphTargets ?? mesh.morphTargets
        uniforms.params = SIMD4<Float>(0, 0, mesh.skinned ? 1 : 0,
                                       Float(morphTargets))
        return uniforms
    }

    private func bindDeform(_ scene: SceneGPU, meshIndex: Int,
                            encoder: MTLRenderCommandEncoder) {
        let mesh = scene.meshes[meshIndex]
        let state = scene.animation?.meshes[meshIndex]
        encoder.setVertexBuffer(state?.skinMatrices ?? mesh.skinMatrices,
                                offset: 0, index: 4)
        encoder.setVertexBuffer(mesh.morphDeltas, offset: 0, index: 5)
        encoder.setVertexBuffer(state?.morphWeights ?? mesh.morphWeights,
                                offset: 0, index: 6)
        encoder.setVertexBuffer(mesh.jointIndices, offset: 0, index: 7)
        encoder.setVertexBuffer(mesh.jointWeights, offset: 0, index: 8)
    }

    private func encodeScene(scene: SceneGPU, frame: inout lmv_camera_frame,
                             commandBuffer: MTLCommandBuffer,
                             descriptor: MTLRenderPassDescriptor,
                             width: Int, height: Int) {
        // Shadow pass: up to four directional/spot casters in a 2x2 atlas.
        let casters = shadowCasters(scene)
        let shadowEnabled = settings.shadows && !casters.isEmpty
        if shadowEnabled, let shadowPipeline, let shadowMap,
           let shadowCommand = queue.makeCommandBuffer() {
            let pass = MTLRenderPassDescriptor()
            pass.depthAttachment.texture = shadowMap
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.storeAction = .store
            pass.depthAttachment.clearDepth = 1.0
            if let encoder = shadowCommand.makeRenderCommandEncoder(descriptor: pass) {
                var uniforms = makeFrameUniforms(scene: scene, camera: frame,
                                                 width: width, height: height)
                encoder.setRenderPipelineState(shadowPipeline)
                encoder.setDepthStencilState(depthWrite)
                encoder.setCullMode(.none)
                encoder.setFrontFacing(.counterClockwise)
                encoder.setVertexBytes(&uniforms, length: MemoryLayout<FrameUniformsSwift>.stride, index: 2)
                for drawIndex in scene.draws.indices {
                    if !drawVisible(scene, index: drawIndex) { continue }
                    let meshIndex = drawMeshIndex(scene, index: drawIndex)
                    let mesh = scene.meshes[meshIndex]
                    var drawUniforms = makeDrawUniforms(scene, index: drawIndex)
                    encoder.setVertexBuffer(mesh.vertices, offset: 0, index: 0)
                    encoder.setVertexBytes(&drawUniforms,
                                           length: MemoryLayout<DrawUniformsSwift>.stride,
                                           index: 1)
                    bindDeform(scene, meshIndex: meshIndex, encoder: encoder)
                    encoder.setCullMode(mesh.doubleSided ? .none : .back)
                    encoder.drawIndexedPrimitives(type: .triangle,
                                                  indexCount: mesh.indexCount,
                                                  indexType: .uint32,
                                                  indexBuffer: mesh.indices,
                                                  indexBufferOffset: 0)
                }
                encoder.endEncoding()
            }
            shadowCommand.commit()
        }

        guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: descriptor) else { return }
        var uniforms = makeFrameUniforms(scene: scene, camera: frame, width: width, height: height)

        // Background.
        if settings.backgroundColor, let backgroundPipeline {
            encoder.setRenderPipelineState(backgroundPipeline)
            encoder.setDepthStencilState(depthAlways)
            encoder.setCullMode(.none)
            encoder.setFragmentBytes(&uniforms, length: MemoryLayout<FrameUniformsSwift>.stride, index: 2)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        }

        let lights = activeLights(scene)
        if !lights.isEmpty {
            encoder.setFragmentBytes(lights, length: MemoryLayout<GPULightSwift>.stride * lights.count, index: 4)
        }

        // Opaque + mask meshes.
        guard let meshOpaque else { encoder.endEncoding(); return }
        encoder.setRenderPipelineState(meshOpaque)
        encoder.setDepthStencilState(depthWrite)
        encoder.setFrontFacing(.counterClockwise)
        encoder.setVertexBytes(&uniforms, length: MemoryLayout<FrameUniformsSwift>.stride, index: 2)
        encoder.setFragmentBytes(&uniforms, length: MemoryLayout<FrameUniformsSwift>.stride, index: 2)

        func bindMaterial(_ material: MaterialGPU, encoder: MTLRenderCommandEncoder, scene: SceneGPU) {
            var m = material.uniforms
            encoder.setFragmentBytes(&m, length: MemoryLayout<MaterialUniformsSwift>.stride, index: 3)
            for slot in 0..<7 {
                encoder.setFragmentTexture(material.textures[slot] ?? scene.dummyTexture,
                                           index: slot)
                encoder.setFragmentTexture(material.arrayTextures[slot] ?? scene.dummyArrayTexture,
                                           index: 9 + slot)
            }
            encoder.setFragmentBuffer(scene.udimLut, offset: 0, index: 5)
            encoder.setFragmentBuffer(scene.ptexRects, offset: 0, index: 6)
            encoder.setFragmentTexture(shadowMap, index: 7)
            encoder.setFragmentTexture(scene.envTexture ?? scene.dummyTexture, index: 8)
        }

        func drawSubset(_ drawIndex: Int, _ draw: DrawGPU, _ subset: (Int, Int, Int), encoder: MTLRenderCommandEncoder, scene: SceneGPU) {
            let meshIndex = drawMeshIndex(scene, index: drawIndex)
            let mesh = scene.meshes[meshIndex]
            var drawUniforms = makeDrawUniforms(scene, index: drawIndex)
            encoder.setVertexBuffer(mesh.vertices, offset: 0, index: 0)
            encoder.setVertexBuffer(mesh.faceIds, offset: 0, index: 3)
            encoder.setVertexBytes(&drawUniforms, length: MemoryLayout<DrawUniformsSwift>.stride, index: 1)
            bindDeform(scene, meshIndex: meshIndex, encoder: encoder)
            var materialIndex = draw.materialOverride
            if materialIndex < 0 || materialIndex >= scene.materials.count {
                materialIndex = subset.2
            }
            let material = materialIndex >= 0 && materialIndex < scene.materials.count
                ? scene.materials[materialIndex] : fallbackMaterial()
            bindMaterial(material, encoder: encoder, scene: scene)
            encoder.setCullMode(mesh.doubleSided ? .none : .back)
            encoder.drawIndexedPrimitives(type: .triangle,
                                          indexCount: subset.1,
                                          indexType: .uint32,
                                          indexBuffer: mesh.indices,
                                          indexBufferOffset: subset.0 * 4)
        }

        for (drawIndex, draw) in scene.draws.enumerated() {
            if !drawVisible(scene, index: drawIndex) { continue }
            let mesh = scene.meshes[drawMeshIndex(scene, index: drawIndex)]
            for subset in mesh.subsets {
                var materialIndex = draw.materialOverride
                if materialIndex < 0 || materialIndex >= scene.materials.count {
                    materialIndex = subset.2
                }
                let alphaMode = materialIndex >= 0 && materialIndex < scene.materials.count
                    ? scene.materials[materialIndex].uniforms.p2.x : 0
                if alphaMode >= 1.5 { continue } // blend pass
                drawSubset(drawIndex, draw, subset, encoder: encoder, scene: scene)
            }
        }

        // Blended meshes, back to front.
        let eye = SIMD3<Float>(frame.eye.0, frame.eye.1, frame.eye.2)
        var blended: [(Int, DrawGPU, (Int, Int, Int), Float)] = []
        for (drawIndex, draw) in scene.draws.enumerated() {
            if !drawVisible(scene, index: drawIndex) { continue }
            let mesh = scene.meshes[drawMeshIndex(scene, index: drawIndex)]
            let world = drawWorld(scene, index: drawIndex)
            let center = SIMD3<Float>(world.columns.3.x, world.columns.3.y,
                                      world.columns.3.z)
            for subset in mesh.subsets {
                var materialIndex = draw.materialOverride
                if materialIndex < 0 || materialIndex >= scene.materials.count {
                    materialIndex = subset.2
                }
                let alphaMode = materialIndex >= 0 && materialIndex < scene.materials.count
                    ? scene.materials[materialIndex].uniforms.p2.x : 0
                if alphaMode < 1.5 { continue }
                blended.append((drawIndex, draw, subset, simd_distance(eye, center)))
            }
        }
        if !blended.isEmpty, let meshBlend {
            encoder.setRenderPipelineState(meshBlend)
            encoder.setDepthStencilState(depthRead)
            encoder.setCullMode(.none)
            for entry in blended.sorted(by: { $0.3 > $1.3 }) {
                drawSubset(entry.0, entry.1, entry.2, encoder: encoder,
                           scene: scene)
            }
        }

        // UsdGeomPoints / particles and curves.
        if let splats = scene.splatBuffer, scene.splatCount > 0,
           let splatPipeline {
            encoder.setRenderPipelineState(splatPipeline)
            encoder.setDepthStencilState(depthRead)
            encoder.setCullMode(.none)
            encoder.setVertexBuffer(splats, offset: 0, index: 0)
            encoder.setVertexBytes(&uniforms,
                                   length: MemoryLayout<FrameUniformsSwift>.stride,
                                   index: 2)
            var viewport = SIMD2<Float>(Float(width), Float(height))
            encoder.setVertexBytes(&viewport, length: 8, index: 4)
            encoder.setFragmentBytes(&uniforms,
                                     length: MemoryLayout<FrameUniformsSwift>.stride,
                                     index: 2)
            encoder.drawPrimitives(type: .point, vertexStart: 0,
                                   vertexCount: scene.splatCount)
        }
        if let lines = scene.lineBuffer, let lineIndices = scene.lineIndexBuffer,
           scene.lineCount > 0, let linePipeline {
            encoder.setRenderPipelineState(linePipeline)
            encoder.setDepthStencilState(depthRead)
            encoder.setCullMode(.none)
            encoder.setVertexBuffer(lines, offset: 0, index: 0)
            encoder.setVertexBytes(&uniforms,
                                   length: MemoryLayout<FrameUniformsSwift>.stride,
                                   index: 2)
            encoder.setFragmentBytes(&uniforms,
                                     length: MemoryLayout<FrameUniformsSwift>.stride,
                                     index: 2)
            encoder.drawIndexedPrimitives(type: .triangle,
                                          indexCount: scene.lineCount * 6,
                                          indexType: .uint32,
                                          indexBuffer: lineIndices,
                                          indexBufferOffset: 0)
        }

        // Wireframe overlay.
        if settings.wireframe, let wirePipeline {
            encoder.setRenderPipelineState(wirePipeline)
            encoder.setDepthStencilState(depthRead)
            encoder.setCullMode(.none)
            encoder.setTriangleFillMode(.lines)
            encoder.setVertexBytes(&uniforms, length: MemoryLayout<FrameUniformsSwift>.stride, index: 2)
            encoder.setFragmentBytes(&uniforms, length: MemoryLayout<FrameUniformsSwift>.stride, index: 2)
            for drawIndex in scene.draws.indices {
                if !drawVisible(scene, index: drawIndex) { continue }
                let meshIndex = drawMeshIndex(scene, index: drawIndex)
                let mesh = scene.meshes[meshIndex]
                var drawUniforms = makeDrawUniforms(scene, index: drawIndex)
                encoder.setVertexBuffer(mesh.vertices, offset: 0, index: 0)
                encoder.setVertexBuffer(mesh.faceIds, offset: 0, index: 3)
                encoder.setVertexBytes(&drawUniforms, length: MemoryLayout<DrawUniformsSwift>.stride, index: 1)
                bindDeform(scene, meshIndex: meshIndex, encoder: encoder)
                encoder.drawIndexedPrimitives(type: .triangle, indexCount: mesh.indexCount,
                                              indexType: .uint32, indexBuffer: mesh.indices,
                                              indexBufferOffset: 0)
            }
            encoder.setTriangleFillMode(.fill)
        }

        // Ground grid.
        if settings.grid, let gridPipeline, let gridVertices, let gridIndices {
            encoder.setRenderPipelineState(gridPipeline)
            encoder.setDepthStencilState(depthRead)
            encoder.setCullMode(.none)
            encoder.setFragmentTexture(shadowMap, index: 7)
            encoder.setVertexBuffer(gridVertices, offset: 0, index: 0)
            encoder.setVertexBytes(&uniforms, length: MemoryLayout<FrameUniformsSwift>.stride, index: 2)
            encoder.setFragmentBytes(&uniforms, length: MemoryLayout<FrameUniformsSwift>.stride, index: 2)
            encoder.drawIndexedPrimitives(type: .triangle, indexCount: gridIndexCount,
                                          indexType: .uint32, indexBuffer: gridIndices,
                                          indexBufferOffset: 0)
        }

        encoder.endEncoding()
    }

    private func fallbackMaterial() -> MaterialGPU {
        // Unbound geometry: a neutral preview surface.
        if let fallback = cachedFallback { return fallback }
        let material = MaterialGPU()
        material.uniforms.baseColor = SIMD4<Float>(0.62, 0.62, 0.64, 1)
        material.uniforms.p0 = SIMD4<Float>(0, 0, 0.5, 1.5)
        material.uniforms.p1 = SIMD4<Float>(0.1, 1.5, 0.3, 1)
        cachedFallback = material
        return material
    }
    private var cachedFallback: MaterialGPU?

    // MARK: - Screenshot

    func renderScreenshot(scene: SceneGPU, camera: CameraController,
                          size: SIMD2<Int>, settings: RenderSettings) -> CGImage? {
        self.settings = settings
        let width = max(size.x, 1)
        let height = max(size.y, 1)
        let sampleCount = device.supportsTextureSampleCount(4) ? 4 : 1
        ensurePipelines(colorFormat: .bgra8Unorm, sampleCount: sampleCount)
        camera.setAspect(Float(width) / Float(height))
        var frame = camera.frame()

        let colorDescriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
        colorDescriptor.usage = [.renderTarget, .shaderRead]
        colorDescriptor.storageMode = .shared
        // Resolve target.
        guard let resolve = device.makeTexture(descriptor: colorDescriptor) else { return nil }

        var colorTarget = resolve
        if sampleCount > 1 {
            let msaaDescriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
            msaaDescriptor.textureType = .type2DMultisample
            msaaDescriptor.sampleCount = sampleCount
            msaaDescriptor.usage = [.renderTarget]
            msaaDescriptor.storageMode = .private
            guard let msaa = device.makeTexture(descriptor: msaaDescriptor) else { return nil }
            colorTarget = msaa
        }

        let depthDescriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .depth32Float, width: width, height: height, mipmapped: false)
        if sampleCount > 1 {
            depthDescriptor.textureType = .type2DMultisample
            depthDescriptor.sampleCount = sampleCount
        }
        depthDescriptor.usage = [.renderTarget]
        depthDescriptor.storageMode = .private
        guard let depth = device.makeTexture(descriptor: depthDescriptor) else { return nil }

        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = colorTarget
        pass.colorAttachments[0].resolveTexture = sampleCount > 1 ? resolve : nil
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = sampleCount > 1 ? .multisampleResolve : .store
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.08, green: 0.085, blue: 0.095, alpha: 1)
        pass.depthAttachment.texture = depth
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.storeAction = .dontCare
        pass.depthAttachment.clearDepth = 1.0

        guard let commandBuffer = queue.makeCommandBuffer() else { return nil }
        encodeScene(scene: scene, frame: &frame, commandBuffer: commandBuffer,
                    descriptor: pass, width: width, height: height)
        commandBuffer.commit()
        commandBuffer.waitUntilCompleted()

        let rowBytes = width * 4
        var bytes = [UInt8](repeating: 0, count: rowBytes * height)
        resolve.getBytes(&bytes, bytesPerRow: rowBytes,
                         from: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0)
        guard let provider = CGDataProvider(data: Data(bytes) as CFData) else { return nil }
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        return CGImage(width: width, height: height, bitsPerComponent: 8,
                       bitsPerPixel: 32, bytesPerRow: rowBytes, space: colorSpace,
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false,
                       intent: .defaultIntent)
    }

    static func writePNG(_ image: CGImage, to url: URL) -> Bool {
        guard let destination = CGImageDestinationCreateWithURL(
            url as CFURL, UTType.png.identifier as CFString, 1, nil) else { return false }
        CGImageDestinationAddImage(destination, image, nil)
        return CGImageDestinationFinalize(destination)
    }
}
