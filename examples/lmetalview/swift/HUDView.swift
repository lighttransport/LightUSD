// SPDX-License-Identifier: Apache-2.0
//
// lmetalview - minimal overlay chrome: file/status readout and the timeline.

import AppKit

final class HUDView: NSView {
    private let infoPanel = NSVisualEffectView()
    private let controlPanel = NSVisualEffectView()
    private let titleLabel = NSTextField(labelWithString: "")
    private let statusLabel = NSTextField(labelWithString: "")
    private let timeLabel = NSTextField(labelWithString: "")
    let timeline = NSSlider(value: 0, minValue: 0, maxValue: 1,
                            target: nil, action: nil)
    let playButton = NSButton(title: "Play", target: nil, action: nil)

    var onTimeChanged: ((Double) -> Void)?
    var onTimeCommit: (() -> Void)?
    var onPlayToggle: (() -> Void)?

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.backgroundColor = NSColor.clear.cgColor

        for panel in [infoPanel, controlPanel] {
            panel.material = .hudWindow
            panel.blendingMode = .withinWindow
            panel.state = .active
            panel.wantsLayer = true
            panel.layer?.cornerRadius = 8
            panel.layer?.masksToBounds = true
            addSubview(panel)
            panel.translatesAutoresizingMaskIntoConstraints = false
        }

        titleLabel.font = .systemFont(ofSize: 13, weight: .semibold)
        titleLabel.textColor = .white
        titleLabel.lineBreakMode = .byTruncatingMiddle
        statusLabel.font = .systemFont(ofSize: 11)
        statusLabel.textColor = .secondaryLabelColor
        statusLabel.maximumNumberOfLines = 3
        statusLabel.lineBreakMode = .byWordWrapping
        timeLabel.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular)
        timeLabel.textColor = .secondaryLabelColor

        for label in [titleLabel, statusLabel] {
            infoPanel.addSubview(label)
            label.translatesAutoresizingMaskIntoConstraints = false
        }
        for view in [playButton, timeline, timeLabel] {
            controlPanel.addSubview(view)
            view.translatesAutoresizingMaskIntoConstraints = false
        }
        playButton.bezelStyle = .rounded
        playButton.target = self
        playButton.action = #selector(playTapped)
        timeline.target = self
        timeline.action = #selector(timelineChanged)
        timeline.isContinuous = true
        timeline.controlSize = .small
        controlPanel.isHidden = true

        NSLayoutConstraint.activate([
            infoPanel.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            infoPanel.topAnchor.constraint(equalTo: topAnchor, constant: 12),
            infoPanel.widthAnchor.constraint(lessThanOrEqualToConstant: 520),
            titleLabel.leadingAnchor.constraint(equalTo: infoPanel.leadingAnchor, constant: 10),
            titleLabel.trailingAnchor.constraint(equalTo: infoPanel.trailingAnchor, constant: -10),
            titleLabel.topAnchor.constraint(equalTo: infoPanel.topAnchor, constant: 8),
            statusLabel.leadingAnchor.constraint(equalTo: titleLabel.leadingAnchor),
            statusLabel.trailingAnchor.constraint(equalTo: titleLabel.trailingAnchor),
            statusLabel.topAnchor.constraint(equalTo: titleLabel.bottomAnchor, constant: 2),
            statusLabel.bottomAnchor.constraint(equalTo: infoPanel.bottomAnchor, constant: -8),
            controlPanel.centerXAnchor.constraint(equalTo: centerXAnchor),
            controlPanel.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -14),
            controlPanel.widthAnchor.constraint(greaterThanOrEqualToConstant: 420),
            playButton.leadingAnchor.constraint(equalTo: controlPanel.leadingAnchor, constant: 10),
            playButton.centerYAnchor.constraint(equalTo: controlPanel.centerYAnchor),
            playButton.topAnchor.constraint(equalTo: controlPanel.topAnchor, constant: 8),
            playButton.bottomAnchor.constraint(equalTo: controlPanel.bottomAnchor, constant: -8),
            timeline.leadingAnchor.constraint(equalTo: playButton.trailingAnchor, constant: 8),
            timeline.centerYAnchor.constraint(equalTo: playButton.centerYAnchor),
            timeLabel.leadingAnchor.constraint(equalTo: timeline.trailingAnchor, constant: 8),
            timeLabel.trailingAnchor.constraint(equalTo: controlPanel.trailingAnchor, constant: -10),
            timeLabel.centerYAnchor.constraint(equalTo: playButton.centerYAnchor),
            timeline.widthAnchor.constraint(greaterThanOrEqualToConstant: 220)
        ])
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func hitTest(_ point: NSPoint) -> NSView? {
        // Only the timeline panel is interactive; the info readout must not
        // swallow orbit/pan drags that start over it.
        let hit = super.hitTest(point)
        guard let hit else { return nil }
        if hit === self { return nil }
        if hit === infoPanel || hit.isDescendant(of: infoPanel) { return nil }
        return hit
    }

    func setTitle(_ text: String) { titleLabel.stringValue = text }
    func setStatus(_ text: String) { statusLabel.stringValue = text }

    func configureTimeline(start: Double, end: Double, value: Double,
                           hasAnimation: Bool, playing: Bool) {
        controlPanel.isHidden = !hasAnimation
        timeline.minValue = start
        timeline.maxValue = max(end, start + 0.0001)
        timeline.doubleValue = value
        playButton.title = playing ? "Pause" : "Play"
        updateTimeLabel(value, fps: 0)
    }

    func setPlaying(_ playing: Bool) {
        playButton.title = playing ? "Pause" : "Play"
    }

    func updateTimeLabel(_ time: Double, fps: Double) {
        if fps > 0 {
            timeLabel.stringValue = String(format: "%.2f (%.0f fps)", time, fps)
        } else {
            timeLabel.stringValue = String(format: "%.2f", time)
        }
    }

    @objc private func playTapped() { onPlayToggle?() }

    @objc private func timelineChanged() {
        onTimeChanged?(timeline.doubleValue)
    }
}
