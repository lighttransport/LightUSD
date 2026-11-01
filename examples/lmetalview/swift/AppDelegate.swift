// SPDX-License-Identifier: Apache-2.0
//
// lmetalview - application delegate: window, menus, file open.

import AppKit
import Metal
import MetalKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: NSWindow!
    private var controller: ViewerController!
    private let pendingPath: String?

    init(initialPath: String?) {
        self.pendingPath = initialPath
        super.init()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        guard let device = MTLCreateSystemDefaultDevice() else {
            fatalError("lmetalview requires a Metal-capable device")
        }
        guard let controller = ViewerController(device: device) else {
            fatalError("could not create the Metal renderer")
        }
        if let error = controller.renderer.loadLibrary() {
            let alert = NSAlert()
            alert.messageText = "Metal shader compilation failed"
            alert.informativeText = error
            alert.runModal()
            NSApp.terminate(nil)
            return
        }
        if let error = controller.renderer.validateLayouts() {
            let alert = NSAlert()
            alert.messageText = "GPU ABI mismatch"
            alert.informativeText = error
            alert.runModal()
            NSApp.terminate(nil)
            return
        }
        self.controller = controller

        let contentRect = NSRect(x: 0, y: 0, width: 1180, height: 760)
        let window = NSWindow(contentRect: contentRect,
                              styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
        window.title = "lmetalview"
        window.minSize = NSSize(width: 480, height: 320)
        window.center()
        window.tabbingMode = .disallowed

        let view = MetalView(frame: contentRect, device: device)
        view.colorPixelFormat = .bgra8Unorm
        view.depthStencilPixelFormat = .depth32Float
        view.sampleCount = device.supportsTextureSampleCount(4) ? 4 : 1
        view.clearColor = MTLClearColor(red: 0.08, green: 0.085, blue: 0.095, alpha: 1)
        view.delegate = controller.renderer
        view.controller = controller
        // Render on demand: interactions and loading mark the view dirty.
        view.isPaused = true
        view.enableSetNeedsDisplay = true
        view.preferredFramesPerSecond = 60
        controller.metalView = view

        let hud = controller.hud
        hud.translatesAutoresizingMaskIntoConstraints = true
        hud.frame = view.bounds
        hud.autoresizingMask = [.width, .height]
        view.addSubview(hud)

        window.contentView = view
        window.makeFirstResponder(view)
        self.window = window
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)

        buildMenu()

        if let path = pendingPath {
            controller.load(path: path)
        }
    }

    func application(_ sender: NSApplication, openFile filename: String) -> Bool {
        controller?.load(path: filename)
        return true
    }

    func application(_ sender: NSApplication, openFiles filenames: [String]) {
        if let first = filenames.first { controller?.load(path: first) }
        sender.reply(toOpenOrPrint: .success)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    // MARK: - Menu actions

    @objc private func openDocument(_ sender: Any?) { controller.openPanel() }
    @objc private func reloadDocument(_ sender: Any?) { controller.reload() }
    @objc private func saveScreenshot(_ sender: Any?) { controller.saveScreenshot() }

    @objc private func frameAll(_ sender: Any?) { controller.frameAll() }
    @objc private func useAuthoredCamera(_ sender: Any?) { controller.useAuthoredCamera() }
    @objc private func toggleWireframe(_ sender: Any?) { controller.toggleWireframe() }
    @objc private func toggleGrid(_ sender: Any?) { controller.toggleGrid() }
    @objc private func toggleShadows(_ sender: Any?) { controller.toggleShadows() }
    @objc private func toggleBackground(_ sender: Any?) { controller.toggleBackground() }
    @objc private func resetExposure(_ sender: Any?) { controller.resetExposure() }
    @objc private func togglePlay(_ sender: Any?) { controller.togglePlay() }

    @objc private func viewPreset(_ sender: NSMenuItem) {
        controller.camera.setPreset(sender.tag)
        controller.requestRedraw()
    }

    private func buildMenu() {
        let main = NSMenu()

        let appItem = NSMenuItem()
        main.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About lmetalview",
                        action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)),
                        keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Hide lmetalview",
                        action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        appMenu.addItem(withTitle: "Quit lmetalview",
                        action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu

        let fileItem = NSMenuItem()
        main.addItem(fileItem)
        let fileMenu = NSMenu(title: "File")
        fileMenu.addItem(withTitle: "Open…", action: #selector(openDocument(_:)),
                         keyEquivalent: "o").target = self
        fileMenu.addItem(withTitle: "Reload", action: #selector(reloadDocument(_:)),
                         keyEquivalent: "r").target = self
        fileMenu.addItem(.separator())
        fileMenu.addItem(withTitle: "Save Screenshot…",
                         action: #selector(saveScreenshot(_:)),
                         keyEquivalent: "s").target = self
        fileMenu.addItem(.separator())
        fileMenu.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)),
                         keyEquivalent: "w")
        fileItem.submenu = fileMenu

        let viewItem = NSMenuItem()
        main.addItem(viewItem)
        let viewMenu = NSMenu(title: "View")
        viewMenu.addItem(withTitle: "Frame All", action: #selector(frameAll(_:)),
                         keyEquivalent: "f").target = self
        viewMenu.addItem(withTitle: "Use Authored Camera",
                         action: #selector(useAuthoredCamera(_:)), keyEquivalent: "").target = self
        viewMenu.addItem(.separator())
        viewMenu.addItem(withTitle: "Wireframe", action: #selector(toggleWireframe(_:)),
                         keyEquivalent: "").target = self
        viewMenu.addItem(withTitle: "Ground Grid", action: #selector(toggleGrid(_:)),
                         keyEquivalent: "").target = self
        viewMenu.addItem(withTitle: "Shadows", action: #selector(toggleShadows(_:)),
                         keyEquivalent: "").target = self
        viewMenu.addItem(withTitle: "Environment Background",
                         action: #selector(toggleBackground(_:)), keyEquivalent: "").target = self
        viewMenu.addItem(.separator())
        viewMenu.addItem(withTitle: "Reset Exposure",
                         action: #selector(resetExposure(_:)), keyEquivalent: "").target = self
        viewItem.submenu = viewMenu

        let cameraItem = NSMenuItem()
        main.addItem(cameraItem)
        let cameraMenu = NSMenu(title: "Camera")
        let presets = ["Isometric", "Front", "Back", "Right", "Left", "Top", "Bottom"]
        for (index, name) in presets.enumerated() {
            let item = NSMenuItem(title: name, action: #selector(viewPreset(_:)),
                                  keyEquivalent: "\(index + 1)")
            item.tag = index
            item.target = self
            cameraMenu.addItem(item)
        }
        cameraItem.submenu = cameraMenu

        let playbackItem = NSMenuItem()
        main.addItem(playbackItem)
        let playbackMenu = NSMenu(title: "Playback")
        playbackMenu.addItem(withTitle: "Play / Pause", action: #selector(togglePlay(_:)),
                             keyEquivalent: "").target = self
        playbackItem.submenu = playbackMenu

        NSApp.mainMenu = main
    }
}
