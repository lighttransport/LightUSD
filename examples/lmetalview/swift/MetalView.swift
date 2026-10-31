// SPDX-License-Identifier: Apache-2.0
//
// lmetalview - MTKView with lusdview-compatible navigation and file drops.

import AppKit
import MetalKit

final class MetalView: MTKView {
    weak var controller: ViewerController?
    private var lastDrag: NSPoint?

    override var acceptsFirstResponder: Bool { true }
    override var isOpaque: Bool { true }

    override init(frame frameRect: CGRect, device: MTLDevice?) {
        super.init(frame: frameRect, device: device)
        registerForDraggedTypes([.fileURL])
        wantsLayer = true
        layerContentsRedrawPolicy = .duringViewResize
    }

    required init(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        window?.makeFirstResponder(self)
        window?.acceptsMouseMovedEvents = true
    }

    // MARK: - Mouse navigation

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        if event.clickCount == 2 {
            controller?.frameAll()
            return
        }
        lastDrag = event.locationInWindow
    }

    override func mouseDragged(with event: NSEvent) {
        let dx = Float(event.deltaX)
        let dy = Float(event.deltaY)
        controller?.camera.orbit(dx: dx, dy: dy)
        controller?.requestRedraw()
    }

    override func mouseUp(with event: NSEvent) { lastDrag = nil }

    override func rightMouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        lastDrag = event.locationInWindow
    }

    override func rightMouseDragged(with event: NSEvent) {
        controller?.camera.pan(dx: Float(event.deltaX), dy: Float(event.deltaY))
        controller?.requestRedraw()
    }

    override func otherMouseDown(with event: NSEvent) { lastDrag = event.locationInWindow }

    override func otherMouseDragged(with event: NSEvent) {
        controller?.camera.pan(dx: Float(event.deltaX), dy: Float(event.deltaY))
        controller?.requestRedraw()
    }

    override func scrollWheel(with event: NSEvent) {
        let scale: Float = event.hasPreciseScrollingDeltas ? 0.08 : 0.6
        controller?.camera.dolly(Float(event.scrollingDeltaY) * scale)
        controller?.requestRedraw()
    }

    override func magnify(with event: NSEvent) {
        controller?.camera.dolly(Float(event.magnification) * 4.0)
        controller?.requestRedraw()
    }

    // MARK: - Keyboard

    override func keyDown(with event: NSEvent) {
        if controller?.handleKey(event) == true { return }
        super.keyDown(with: event)
    }

    // MARK: - Drag & drop

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        return accepts(sender) ? .copy : []
    }

    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        return accepts(sender) ? .copy : []
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        guard let url = fileURL(from: sender) else { return false }
        controller?.open(url: url)
        return true
    }

    private func accepts(_ info: NSDraggingInfo) -> Bool {
        fileURL(from: info) != nil
    }

    private func fileURL(from info: NSDraggingInfo) -> URL? {
        let options: [NSPasteboard.ReadingOptionKey: Any] = [
            .urlReadingFileURLsOnly: true,
            .urlReadingContentsConformToTypes: [
                "com.pixar.universal-scene-description",
                "com.pixar.universal-scene-description-mobile",
                "public.usdz",
                "public.data"
            ]
        ]
        guard let items = info.draggingPasteboard.readObjects(
            forClasses: [NSURL.self], options: options) as? [URL] else {
            return nil
        }
        return items.first(where: { ViewerController.isSupported(path: $0.path) })
    }
}
