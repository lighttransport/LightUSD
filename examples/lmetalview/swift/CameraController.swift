// SPDX-License-Identifier: Apache-2.0
//
// lmetalview - Swift wrapper over the shared C++ orbit camera
// (examples/common/light3d/camera-nav.hh). Navigation, framing and automatic
// near/far clipping therefore match lusdview exactly.

import Foundation
import simd
import LMV

final class CameraController {
    /// Called when the user navigates away from an authored camera. `useAuthoredCamera`
    /// does not fire it, so playback can keep following an animated camera.
    var onUserNavigate: (() -> Void)?
    private var handle: OpaquePointer?
    private var zUp = false

    init?() {
        guard let camera = lmv_camera_create() else { return nil }
        handle = camera
    }

    deinit {
        if let handle { lmv_camera_destroy(handle) }
    }

    func setAspect(_ aspect: Float) {
        guard let handle else { return }
        lmv_camera_set_aspect(handle, max(aspect, 0.01))
    }

    func setUpAxis(zUp: Bool) {
        self.zUp = zUp
        guard let handle else { return }
        lmv_camera_set_up_axis(handle, zUp ? 1 : 0)
    }

    func setFovY(degrees: Float) {
        guard let handle else { return }
        lmv_camera_set_fov_y(handle, degrees)
    }

    func setExposure(_ stops: Float) {
        guard let handle else { return }
        lmv_camera_set_exposure(handle, stops)
    }

    func setOrthographic(_ on: Bool) {
        if let handle { lmv_camera_set_orthographic(handle, on ? 1 : 0) }
    }

    func setPreset(_ index: Int) {
        onUserNavigate?()
        if let handle { lmv_camera_set_preset(handle, Int32(index)) }
    }

    /// Frame the bounds with a little breathing room, like a Quick Look
    /// preview: the shared camera fits tightly (good for inspection in
    /// lusdview), so the viewer pulls the eye back by `padding`.
    func frameBounds(min: SIMD3<Float>, max: SIMD3<Float>, padding: Float = 1.35) {
        guard let handle else { return }
        onUserNavigate?()
        var lo = [min.x, min.y, min.z]
        var hi = [max.x, max.y, max.z]
        lmv_camera_frame_bounds(handle, &lo, &hi)
        var target = [Float](repeating: 0, count: 3)
        var yaw: Float = 0
        var pitch: Float = 0
        var distance: Float = 0
        lmv_camera_get_orbit(handle, &target, &yaw, &pitch, &distance)
        // Flat assets (a ground plane, a decal sheet) are unreadable from the
        // default three-quarter elevation; tilt toward a top view instead.
        let extent = max - min
        let horizontal = zUp ? Swift.max(extent.x, extent.y) : Swift.max(extent.x, extent.z)
        let vertical = zUp ? extent.z : extent.y
        if vertical < horizontal * 0.2 && pitch < 0.65 && horizontal > 1e-5 {
            pitch = 0.8
        }
        if distance > 0 {
            lmv_camera_set_orbit(handle, &target, yaw, pitch, distance * padding)
        }
    }

    func orbit(dx: Float, dy: Float) {
        onUserNavigate?()
        if let handle { lmv_camera_orbit(handle, dx, dy) }
    }

    func pan(dx: Float, dy: Float) {
        onUserNavigate?()
        if let handle { lmv_camera_pan(handle, dx, dy) }
    }

    func dolly(_ amount: Float) {
        onUserNavigate?()
        if let handle { lmv_camera_dolly(handle, amount) }
    }

    func useAuthoredCamera(_ authored: AuthoredCamera) {
        guard let handle else { return }
        let transform = authored.transform
        transform.withUnsafeBufferPointer { buffer in
            var view = lmv_camera_view()
            view.transform = buffer.baseAddress
            view.orthographic = authored.orthographic ? 1 : 0
            view.focal_length = authored.focalLength
            view.vertical_aperture = authored.verticalAperture
            view.ortho_width = authored.orthoWidth
            view.near_clip = authored.nearClip
            view.far_clip = authored.farClip
            view.focus_distance = authored.focusDistance
            view.exposure = authored.exposure
            view.fstop = authored.fstop
            lmv_camera_set_from_camera(handle, &view)
        }
    }

    func frame() -> lmv_camera_frame {
        var frame = lmv_camera_frame()
        if let handle {
            lmv_camera_get_frame(handle, &frame)
        }
        return frame
    }
}
