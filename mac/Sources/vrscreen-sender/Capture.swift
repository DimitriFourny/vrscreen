import CoreGraphics
import CoreVideo
import Foundation
import MirrorProtocol

/// Which display to capture.
enum DisplaySelection: Equatable {
    case main
    /// --display N: index in ScreenCaptureKit's list (logged at startup).
    case index(Int)
    /// CGDirectDisplayID chosen in the menu; the main display if it is not connected.
    case id(UInt32)
}

/// The settings that shape the video stream.
struct VideoSettings: Equatable {
    var maxWidth: Int
    var fps: Int
    var codec: VideoCodec
    var bitrateMbps: Double
    var display: DisplaySelection

    var description: String {
        "max \(maxWidth) px, \(fps) fps, \(codec), \(SenderSettings.format(mbps: bitrateMbps)) Mbit/s"
    }

    /// The encoder must be recreated for this change.
    func needsNewEncoder(from old: VideoSettings) -> Bool {
        codec != old.codec || bitrateMbps != old.bitrateMbps || fps != old.fps
    }

    /// The capture must be restarted for this change.
    func needsNewCapture(from old: VideoSettings) -> Bool {
        fps != old.fps || maxWidth != old.maxWidth || display != old.display
    }

    /// `width`×`height` scaled down to fit `maxWidth` (never up), even dimensions.
    static func fit(width: Int, height: Int, maxWidth: Int) -> (width: Int, height: Int) {
        let scale = min(1, Double(maxWidth) / Double(max(1, width)))
        let w = Int((Double(width) * scale).rounded()) & ~1
        let h = Int((Double(height) * scale).rounded()) & ~1
        return (max(2, w), max(2, h))
    }
}

/// Owns the frame source (screen or test pattern) and applies video settings live: a
/// codec/bitrate/frame-rate change swaps the encoder, a frame-rate/size/display change
/// restarts the capture. A failed screen capture start is retried every 10 s. On `queue`.
final class CaptureController {
    enum Status: Equatable {
        case starting
        case running(displayID: UInt32?)
        case failed(String, isPermission: Bool)
    }

    static let retryInterval: TimeInterval = 10

    private let source: SenderOptions.Source
    private let testSize: (width: Int, height: Int)
    private let keyframeIntervalSeconds: Double
    private let queue: DispatchQueue
    private let pipeline: SenderPipeline
    /// Audio that rides on the source: ScreenCaptureKit audio (nil when the process tap is
    /// used) or the test pattern's beeps.
    private let sourceAudio: AudioPipeline?
    private(set) var video: VideoSettings
    private var audioOn: Bool
    private var screenSource: ScreenSource?
    private var testSource: TestPatternSource?
    /// Bumped on every (re)start; callbacks of older starts are ignored.
    private var generation = 0
    private var stopped = false
    /// The last start error (logged once, not on every retry).
    private var lastFailure: String?
    /// Called on `queue` with every status change.
    var onStatus: ((Status) -> Void)?

    init(source: SenderOptions.Source, testSize: (width: Int, height: Int), keyframeIntervalSeconds: Double,
         video: VideoSettings, audioOn: Bool, sourceAudio: AudioPipeline?, queue: DispatchQueue, pipeline: SenderPipeline) {
        self.source = source
        self.testSize = testSize
        self.keyframeIntervalSeconds = keyframeIntervalSeconds
        self.video = video
        self.audioOn = audioOn
        self.sourceAudio = sourceAudio
        self.queue = queue
        self.pipeline = pipeline
    }

    static func makeEncoder(_ video: VideoSettings, keyframeIntervalSeconds: Double) -> VideoEncoder {
        VideoEncoder(codec: video.codec, bitrate: Int(video.bitrateMbps * 1_000_000), fps: video.fps,
                     keyframeIntervalSeconds: keyframeIntervalSeconds)
    }

    func start() {
        startSource()
    }

    /// Applies new video settings (no-op if unchanged).
    func apply(_ new: VideoSettings) {
        let old = video
        guard new != old, !stopped else { return }
        video = new
        let encoder = new.needsNewEncoder(from: old), capture = new.needsNewCapture(from: old)
        log("video: \(new.description)"
            + (encoder || capture ? "; restarting \([capture ? "capture" : nil, encoder ? "encoder" : nil].compactMap { $0 }.joined(separator: " and "))" : ""))
        if capture {
            // Before the encoder swap, so the new encoder doesn't start on an old-size image.
            stopSource()
            pipeline.discardLatestCapture()
        }
        if encoder {
            pipeline.replaceEncoder(Self.makeEncoder(new, keyframeIntervalSeconds: keyframeIntervalSeconds))
        }
        if capture { startSource() }
    }

    /// Audio capture on the source on/off (ScreenCaptureKit audio or test beeps).
    func setAudio(_ on: Bool) {
        audioOn = on
        testSource?.setAudio(on)
        screenSource?.setAudioCapture(on)
    }

    /// Follows display resolution changes. Call periodically.
    func checkDisplayMode() {
        screenSource?.checkDisplayMode()
    }

    func stop(completion: @escaping () -> Void) {
        stopped = true
        generation += 1
        testSource?.stop()
        testSource = nil
        guard let screen = screenSource else { return completion() }
        screenSource = nil
        screen.stop(completion: completion)
    }

    private func stopSource() {
        testSource?.stop()
        testSource = nil
        screenSource?.stop {}
        screenSource = nil
    }

    private func startSource() {
        generation += 1
        let generation = generation
        onStatus?(.starting)
        let deliver: (CVPixelBuffer, UInt64) -> Void = { [weak pipeline] buffer, micros in
            pipeline?.handleCapture(buffer, captureMicros: micros)
        }
        switch source {
        case .test:
            let size = VideoSettings.fit(width: testSize.width, height: testSize.height, maxWidth: video.maxWidth)
            let test = TestPatternSource(
                width: size.width, height: size.height, fps: video.fps, queue: queue,
                audioHandler: sourceAudio.map { audio in
                    { samples, micros in audio.queue.async { audio.handle(interleaved: samples, captureMicros: micros) } }
                },
                handler: deliver)
            do {
                try test.start()
            } catch {
                onStatus?(.failed("\(error)", isPermission: false))
                return
            }
            test.setAudio(audioOn)
            testSource = test
            onStatus?(.running(displayID: nil))
        case .screen:
            let screen = ScreenSource(display: video.display, fps: video.fps, maxWidth: video.maxWidth, queue: queue,
                                      audio: sourceAudio, capturesAudio: audioOn, handler: deliver)
            screenSource = screen
            screen.start { [weak self, weak screen] error in
                guard let self, generation == self.generation, let screen else { return }
                if let error {
                    if "\(error)" != self.lastFailure {
                        log("source: screen capture failed to start: \(error); retrying every \(Int(Self.retryInterval)) s")
                    }
                    self.lastFailure = "\(error)"
                    self.screenSource = nil
                    self.onStatus?(.failed("\(error)", isPermission: ScreenSource.isPermissionError(error)))
                    self.queue.asyncAfter(deadline: .now() + Self.retryInterval) { [weak self] in
                        guard let self, generation == self.generation, !self.stopped else { return }
                        self.startSource()
                    }
                } else {
                    self.lastFailure = nil
                    self.onStatus?(.running(displayID: screen.displayID))
                }
            }
        }
    }
}
