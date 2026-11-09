// SPDX-License-Identifier: Apache-2.0
//
// lmetalview - convert the C preview scene (core/lmv.h) into Metal resources.
//
// Building happens on the loader queue; the finished SceneGPU is handed to the
// main thread and installed on the renderer. The LMV scene itself is never
// touched by the render thread, so a time re-conversion cannot race a draw.

import Foundation
import Metal
import simd
import LMV

struct MeshGPU {
    var vertices: MTLBuffer
    var faceIds: MTLBuffer
    var indices: MTLBuffer
    var indexCount: Int
    var doubleSided: Bool
    // (firstIndex, indexCount, materialID)
    var subsets: [(Int, Int, Int)]
    // UsdSkel playback. Geometry stays in the bind pose; the vertex shader
    // applies the palette, morph deltas and four influences per vertex.
    var skinned: Bool = false
    var skinMatrices: MTLBuffer? = nil
    var jointIndices: MTLBuffer? = nil
    var jointWeights: MTLBuffer? = nil
    var morphDeltas: MTLBuffer? = nil
    var morphWeights: MTLBuffer? = nil
    var morphTargets: Int = 0
}

struct DrawGPU {
    var mesh: Int
    var materialOverride: Int
    var world: float4x4
    // Visibility can animate; the draw is kept and skipped at encode time.
    var visible: Bool
    // Index into lmv_scene's draw array (draws that fail validation are
    // skipped when building the GPU scene).
    var coreIndex: Int
}

struct MeshAnimationGPU {
    var skinMatrices: MTLBuffer?
    var jointCount: Int
    var morphWeights: MTLBuffer?
    var morphTargets: Int
}

// Per-time playback state. Rebuilt on the loader queue and installed on the
// main thread as one snapshot, so the renderer never sees a half update.
struct SceneAnimation {
    var drawWorlds: [float4x4]
    var drawVisible: [Bool]
    var drawMeshes: [Int]
    var meshes: [MeshAnimationGPU]
    var lights: [GPULightSwift]
    var camera: AuthoredCamera?
}

struct AuthoredCamera {
    var name: String
    var transform: [Float]  // column-major
    var orthographic: Bool
    var focalLength: Float
    var verticalAperture: Float
    var orthoWidth: Float
    var nearClip: Float
    var farClip: Float
    var focusDistance: Float
    var exposure: Float
    var fstop: Float
}

final class MaterialGPU {
    var uniforms = MaterialUniformsSwift()
    // Slot order: base color, metallic, roughness, normal, emission, opacity.
    var textures: [MTLTexture?] = Array(repeating: nil, count: 7)
    var arrayTextures: [MTLTexture?] = Array(repeating: nil, count: 7)
}

final class SceneGPU {
    let meshes: [MeshGPU]
    let draws: [DrawGPU]
    let materials: [MaterialGPU]
    let lights: [GPULightSwift]
    let sh: [SIMD4<Float>]
    let envTexture: MTLTexture?
    let boundsMin: SIMD3<Float>
    let boundsMax: SIMD3<Float>
    let upAxisZ: Bool
    let triangleCount: Int
    let vertexCount: Int
    let warnings: [String]
    let shadowLightIndex: Int
    let usesStudioRig: Bool
    let authoredCamera: AuthoredCamera?
    var timeCode: Double
    let startTime: Double
    let endTime: Double
    let fps: Double
    let hasAnimation: Bool
    let filePath: String
    let colorMatrix: simd_float3x3
    let splatBuffer: MTLBuffer?
    let splatCount: Int
    let lineBuffer: MTLBuffer?
    let lineIndexBuffer: MTLBuffer?
    let lineCount: Int
    let dummyTexture: MTLTexture
    let dummyArrayTexture: MTLTexture
    let udimLut: MTLBuffer
    let ptexRects: MTLBuffer
    // Current animation snapshot (nil until the first playback update).
    var animation: SceneAnimation?

    init(meshes: [MeshGPU], draws: [DrawGPU], materials: [MaterialGPU],
         lights: [GPULightSwift], sh: [SIMD4<Float>], envTexture: MTLTexture?,
         boundsMin: SIMD3<Float>, boundsMax: SIMD3<Float>, upAxisZ: Bool,
         triangleCount: Int, vertexCount: Int, warnings: [String],
         shadowLightIndex: Int, usesStudioRig: Bool,
         authoredCamera: AuthoredCamera?, timeCode: Double,
         startTime: Double, endTime: Double, fps: Double, hasAnimation: Bool,
         filePath: String, colorMatrix: simd_float3x3,
         splatBuffer: MTLBuffer?, splatCount: Int,
         lineBuffer: MTLBuffer?, lineIndexBuffer: MTLBuffer?, lineCount: Int,
         dummy: MTLTexture, dummyArray: MTLTexture,
         udimLut: MTLBuffer, ptexRects: MTLBuffer) {
        self.meshes = meshes
        self.draws = draws
        self.materials = materials
        self.lights = lights
        self.sh = sh
        self.envTexture = envTexture
        self.boundsMin = boundsMin
        self.boundsMax = boundsMax
        self.upAxisZ = upAxisZ
        self.triangleCount = triangleCount
        self.vertexCount = vertexCount
        self.warnings = warnings
        self.shadowLightIndex = shadowLightIndex
        self.usesStudioRig = usesStudioRig
        self.authoredCamera = authoredCamera
        self.timeCode = timeCode
        self.startTime = startTime
        self.endTime = endTime
        self.fps = fps
        self.hasAnimation = hasAnimation
        self.filePath = filePath
        self.colorMatrix = colorMatrix
        self.splatBuffer = splatBuffer
        self.splatCount = splatCount
        self.lineBuffer = lineBuffer
        self.lineIndexBuffer = lineIndexBuffer
        self.lineCount = lineCount
        self.dummyTexture = dummy
        self.dummyArrayTexture = dummyArray
        self.udimLut = udimLut
        self.ptexRects = ptexRects
    }
}

func makeWhiteArrayTexture(device: MTLDevice) -> MTLTexture? {
    let descriptor = MTLTextureDescriptor()
    descriptor.textureType = .type2DArray
    descriptor.pixelFormat = .rgba8Unorm
    descriptor.width = 1
    descriptor.height = 1
    descriptor.arrayLength = 1
    descriptor.mipmapLevelCount = 1
    descriptor.usage = [.shaderRead]
    descriptor.storageMode = .shared
    guard let texture = device.makeTexture(descriptor: descriptor) else { return nil }
    var white: [UInt8] = [255, 255, 255, 255]
    texture.replace(region: MTLRegionMake2D(0, 0, 1, 1), mipmapLevel: 0,
                    slice: 0, withBytes: &white, bytesPerRow: 4, bytesPerImage: 4)
    return texture
}

func makeWhiteTexture(device: MTLDevice) -> MTLTexture? {
    let descriptor = MTLTextureDescriptor.texture2DDescriptor(
        pixelFormat: .rgba8Unorm, width: 1, height: 1, mipmapped: false)
    descriptor.usage = [.shaderRead]
    guard let texture = device.makeTexture(descriptor: descriptor) else { return nil }
    var white: [UInt8] = [255, 255, 255, 255]
    texture.replace(region: MTLRegionMake2D(0, 0, 1, 1), mipmapLevel: 0,
                    withBytes: &white, bytesPerRow: 4)
    return texture
}

// Swift mirror of the packed OpenPBR indices in core/lmv.h.
enum PbrIndex {
    static let baseColor = 0
    static let baseWeight = 3
    static let specularColor = 4
    static let specularWeight = 7
    static let transmissionColor = 8
    static let transmission = 11
    static let coatColor = 24
    static let coatWeight = 27
    static let sheenColor = 28
    static let sheenWeight = 31
    static let emissionColor = 32
    static let emission = 35
    static let opacity = 39
    static let metalness = 40
    static let diffuseRoughness = 41
    static let specularRoughness = 42
    static let specularIor = 43
    static let coatRoughness = 44
    static let coatIor = 45
    static let sheenRoughness = 46
    static let alphaMode = 54
    static let alphaCutoff = 55
    static let thinWalled = 71
}

final class SceneBuilder {
    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let dummyTexture: MTLTexture
    private let dummyArray: MTLTexture

    init(device: MTLDevice, queue: MTLCommandQueue, dummy: MTLTexture) {
        self.device = device
        self.queue = queue
        self.dummyTexture = dummy
        self.dummyArray = makeWhiteArrayTexture(device: device) ?? dummy
    }

    func build(scene: OpaquePointer, path: String, exposure: Float,
               shadows: Bool) -> SceneGPU? {
        let meshCount = Int(lmv_scene_mesh_count(scene))
        var meshes: [MeshGPU] = []
        meshes.reserveCapacity(meshCount)
        var triangles = 0
        var vertices = 0

        for i in 0..<meshCount {
            var view = lmv_mesh_view()
            guard lmv_scene_mesh(scene, UInt32(i), &view) == LMV_OK,
                  let vertexPtr = view.vertices,
                  let indexPtr = view.indices,
                  view.index_count > 0 else {
                meshes.append(MeshGPU(vertices: device.makeBuffer(length: 48)!,
                                      faceIds: device.makeBuffer(length: 4)!,
                                      indices: device.makeBuffer(length: 4)!,
                                      indexCount: 0, doubleSided: false, subsets: []))
                continue
            }
            let vertexBytes = Int(view.vertex_count) * MemoryLayout<Vertex>.stride
            guard let vbuf = device.makeBuffer(bytes: vertexPtr, length: max(vertexBytes, 48), options: .storageModeShared),
                  let ibuf = device.makeBuffer(bytes: indexPtr, length: Int(view.index_count) * 4, options: .storageModeShared) else {
                return nil
            }
            vbuf.label = "lmv.vertices"
            ibuf.label = "lmv.indices"
            var subsets: [(Int, Int, Int)] = []
            if let subsetPtr = view.subsets {
                for s in 0..<Int(view.subset_count) {
                    let subset = subsetPtr[s]
                    subsets.append((Int(subset.first_index), Int(subset.index_count), Int(subset.material_id)))
                }
            }
            if subsets.isEmpty {
                subsets.append((0, Int(view.index_count), Int(view.material_id)))
            }
            triangles += Int(view.index_count) / 3
            vertices += Int(view.vertex_count)
            let faceCount = Int(view.vertex_count)
            var faceIds = [UInt32](repeating: 0, count: max(faceCount, 1))
            if let source = view.face_ids {
                for i in 0..<faceCount { faceIds[i] = source[i] }
            }
            guard let fbuf = device.makeBuffer(bytes: faceIds,
                                               length: faceIds.count * 4,
                                               options: .storageModeShared) else {
                return nil
            }
            fbuf.label = "lmv.faceIds"

            var mesh = MeshGPU(vertices: vbuf, faceIds: fbuf, indices: ibuf,
                               indexCount: Int(view.index_count),
                               doubleSided: view.double_sided != 0,
                               subsets: subsets)
            if view.skinned != 0 {
                mesh.skinned = true
                var count: UInt32 = 0
                if let source = lmv_scene_mesh_joint_indices(scene, UInt32(i), &count),
                   count > 0 {
                    let length = (Int(count) * MemoryLayout<UInt16>.stride + 3) & ~3
                    mesh.jointIndices = device.makeBuffer(bytes: source,
                                                          length: max(length, 4),
                                                          options: .storageModeShared)
                    mesh.jointIndices?.label = "lmv.jointIndices"
                }
                count = 0
                if let source = lmv_scene_mesh_joint_weights(scene, UInt32(i), &count),
                   count > 0 {
                    mesh.jointWeights = device.makeBuffer(bytes: source,
                                                         length: Int(count) * 4,
                                                         options: .storageModeShared)
                    mesh.jointWeights?.label = "lmv.jointWeights"
                }
                count = 0
                if let source = lmv_scene_mesh_skin_matrices(scene, UInt32(i), &count),
                   count > 0 {
                    mesh.skinMatrices = device.makeBuffer(bytes: source,
                                                          length: Int(count) * 64,
                                                          options: .storageModeShared)
                    mesh.skinMatrices?.label = "lmv.skinMatrices"
                }
                if mesh.jointIndices == nil || mesh.jointWeights == nil ||
                    mesh.skinMatrices == nil {
                    // Leave the bind pose rather than binding a nil buffer.
                    mesh.skinned = false
                }
            }
            let morphTargets = Int(view.morph_targets)
            if morphTargets > 0 {
                let vertexCount = Int(view.vertex_count)
                var interleaved = [Float](repeating: 0,
                                          count: vertexCount * morphTargets * 3)
                for target in 0..<morphTargets {
                    var count: UInt32 = 0
                    guard let source = lmv_scene_mesh_morph_deltas(
                        scene, UInt32(i), UInt32(target), &count),
                        count >= UInt32(vertexCount * 3) else { continue }
                    for v in 0..<vertexCount {
                        let dst = (v * morphTargets + target) * 3
                        let src = v * 3
                        interleaved[dst + 0] = source[src + 0]
                        interleaved[dst + 1] = source[src + 1]
                        interleaved[dst + 2] = source[src + 2]
                    }
                }
                if !interleaved.isEmpty,
                   let buffer = device.makeBuffer(
                    bytes: interleaved, length: interleaved.count * 4,
                    options: .storageModeShared) {
                    mesh.morphDeltas = buffer
                    buffer.label = "lmv.morphDeltas"
                    mesh.morphTargets = morphTargets
                }
                var weightCount: UInt32 = 0
                if let weights = lmv_scene_mesh_morph_weights(scene, UInt32(i),
                                                              &weightCount),
                   weightCount >= UInt32(morphTargets) {
                    mesh.morphWeights = device.makeBuffer(
                        bytes: weights, length: Int(weightCount) * 4,
                        options: .storageModeShared)
                    mesh.morphWeights?.label = "lmv.morphWeights"
                } else {
                    // Never let the shader read past the weights buffer.
                    mesh.morphTargets = 0
                }
            }
            meshes.append(mesh)
        }

        // Draws (one per visible mesh node).
        let drawCount = Int(lmv_scene_draw_count(scene))
        var draws: [DrawGPU] = []
        draws.reserveCapacity(drawCount)
        for i in 0..<drawCount {
            var view = lmv_draw_view()
            guard lmv_scene_draw(scene, UInt32(i), &view) == LMV_OK,
                  let world = view.world,
                  view.mesh_id >= 0, Int(view.mesh_id) < meshes.count else { continue }
            guard meshes[Int(view.mesh_id)].indexCount > 0 else { continue }
            draws.append(DrawGPU(mesh: Int(view.mesh_id),
                                 materialOverride: Int(view.material_override),
                                 world: mat4(world),
                                 visible: view.visible != 0,
                                 coreIndex: i))
        }

        // Images -> Metal textures (decoded by the core).
        var imageTextures: [Int: MTLTexture] = [:]
        let imageCount = Int(lmv_scene_image_count(scene))
        var envImageID = -1
        let lightCountRaw = Int(lmv_scene_light_count(scene))
        for i in 0..<lightCountRaw {
            var light = lmv_light_view()
            if lmv_scene_light(scene, UInt32(i), &light) == LMV_OK,
               light.type == UInt32(LMV_LIGHT_DOME.rawValue),
               light.dome_texture_id >= 0 {
                var tex = lmv_texture_view()
                if lmv_scene_texture(scene, UInt32(light.dome_texture_id), &tex) == LMV_OK,
                   tex.image_id >= 0 {
                    envImageID = Int(tex.image_id)
                    break
                }
            }
        }

        for imageID in 0..<imageCount {
            var image = lmv_image_view()
            guard lmv_scene_image(scene, UInt32(imageID), &image) == LMV_OK,
                  image.width > 0, image.height > 0,
                  image.compressed_format != 0 || image.pixels != nil else {
                continue
            }
            let isEnv = imageID == envImageID
            if let texture = makeTexture(image: image, pixels: image.pixels,
                                         isEnv: isEnv) {
                imageTextures[imageID] = texture
            }
        }

        // UDIM textures: one texture2DArray per UDIM texture plus a 10x10 LUT
        // (per texture) mapping tile id -> array slice.
        let textureCount = Int(lmv_scene_texture_count(scene))
        var udimArrays: [Int: MTLTexture] = [:]
        var lut = [Int32](repeating: -1, count: Swift.max(textureCount, 1) * 100)
        for t in 0..<textureCount {
            var texture = lmv_texture_view()
            guard lmv_scene_texture(scene, UInt32(t), &texture) == LMV_OK,
                  texture.udim_tile_count > 0,
                  let tilePtr = texture.udim_tiles else { continue }
            var tiles: [lmv_udim_tile] = []
            tiles.reserveCapacity(Int(texture.udim_tile_count))
            for i in 0..<Int(texture.udim_tile_count) {
                tiles.append(tilePtr[i])
            }
            guard let array = makeUdimArray(scene: scene, tiles: tiles) else { continue }
            udimArrays[t] = array
            for (slice, tile) in tiles.enumerated() {
                let index = Int(tile.udim) - 1001
                if index >= 0 && index < 100 {
                    lut[t * 100 + index] = Int32(slice)
                }
            }
        }
        let lutBuffer = device.makeBuffer(bytes: lut, length: lut.count * 4,
                                          options: .storageModeShared)
            ?? device.makeBuffer(length: 4, options: .storageModeShared)!

        // Ptex rect tables: one normalized float4 per face, concatenated for
        // every Ptex texture. Slot uniforms carry the table offset.
        var ptexSlots: [Int: Int] = [:]
        var ptexRects: [SIMD4<Float>] = []
        for t in 0..<textureCount {
            var texture = lmv_texture_view()
            guard lmv_scene_texture(scene, UInt32(t), &texture) == LMV_OK,
                  texture.is_ptex != 0,
                  let rects = texture.ptex_rects else { continue }
            ptexSlots[t] = ptexRects.count
            for i in 0..<Int(texture.ptex_face_count) {
                let r = rects[i]
                ptexRects.append(SIMD4<Float>(r.x, r.y, r.w, r.h))
            }
        }
        if ptexRects.isEmpty {
            ptexRects.append(SIMD4<Float>(0, 0, 0, 0))
        }
        let ptexBuffer = device.makeBuffer(bytes: ptexRects,
                                            length: ptexRects.count * 16,
                                            options: .storageModeShared)
            ?? device.makeBuffer(length: 16, options: .storageModeShared)!

        // Materials.
        let materialCount = Int(lmv_scene_material_count(scene))
        var materials: [MaterialGPU] = []
        materials.reserveCapacity(materialCount)
        for i in 0..<materialCount {
            materials.append(buildMaterial(scene: scene, index: i,
                                           imageTextures: imageTextures,
                                           udimArrays: udimArrays,
                                           ptexSlots: ptexSlots))
        }

        // Lights: authored lights win, otherwise a fixed three-point rig.
        let authored = buildAuthoredLights(scene: scene)
        let lights = authored.isEmpty ? studioRig() : authored
        let authoredCamera = buildAuthoredCamera(scene: scene)

        // Shadow caster: first directional light (authored or studio key).
        var shadowLight = -1
        for (i, light) in lights.enumerated() where Int(light.positionType.w) == 1 {
            shadowLight = i
            break
        }

        var boundsMinF = [Float](repeating: -1, count: 3)
        var boundsMaxF = [Float](repeating: 1, count: 3)
        lmv_scene_bounds(scene, &boundsMinF, &boundsMaxF)
        let boundsMin = SIMD3<Float>(boundsMinF[0], boundsMinF[1], boundsMinF[2])
        let boundsMax = SIMD3<Float>(boundsMaxF[0], boundsMaxF[1], boundsMaxF[2])

        // Working-space -> display-linear Rec.709 matrix.
        var matrixValues = [Float](repeating: 0, count: 9)
        lmv_scene_working_to_display_linear(scene, &matrixValues)
        let colorMatrix = simd_float3x3(
            SIMD3<Float>(matrixValues[0], matrixValues[3], matrixValues[6]),
            SIMD3<Float>(matrixValues[1], matrixValues[4], matrixValues[7]),
            SIMD3<Float>(matrixValues[2], matrixValues[5], matrixValues[8]))

        // Environment map + SH.
        var envTexture: MTLTexture? = nil
        var sh = [SIMD4<Float>](repeating: .zero, count: 9)
        if envImageID >= 0, let envImage = imageTextures[envImageID],
           let floatPixels = readEnvPixels(scene: scene, imageID: envImageID) {
            envTexture = envImage
            sh = floatPixels.withUnsafeBufferPointer { buffer in
                let w = Int(envImage.width)
                let h = Int(envImage.height)
                return projectEquirectSH(width: w, height: h, rgb: buffer.baseAddress!, rotation: 0)
            }
            // SH is a colour signal: move it into display-linear Rec.709 too.
            for i in 0..<sh.count {
                let c = colorMatrix * SIMD3<Float>(sh[i].x, sh[i].y, sh[i].z)
                sh[i] = SIMD4<Float>(c.x, c.y, c.z, sh[i].w)
            }
        }

        let upAxisZ = lmv_scene_up_axis(scene) == 1
        var warnings: [String] = []
        let warningCount = Int(lmv_scene_warning_count(scene))
        for i in 0..<warningCount {
            if let text = lmv_scene_warning(scene, UInt32(i)) {
                let s = String(cString: text)
                if !s.isEmpty { warnings.append(s) }
            }
        }

        // UsdGeomPoints / Gaussian particles (splats) and curve segments.
        let splatCount = Int(lmv_scene_splat_count(scene))
        var splatBuffer: MTLBuffer? = nil
        if splatCount > 0, let source = lmv_scene_splats(scene) {
            var splats = [SplatGPU](repeating: SplatGPU(), count: splatCount)
            for i in 0..<splatCount {
                let s = source[i]
                splats[i] = SplatGPU(x: s.x, y: s.y, z: s.z, radius: s.radius,
                                     r: s.r, g: s.g, b: s.b, a: s.a)
            }
            splatBuffer = device.makeBuffer(bytes: splats,
                                            length: splats.count * MemoryLayout<SplatGPU>.stride,
                                            options: .storageModeShared)
            splatBuffer?.label = "lmv.splats"
        }
        let lineCount = Int(lmv_scene_line_count(scene))
        var lineBuffer: MTLBuffer? = nil
        var lineIndexBuffer: MTLBuffer? = nil
        if lineCount > 0, let source = lmv_scene_lines(scene) {
            var lines = [LineGPU](repeating: LineGPU(), count: lineCount)
            for i in 0..<lineCount {
                let l = source[i]
                lines[i] = LineGPU(p0x: l.p0.0, p0y: l.p0.1, p0z: l.p0.2,
                                   p1x: l.p1.0, p1y: l.p1.1, p1z: l.p1.2,
                                   width: l.width, r: l.r, g: l.g, b: l.b, a: l.a)
            }
            lineBuffer = device.makeBuffer(bytes: lines,
                                           length: lines.count * MemoryLayout<LineGPU>.stride,
                                           options: .storageModeShared)
            lineBuffer?.label = "lmv.lines"
            var indices = [UInt32]()
            indices.reserveCapacity(lineCount * 6)
            for i in 0..<lineCount {
                let base = UInt32(i * 4)
                indices.append(contentsOf: [base, base + 1, base + 3,
                                            base, base + 3, base + 2])
            }
            lineIndexBuffer = device.makeBuffer(bytes: indices,
                                                length: indices.count * 4,
                                                options: .storageModeShared)
            lineIndexBuffer?.label = "lmv.lineIndices"
        }

        return SceneGPU(meshes: meshes, draws: draws, materials: materials,
                        lights: lights, sh: sh, envTexture: envTexture,
                        boundsMin: boundsMin, boundsMax: boundsMax,
                        upAxisZ: upAxisZ, triangleCount: triangles,
                        vertexCount: vertices, warnings: warnings,
                        shadowLightIndex: shadowLight,
                        usesStudioRig: authored.isEmpty,
                        authoredCamera: authoredCamera,
                        timeCode: lmv_scene_time(scene),
                        startTime: lmv_scene_start_time(scene),
                        endTime: lmv_scene_end_time(scene),
                        fps: lmv_scene_frames_per_second(scene),
                        hasAnimation: lmv_scene_has_animation(scene) != 0,
                        filePath: path, colorMatrix: colorMatrix,
                        splatBuffer: splatBuffer, splatCount: splatCount,
                        lineBuffer: lineBuffer,
                        lineIndexBuffer: lineIndexBuffer, lineCount: lineCount,
                        dummy: dummyTexture, dummyArray: dummyArray,
                        udimLut: lutBuffer, ptexRects: ptexBuffer)
    }

    // Snapshot the core's current animation state (node worlds, skin palettes,
    // morph weights) into GPU resources. Called after
    // lmv_scene_update_animation succeeds, so no geometry is rebuilt.
    func buildAnimation(scene: OpaquePointer, base: SceneGPU) -> SceneAnimation? {
        var worlds = base.draws.map { $0.world }
        var visible = base.draws.map { $0.visible }
        var meshes = base.draws.map { $0.mesh }
        for (i, draw) in base.draws.enumerated() {
            var view = lmv_draw_view()
            guard lmv_scene_draw(scene, UInt32(draw.coreIndex), &view) == LMV_OK else {
                continue
            }
            if let world = view.world {
                worlds[i] = mat4(world)
            }
            visible[i] = view.visible != 0
            if view.mesh_id >= 0 && Int(view.mesh_id) < base.meshes.count {
                meshes[i] = Int(view.mesh_id)
            }
        }

        var meshStates: [MeshAnimationGPU] = []
        meshStates.reserveCapacity(base.meshes.count)
        for (i, mesh) in base.meshes.enumerated() {
            var state = MeshAnimationGPU(skinMatrices: mesh.skinMatrices,
                                         jointCount: 0,
                                         morphWeights: mesh.morphWeights,
                                         morphTargets: mesh.morphTargets)
            var view = lmv_mesh_view()
            if lmv_scene_mesh(scene, UInt32(i), &view) == LMV_OK {
                if view.skinned != 0 {
                    var count: UInt32 = 0
                    if let source = lmv_scene_mesh_skin_matrices(
                        scene, UInt32(i), &count),
                       count > 0,
                       let buffer = device.makeBuffer(
                        bytes: source, length: Int(count) * 64,
                        options: .storageModeShared) {
                        state.skinMatrices = buffer
                        state.jointCount = Int(count)
                    }
                }
                if view.morph_targets > 0 {
                    var count: UInt32 = 0
                    if let source = lmv_scene_mesh_morph_weights(
                        scene, UInt32(i), &count),
                       count >= view.morph_targets,
                       let buffer = device.makeBuffer(
                        bytes: source, length: Int(count) * 4,
                        options: .storageModeShared) {
                        state.morphWeights = buffer
                        state.morphTargets = Int(view.morph_targets)
                    } else {
                        state.morphTargets = 0
                    }
                }
            }
            meshStates.append(state)
        }
        // Authored light transforms can animate too; keep the studio rig when
        // the scene has no authored lights.
        let authoredLights = buildAuthoredLights(scene: scene)
        let lights = authoredLights.isEmpty ? base.lights : authoredLights
        let camera = buildAuthoredCamera(scene: scene) ?? base.authoredCamera
        return SceneAnimation(drawWorlds: worlds, drawVisible: visible,
                              drawMeshes: meshes, meshes: meshStates,
                              lights: lights, camera: camera)
    }

    private func buildAuthoredCamera(scene: OpaquePointer) -> AuthoredCamera? {
        guard Int(lmv_scene_camera_count(scene)) > 0 else { return nil }
        var view = lmv_camera_view()
        guard lmv_scene_camera(scene, 0, &view) == LMV_OK,
              let transform = view.transform else { return nil }
        var m = [Float](repeating: 0, count: 16)
        for i in 0..<16 { m[i] = transform[i] }
        return AuthoredCamera(
            name: view.name != nil ? String(cString: view.name) : "Camera",
            transform: m,
            orthographic: view.orthographic != 0,
            focalLength: view.focal_length,
            verticalAperture: view.vertical_aperture,
            orthoWidth: view.ortho_width,
            nearClip: view.near_clip,
            farClip: view.far_clip,
            focusDistance: view.focus_distance,
            exposure: view.exposure,
            fstop: view.fstop)
    }

    // MARK: - Materials

    private func buildMaterial(scene: OpaquePointer, index: Int,
                               imageTextures: [Int: MTLTexture],
                               udimArrays: [Int: MTLTexture],
                               ptexSlots: [Int: Int]) -> MaterialGPU {
        let material = MaterialGPU()
        var view = lmv_material_view()
        guard lmv_scene_material(scene, UInt32(index), &view) == LMV_OK,
              let pbr = view.pbr else { return material }
        let p = UnsafeBufferPointer(start: pbr, count: Int(LMV_PBR_FLOATS))
        func v(_ i: Int) -> Float { i < p.count ? p[i] : 0 }
        func rgb(_ i: Int) -> SIMD3<Float> { SIMD3<Float>(v(i), v(i + 1), v(i + 2)) }

        var u = MaterialUniformsSwift()
        u.baseColor = SIMD4<Float>(rgb(PbrIndex.baseColor), v(PbrIndex.baseWeight))
        u.emission = SIMD4<Float>(rgb(PbrIndex.emissionColor), v(PbrIndex.emission))
        u.specular = SIMD4<Float>(rgb(PbrIndex.specularColor), v(PbrIndex.specularWeight))
        u.coat = SIMD4<Float>(rgb(PbrIndex.coatColor), v(PbrIndex.coatWeight))
        u.sheen = SIMD4<Float>(rgb(PbrIndex.sheenColor), v(PbrIndex.sheenWeight))
        u.transmission = SIMD4<Float>(rgb(PbrIndex.transmissionColor), v(PbrIndex.transmission))
        u.p0 = SIMD4<Float>(v(PbrIndex.metalness), v(PbrIndex.diffuseRoughness),
                            max(v(PbrIndex.specularRoughness), 0.02),
                            max(v(PbrIndex.specularIor), 1.0))
        u.p1 = SIMD4<Float>(v(PbrIndex.coatRoughness), max(v(PbrIndex.coatIor), 1.0),
                            v(PbrIndex.sheenRoughness), v(PbrIndex.opacity))
        u.p2 = SIMD4<Float>(Float(view.alpha_mode), view.alpha_cutoff,
                            max(view.normal_scale, 0), v(PbrIndex.thinWalled))
        u.p3 = SIMD4<Float>(max(view.coat_normal_scale, 0), 0, 0, 0)
        u.p4 = SIMD4<Float>(v(48), v(49), max(v(50), 1.0), v(57))
        u.p5 = SIMD4<Float>(v(19), v(15), v(67), v(58))
        u.p6 = SIMD4<Float>(v(16), v(17), v(18), v(23))

        let slots = [
            (view.tex_base_color, true, 4),
            (view.tex_metallic, false, -1),
            (view.tex_roughness, false, -1),
            (view.tex_normal, false, 4),
            (view.tex_emission, true, 4),
            (view.tex_opacity, false, -1),
            (view.tex_coat_normal, false, 4)
        ]
        var slotParams = (SlotParamsSwift(), SlotParamsSwift(), SlotParamsSwift(),
                          SlotParamsSwift(), SlotParamsSwift(), SlotParamsSwift(),
                          SlotParamsSwift())
        for (slotIndex, slot) in slots.enumerated() {
            guard slot.0 >= 0 else { continue }
            var tex = lmv_texture_view()
            guard lmv_scene_texture(scene, UInt32(slot.0), &tex) == LMV_OK else { continue }
            var params = SlotParamsSwift()
            params.uvOffsetScale = SIMD4<Float>(tex.uv_offset.0, tex.uv_offset.1,
                                                tex.uv_scale.0, tex.uv_scale.1)
            let c = cos(tex.uv_rotation)
            let s = sin(tex.uv_rotation)
            params.uvRot = SIMD4<Float>(c, s, 0, 0)
            params.valueScale = SIMD4<Float>(tex.scale.0, tex.scale.1, tex.scale.2, tex.scale.3)
            params.valueBias = SIMD4<Float>(tex.bias.0, tex.bias.1, tex.bias.2, tex.bias.3)
            var channel = Int32(tex.channel)
            if channel < 0 || channel > 5 {
                channel = slot.2 >= 0 ? Int32(slot.2) : 4
            }
            if slotIndex == 1 || slotIndex == 2 {
                // Scalar maps usually pack R/G/B; honour a single-channel
                // authoring but otherwise read the conventional channel.
                if channel == 4 || channel == 5 {
                    channel = tex.channel == 1 && slotIndex == 2 ? 1 :
                              (slotIndex == 1 ? 2 : 0)
                } else if channel < 0 || channel > 3 {
                    channel = slotIndex == 1 ? 2 : 0
                }
            }
            if slotIndex == 3 || slotIndex == 0 || slotIndex == 4 || slotIndex == 6 {
                channel = 4 // RGB
            }
            var srgb: Int32 = 0
            var image = lmv_image_view()
            if tex.image_id >= 0,
               lmv_scene_image(scene, UInt32(tex.image_id), &image) == LMV_OK {
                srgb = (slot.1 && image.srgb != 0 &&
                        image.compressed_format == 0) ? 1 : 0
                params.meta.w = imageTextures[Int(tex.image_id)] != nil ? 1 : 0
            } else {
                params.meta.w = 0
            }
            params.meta.x = Int32(tex.wrap_s == UInt32(LMV_WRAP_REPEAT.rawValue) ? 0 : 1)
            params.meta.y = srgb
            params.meta.z = channel
            params.udim.w = Int32(tex.uv_set)
            if tex.is_ptex != 0, let offset = ptexSlots[Int(slot.0)] {
                params.ptex = SIMD4<Int32>(1, Int32(offset),
                                           Int32(tex.ptex_face_count), 0)
                params.meta.w = 1
            } else if tex.udim_tile_count > 0, let array = udimArrays[Int(slot.0)] {
                params.udim = SIMD4<Int32>(1, Int32(Int(slot.0) * 100),
                                           Int32(tex.udim_tile_count), 0)
                params.meta.w = 1
                material.arrayTextures[slotIndex] = array
            }
            switch slotIndex {
            case 0: slotParams.0 = params
            case 1: slotParams.1 = params
            case 2: slotParams.2 = params
            case 3: slotParams.3 = params
            case 4: slotParams.4 = params
            case 5: slotParams.5 = params
            default: slotParams.6 = params
            }
            if tex.image_id >= 0, let texture = imageTextures[Int(tex.image_id)] {
                material.textures[slotIndex] = texture
            }
        }
        u.slots = slotParams
        material.uniforms = u
        return material
    }

    // MARK: - UDIM

    private func makeUdimArray(scene: OpaquePointer,
                               tiles: [lmv_udim_tile]) -> MTLTexture? {
        guard let first = tiles.first else { return nil }
        var info = lmv_image_view()
        guard lmv_scene_image(scene, UInt32(first.image_id), &info) == LMV_OK,
              info.width > 0, info.height > 0 else { return nil }
        let width = Int(info.width)
        let height = Int(info.height)
        let isFloat = info.is_float != 0
        let mipCount = max(1, Int(floor(log2(Double(max(width, height))))) + 1)
        let descriptor = MTLTextureDescriptor()
        descriptor.textureType = .type2DArray
        descriptor.pixelFormat = isFloat ? .rgba16Float : .rgba8Unorm
        descriptor.width = width
        descriptor.height = height
        descriptor.arrayLength = tiles.count
        descriptor.mipmapLevelCount = mipCount
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        guard let texture = device.makeTexture(descriptor: descriptor) else { return nil }
        texture.label = "lmv.udim"

        for (slice, tile) in tiles.enumerated() {
            var image = lmv_image_view()
            guard lmv_scene_image(scene, UInt32(tile.image_id), &image) == LMV_OK,
                  Int(image.width) == width, Int(image.height) == height,
                  image.is_float != 0 ? isFloat : !isFloat,
                  let pixels = image.pixels else {
                return nil
            }
            if isFloat {
                var half = [Float16]()
                half.reserveCapacity(width * height * 4)
                let src = pixels.assumingMemoryBound(to: Float.self)
                for i in 0..<(width * height) {
                    half.append(Float16(src[i * 3 + 0]))
                    half.append(Float16(src[i * 3 + 1]))
                    half.append(Float16(src[i * 3 + 2]))
                    half.append(1)
                }
                half.withUnsafeBytes { bytes in
                    texture.replace(region: MTLRegionMake2D(0, 0, width, height),
                                    mipmapLevel: 0, slice: slice,
                                    withBytes: bytes.baseAddress!,
                                    bytesPerRow: width * 4 * MemoryLayout<Float16>.size,
                                    bytesPerImage: width * height * 4 * MemoryLayout<Float16>.size)
                }
            } else {
                texture.replace(region: MTLRegionMake2D(0, 0, width, height),
                                mipmapLevel: 0, slice: slice,
                                withBytes: pixels, bytesPerRow: width * 4,
                                bytesPerImage: width * height * 4)
            }
        }

        guard let commandBuffer = queue.makeCommandBuffer(),
              let blit = commandBuffer.makeBlitCommandEncoder() else { return texture }
        blit.generateMipmaps(for: texture)
        blit.endEncoding()
        commandBuffer.commit()
        commandBuffer.waitUntilCompleted()
        return texture
    }

    // MARK: - Lights

    private func buildAuthoredLights(scene: OpaquePointer) -> [GPULightSwift] {
        let count = Int(lmv_scene_light_count(scene))
        var lights: [GPULightSwift] = []
        for i in 0..<count {
            var light = lmv_light_view()
            guard lmv_scene_light(scene, UInt32(i), &light) == LMV_OK else { continue }
            let type = Int(light.type)
            if type == Int(LMV_LIGHT_DOME.rawValue) { continue } // handled as IBL
            guard let transform = light.transform else { continue }
            let m = mat4(transform)
            var gpu = GPULightSwift()
            gpu.positionType = SIMD4<Float>(m.columns.3.x, m.columns.3.y,
                                            m.columns.3.z, Float(type))
            let forward = -SIMD3<Float>(m.columns.2.x, m.columns.2.y, m.columns.2.z)
            gpu.direction = SIMD4<Float>(-forward, 0)
            let exposureScale = exp2(light.exposure)
            let intensity = light.intensity * exposureScale
            gpu.colorIntensity = SIMD4<Float>(light.color.0 * intensity,
                                              light.color.1 * intensity,
                                              light.color.2 * intensity, 1)
            switch type {
            case Int(LMV_LIGHT_DIRECTIONAL.rawValue):
                gpu.positionType.w = Float(LMV_LIGHT_DIRECTIONAL.rawValue)
            case Int(LMV_LIGHT_SPOT.rawValue):
                let angle = max(light.param0, 0.001)
                let half = angle * 0.5
                gpu.colorIntensity.w = cos(half)
                gpu.params = SIMD4<Float>(0, cos(half * 0.8), 0, 0)
                gpu.positionType.w = Float(LMV_LIGHT_SPOT.rawValue)
            case Int(LMV_LIGHT_RECT.rawValue), Int(LMV_LIGHT_DISK.rawValue),
                 Int(LMV_LIGHT_SPHERE.rawValue):
                gpu.params = SIMD4<Float>(max(light.param0, 0.001),
                                          max(light.param1, 0.001), 0, 0)
                gpu.positionType.w = Float(LMV_LIGHT_POINT.rawValue)
            default:
                gpu.positionType.w = Float(LMV_LIGHT_POINT.rawValue)
            }
            lights.append(gpu)
        }
        return lights
    }

    private func studioRig() -> [GPULightSwift] {
        func directional(_ dir: SIMD3<Float>, _ color: SIMD3<Float>) -> GPULightSwift {
            var l = GPULightSwift()
            let n = simd_normalize(dir)
            l.positionType = SIMD4<Float>(n * 10, Float(LMV_LIGHT_DIRECTIONAL.rawValue))
            l.direction = SIMD4<Float>(n, 0)
            l.colorIntensity = SIMD4<Float>(color, 1)
            return l
        }
        return [
            directional(SIMD3<Float>(-0.45, 0.60, 0.50), SIMD3<Float>(2.6, 2.5, 2.35)),
            directional(SIMD3<Float>(0.65, 0.18, 0.35), SIMD3<Float>(0.65, 0.7, 0.8)),
            directional(SIMD3<Float>(0.15, -0.25, -0.9), SIMD3<Float>(0.5, 0.55, 0.7))
        ]
    }

    // MARK: - Textures

    private func makeTexture(image: lmv_image_view, pixels: UnsafeRawPointer?,
                             isEnv: Bool) -> MTLTexture? {
        let width = Int(image.width)
        let height = Int(image.height)

        // Block-compressed KTX2: upload the authored BC levels directly.
        if image.compressed_format != 0, let levels = image.levels {
            let format: MTLPixelFormat
            let blockBytes: Int
            let srgb = image.srgb != 0
            switch Int(image.compressed_format) {
            case Int(LMV_COMPRESSED_BC1.rawValue):
                format = srgb ? .bc1_rgba_srgb : .bc1_rgba
                blockBytes = 8
            case Int(LMV_COMPRESSED_BC3.rawValue):
                format = srgb ? .bc3_rgba_srgb : .bc3_rgba
                blockBytes = 16
            case Int(LMV_COMPRESSED_BC5.rawValue):
                format = .bc5_rgUnorm
                blockBytes = 16
            case Int(LMV_COMPRESSED_BC6H.rawValue):
                // The Swift overlay does not expose the BC6H cases; use the
                // Metal enum value directly (MTLPixelFormatBC6H_RGBFloat = 150).
                format = MTLPixelFormat(rawValue: 150) ?? .rgba16Float
                blockBytes = 16
            case Int(LMV_COMPRESSED_BC7.rawValue):
                format = srgb ? .bc7_rgbaUnorm_srgb : .bc7_rgbaUnorm
                blockBytes = 16
            default:
                return nil
            }
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: format, width: width, height: height, mipmapped: true)
            descriptor.mipmapLevelCount = max(1, Int(image.mip_levels))
            descriptor.usage = [.shaderRead]
            descriptor.storageMode = .shared
            guard let texture = device.makeTexture(descriptor: descriptor) else { return nil }
            texture.label = isEnv ? "lmv.env.ktx2" : "lmv.texture.ktx2"
            for level in 0..<Int(image.mip_levels) {
                let info = levels[level]
                guard let data = info.data, info.nbytes > 0 else { return nil }
                let w = Int(info.width)
                let h = Int(info.height)
                let bytesPerRow = max(1, (w + 3) / 4) * blockBytes
                texture.replace(region: MTLRegionMake2D(0, 0, w, h),
                                mipmapLevel: level,
                                withBytes: data, bytesPerRow: bytesPerRow)
            }
            return texture
        }

        guard let pixels else { return nil }
        let mipCount = max(1, Int(floor(log2(Double(max(width, height))))) + 1)
        let isFloat = image.is_float != 0
        let format: MTLPixelFormat = isFloat ? .rgba16Float : .rgba8Unorm
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: width, height: height, mipmapped: true)
        descriptor.mipmapLevelCount = mipCount
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        guard let texture = device.makeTexture(descriptor: descriptor) else { return nil }
        texture.label = isEnv ? "lmv.env" : "lmv.texture"

        if isFloat {
            var half = [Float16]()
            half.reserveCapacity(width * height * 4)
            let src = pixels.assumingMemoryBound(to: Float.self)
            for i in 0..<(width * height) {
                half.append(Float16(src[i * 3 + 0]))
                half.append(Float16(src[i * 3 + 1]))
                half.append(Float16(src[i * 3 + 2]))
                half.append(1)
            }
            half.withUnsafeBytes { bytes in
                texture.replace(region: MTLRegionMake2D(0, 0, width, height),
                                mipmapLevel: 0, withBytes: bytes.baseAddress!,
                                bytesPerRow: width * 4 * MemoryLayout<Float16>.size)
            }
        } else if isEnv && image.srgb != 0 {
            // LDR environments are sampled as linear radiance, so decode sRGB
            // once here instead of in the shader.
            var linear = [UInt8](repeating: 255, count: width * height * 4)
            let src = pixels.assumingMemoryBound(to: UInt8.self)
            for i in 0..<(width * height) {
                for c in 0..<3 {
                    let v = Float(src[i * 4 + c]) / 255
                    let lin = v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4)
                    linear[i * 4 + c] = UInt8(max(0, min(255, lin * 255 + 0.5)))
                }
            }
            linear.withUnsafeBytes { bytes in
                texture.replace(region: MTLRegionMake2D(0, 0, width, height),
                                mipmapLevel: 0, withBytes: bytes.baseAddress!,
                                bytesPerRow: width * 4)
            }
        } else {
            texture.replace(region: MTLRegionMake2D(0, 0, width, height),
                            mipmapLevel: 0, withBytes: pixels,
                            bytesPerRow: width * 4)
        }

        guard let commandBuffer = queue.makeCommandBuffer(),
              let blit = commandBuffer.makeBlitCommandEncoder() else { return texture }
        blit.generateMipmaps(for: texture)
        blit.endEncoding()
        commandBuffer.commit()
        commandBuffer.waitUntilCompleted()
        return texture
    }

    private func readEnvPixels(scene: OpaquePointer, imageID: Int) -> [Float]? {
        var image = lmv_image_view()
        guard lmv_scene_image(scene, UInt32(imageID), &image) == LMV_OK,
              let pixels = image.pixels, image.width > 0, image.height > 0 else {
            return nil
        }
        let count = Int(image.width) * Int(image.height) * 3
        if image.is_float != 0 {
            let src = pixels.assumingMemoryBound(to: Float.self)
            return Array(UnsafeBufferPointer(start: src, count: count))
        }
        // LDR environment: widen to linear float.
        let src = pixels.assumingMemoryBound(to: UInt8.self)
        let srgb = image.srgb != 0
        var out = [Float](repeating: 0, count: count)
        for i in 0..<(Int(image.width) * Int(image.height)) {
            for c in 0..<3 {
                var value = Float(src[i * 4 + c]) / 255
                if srgb {
                    value = value <= 0.04045 ? value / 12.92 :
                        pow((value + 0.055) / 1.055, 2.4)
                }
                out[i * 3 + c] = value
            }
        }
        return out
    }
}
