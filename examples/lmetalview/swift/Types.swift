// SPDX-License-Identifier: Apache-2.0
//
// lmetalview - GPU-side layout and small math helpers.
//
// Every struct here mirrors a struct in Shaders.swift; the C core's ABI is in
// core/lmv.h. Layouts are asserted at startup (see Renderer.validateLayouts) so
// a shader/host mismatch fails loudly instead of rendering garbage.

import Foundation
import simd

// Mirrors lmv_vertex (core/lmv.h) and LMVVertex (Shaders.swift): 48 bytes.
struct Vertex {
    var px: Float = 0
    var py: Float = 0
    var pz: Float = 0
    var nx: Float = 0
    var ny: Float = 1
    var nz: Float = 0
    var u: Float = 0
    var v: Float = 0
    var u1: Float = 0
    var v1: Float = 0
    var r: Float = 1
    var g: Float = 1
    var b: Float = 1
    var a: Float = 1
}

struct SlotParamsSwift {
    var uvOffsetScale = SIMD4<Float>(0, 0, 1, 1)
    var uvRot = SIMD4<Float>(1, 0, 0, 0)
    var valueScale = SIMD4<Float>(1, 1, 1, 1)
    var valueBias = SIMD4<Float>(0, 0, 0, 0)
    var meta = SIMD4<Int32>(1, 0, 4, 0)  // wrap, srgb, channel, valid
    var udim = SIMD4<Int32>(0, 0, 0, 0)  // is_udim, LUT offset, tile count, -
    var ptex = SIMD4<Int32>(0, 0, 0, 0)  // is_ptex, rect offset, face count, -
}

struct MaterialUniformsSwift {
    var baseColor = SIMD4<Float>(0.8, 0.8, 0.8, 1)
    var emission = SIMD4<Float>(0, 0, 0, 0)
    var specular = SIMD4<Float>(1, 1, 1, 1)
    var coat = SIMD4<Float>(1, 1, 1, 0)
    var sheen = SIMD4<Float>(1, 1, 1, 0)
    var transmission = SIMD4<Float>(1, 1, 1, 0)
    var p0 = SIMD4<Float>(0, 0, 0.5, 1.5)
    var p1 = SIMD4<Float>(0.1, 1.5, 0.3, 1)
    var p2 = SIMD4<Float>(0, 0.5, 1, 0)
    var p3 = SIMD4<Float>(1, 0, 0, 0)
    var p4 = SIMD4<Float>(0, 0, 1.5, 0)   // thin film weight/nm/ior, anisotropy
    var p5 = SIMD4<Float>(0, 0, 0, 0)    // subsurface, transmission depth, coat affect color, rotation
    var p6 = SIMD4<Float>(0.8, 0.8, 0.8, 1) // subsurface color rgb, scale
    var unused0 = SIMD4<Int32>(-1, -1, -1, -1)
    var unused1 = SIMD4<Int32>(-1, -1, -1, -1)
    var slots: (SlotParamsSwift, SlotParamsSwift, SlotParamsSwift,
                SlotParamsSwift, SlotParamsSwift, SlotParamsSwift,
                SlotParamsSwift) = (
        SlotParamsSwift(), SlotParamsSwift(), SlotParamsSwift(),
        SlotParamsSwift(), SlotParamsSwift(), SlotParamsSwift(),
        SlotParamsSwift()
    )
}

struct DrawUniformsSwift {
    var world = matrix_identity_float4x4
    var normalMat = matrix_identity_float4x4
    var params = SIMD4<Float>(0, 0, 0, 0)
}

struct GPULightSwift {
    var positionType = SIMD4<Float>(0, 0, 0, 0)
    var colorIntensity = SIMD4<Float>(1, 1, 1, 1)
    var direction = SIMD4<Float>(0, 1, 0, 0)
    var params = SIMD4<Float>(0, 0, 0, 0)
}

struct FrameUniformsSwift {
    var view = matrix_identity_float4x4
    var proj = matrix_identity_float4x4
    var invViewProj = matrix_identity_float4x4
    var shadowMatrix = matrix_identity_float4x4
    var cameraPos = SIMD4<Float>(0, 0, 1, 0)
    var shadowParams = SIMD4<Float>(0, 1.0 / 2048.0, 0.002, 0.7)
    var envParams = SIMD4<Float>(0, 1, 0, 0)
    var toneParams = SIMD4<Float>(0, 0, 0, 0)
    var sh0 = SIMD4<Float>(0, 0, 0, 0)
    var sh1 = SIMD4<Float>(0, 0, 0, 0)
    var sh2 = SIMD4<Float>(0, 0, 0, 0)
    var sh3 = SIMD4<Float>(0, 0, 0, 0)
    var sh4 = SIMD4<Float>(0, 0, 0, 0)
    var sh5 = SIMD4<Float>(0, 0, 0, 0)
    var sh6 = SIMD4<Float>(0, 0, 0, 0)
    var sh7 = SIMD4<Float>(0, 0, 0, 0)
    var sh8 = SIMD4<Float>(0, 0, 0, 0)
    var misc = SIMD4<Float>(0, 0, 0, 0)
    var gridParams = SIMD4<Float>(1, 20, 0, 0)
    var gridCenter = SIMD4<Float>(0, 0, 0, 0)
    var flags = SIMD4<Float>(0, 0, 0, 0)
    var cm0 = SIMD4<Float>(1, 0, 0, 0)
    var cm1 = SIMD4<Float>(0, 1, 0, 0)
    var cm2 = SIMD4<Float>(0, 0, 1, 0)
    var shadowMatrices: (float4x4, float4x4, float4x4, float4x4) = (
        matrix_identity_float4x4, matrix_identity_float4x4,
        matrix_identity_float4x4, matrix_identity_float4x4)
    var shadowFlags = SIMD4<Float>(0, 0, 0, 0)
}

// Mirrors Shaders.swift `GridVertex` (same packed layout as Vertex).
typealias GridVertex = Vertex

// Mirrors Shaders.swift `LMVSplat` (32 bytes) and `LMVLineSegment` (44 bytes).
struct SplatGPU {
    var x: Float = 0
    var y: Float = 0
    var z: Float = 0
    var radius: Float = 0
    var r: Float = 1
    var g: Float = 1
    var b: Float = 1
    var a: Float = 1
}

struct LineGPU {
    var p0x: Float = 0
    var p0y: Float = 0
    var p0z: Float = 0
    var p1x: Float = 0
    var p1y: Float = 0
    var p1z: Float = 0
    var width: Float = 0
    var r: Float = 1
    var g: Float = 1
    var b: Float = 1
    var a: Float = 1
}

// ---------------------------------------------------------------------------
// Matrix helpers
// ---------------------------------------------------------------------------

@inline(__always)
func mat4(_ m: UnsafePointer<Float>) -> float4x4 {
    float4x4(columns: (
        SIMD4<Float>(m[0], m[1], m[2], m[3]),
        SIMD4<Float>(m[4], m[5], m[6], m[7]),
        SIMD4<Float>(m[8], m[9], m[10], m[11]),
        SIMD4<Float>(m[12], m[13], m[14], m[15])
    ))
}

@inline(__always)
func normalMatrix(_ world: float4x4) -> float4x4 {
    // Inverse-transpose of the upper 3x3, padded to a float4x4.
    let upper = float3x3(
        SIMD3<Float>(world.columns.0.x, world.columns.0.y, world.columns.0.z),
        SIMD3<Float>(world.columns.1.x, world.columns.1.y, world.columns.1.z),
        SIMD3<Float>(world.columns.2.x, world.columns.2.y, world.columns.2.z)
    )
    let det = upper.determinant
    let inv = det != 0 ? upper.inverse.transpose : matrix_identity_float3x3
    return float4x4(columns: (
        SIMD4<Float>(inv.columns.0.x, inv.columns.0.y, inv.columns.0.z, 0),
        SIMD4<Float>(inv.columns.1.x, inv.columns.1.y, inv.columns.1.z, 0),
        SIMD4<Float>(inv.columns.2.x, inv.columns.2.y, inv.columns.2.z, 0),
        SIMD4<Float>(0, 0, 0, 1)
    ))
}

// ---------------------------------------------------------------------------
// Environment: equirectangular float image -> irradiance SH9
// ---------------------------------------------------------------------------

func projectEquirectSH(width: Int, height: Int, rgb: UnsafePointer<Float>,
                       rotation: Float) -> [SIMD4<Float>] {
    var sh = [SIMD4<Float>](repeating: SIMD4<Float>(0, 0, 0, 0), count: 9)
    guard width > 1, height > 1 else { return sh }
    let step = max(1, min(width, height) / 128)
    let pi = Float.pi
    let invW = 1.0 / Float(width)
    let invH = 1.0 / Float(height)
    var accumulated = [SIMD3<Float>](repeating: SIMD3<Float>(0, 0, 0), count: 9)
    var y = 0
    while y < height {
        let v = (Float(y) + 0.5) * invH
        let theta = (0.5 - v) * pi  // elevation
        let cosTheta = cos(theta)
        let sinTheta = sin(theta)
        let dOmega = (2 * pi * invW) * (pi * invH) * max(cosTheta, 0.001)
        var x = 0
        while x < width {
            let u = (Float(x) + 0.5) * invW
            let phi = (u - 0.5) * 2 * pi + rotation
            let dir = SIMD3<Float>(cosTheta * cos(phi), sinTheta,
                                   cosTheta * sin(phi))
            let base = (y * width + x) * 3
            let radiance = SIMD3<Float>(max(rgb[base], 0), max(rgb[base + 1], 0),
                                        max(rgb[base + 2], 0)) * dOmega
            let dx = dir.x, dy = dir.y, dz = dir.z
            accumulated[0] += radiance * 0.282095
            accumulated[1] += radiance * (0.488603 * dy)
            accumulated[2] += radiance * (0.488603 * dz)
            accumulated[3] += radiance * (0.488603 * dx)
            accumulated[4] += radiance * (1.092548 * dx * dy)
            accumulated[5] += radiance * (1.092548 * dy * dz)
            accumulated[6] += radiance * (0.315392 * (3 * dz * dz - 1))
            accumulated[7] += radiance * (1.092548 * dx * dz)
            accumulated[8] += radiance * (0.546274 * (dx * dx - dy * dy))
            x += step
        }
        y += step
    }
    let factors: [Float] = [Float.pi, 2 * Float.pi / 3, 2 * Float.pi / 3,
                            2 * Float.pi / 3, Float.pi / 4, Float.pi / 4,
                            Float.pi / 4, Float.pi / 4, Float.pi / 4]
    for i in 0..<9 {
        let c = accumulated[i] * factors[i]
        sh[i] = SIMD4<Float>(c.x, c.y, c.z, 0)
    }
    return sh
}

func equirectDirection(u: Float, v: Float) -> SIMD3<Float> {
    let phi = (u - 0.5) * 2 * Float.pi
    let theta = (0.5 - v) * Float.pi
    return SIMD3<Float>(cos(theta) * cos(phi), sin(theta), cos(theta) * sin(phi))
}
