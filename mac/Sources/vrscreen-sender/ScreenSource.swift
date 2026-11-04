import CoreGraphics
import CoreMedia
import Foundation
import MirrorProtocol
import ScreenCaptureKit

/// Captures one display with ScreenCaptureKit as 420v (BT.709 video range), and the
/// system audio (48 kHz stereo, this process excluded) into an `AudioPipeline` while
/// audio capture is on. SCStream only delivers images when the screen changes;
/// idle/blank frames are ignored. Audio buffers arrive continuously, silence included.
final class ScreenSource: NSObject, SCStreamOutput, SCStreamDelegate {
    private let displayIndex: Int?
    private let fps: Int
    private let maxWidth: Int
    private let queue: DispatchQueue
    private let handler: (CVPixelBuffer, UInt64) -> Void
    private let audio: AudioPipeline?
    /// Whether the stream captures audio (`setAudioCapture`). On `queue`.
    private var capturesAudio: Bool

    private var stream: SCStream?
    private var display: SCDisplay?
    private var outputSize = (width: 0, height: 0)
    private var stopped = false

    /// `handler` is called on `queue` with each captured image and its capture time
    /// (µs, host clock: the sample's presentation timestamp). Audio buffers go to `audio`
    /// on its own queue while `capturesAudio` is on.
    init(displayIndex: Int?, fps: Int, maxWidth: Int, queue: DispatchQueue, audio: AudioPipeline?,
         capturesAudio: Bool, handler: @escaping (CVPixelBuffer, UInt64) -> Void) {
        self.displayIndex = displayIndex
        self.fps = fps
        self.maxWidth = maxWidth
        self.queue = queue
        self.audio = audio
        self.capturesAudio = capturesAudio && audio != nil
        self.handler = handler
    }

    /// Starts capture; `completion` gets an error (e.g. missing Screen Recording permission).
    func start(completion: @escaping (Error?) -> Void) {
        SCShareableContent.getExcludingDesktopWindows(false, onScreenWindowsOnly: true) { [weak self] content, error in
            guard let self else { return }
            self.queue.async {
                if let error { return completion(error) }
                guard let content, !content.displays.isEmpty else {
                    return completion(OptionError("ScreenCaptureKit returned no displays (Screen Recording permission?)"))
                }
                for (i, d) in content.displays.enumerated() {
                    let mode = CGDisplayCopyDisplayMode(d.displayID)
                    let main = d.displayID == CGMainDisplayID() ? " (main)" : ""
                    log("source: display \(i): id \(d.displayID), \(mode?.pixelWidth ?? d.width)x\(mode?.pixelHeight ?? d.height) px\(main)")
                }
                let display: SCDisplay
                if let index = self.displayIndex {
                    guard index < content.displays.count else {
                        return completion(OptionError("--display \(index) out of range (\(content.displays.count) displays)"))
                    }
                    display = content.displays[index]
                } else {
                    display = content.displays.first { $0.displayID == CGMainDisplayID() } ?? content.displays[0]
                }
                self.display = display
                self.startStream(on: display, completion: completion)
            }
        }
    }

    func stop(completion: @escaping () -> Void) {
        stopped = true
        guard let stream else { return completion() }
        stream.stopCapture { _ in completion() }
        self.stream = nil
    }

    /// Native pixel size scaled down to fit `maxWidth`, even dimensions.
    private func computeOutputSize(for display: SCDisplay) -> (width: Int, height: Int) {
        let mode = CGDisplayCopyDisplayMode(display.displayID)
        let nativeW = mode?.pixelWidth ?? display.width
        let nativeH = mode?.pixelHeight ?? display.height
        let scale = min(1, Double(maxWidth) / Double(nativeW))
        let w = Int((Double(nativeW) * scale).rounded()) & ~1
        let h = Int((Double(nativeH) * scale).rounded()) & ~1
        return (max(2, w), max(2, h))
    }

    private func makeConfiguration(width: Int, height: Int) -> SCStreamConfiguration {
        let c = SCStreamConfiguration()
        c.width = width
        c.height = height
        c.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        c.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        c.colorMatrix = CGDisplayStream.yCbCrMatrix_ITU_R_709_2
        c.colorSpaceName = CGColorSpace.sRGB
        c.showsCursor = true
        c.queueDepth = 6
        c.capturesAudio = capturesAudio
        c.sampleRate = Int(AudioPipeline.config.sampleRate)
        c.channelCount = Int(AudioPipeline.config.channels)
        c.excludesCurrentProcessAudio = true
        return c
    }

    private func startStream(on display: SCDisplay, completion: @escaping (Error?) -> Void) {
        let size = computeOutputSize(for: display)
        outputSize = size
        let filter = SCContentFilter(display: display, excludingWindows: [])
        let stream = SCStream(filter: filter, configuration: makeConfiguration(width: size.width, height: size.height),
                              delegate: self)
        do {
            try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
            if let audio { try stream.addStreamOutput(self, type: .audio, sampleHandlerQueue: audio.queue) }
        } catch {
            return completion(error)
        }
        self.stream = stream
        stream.startCapture { [weak self] error in
            self?.queue.async {
                if error == nil {
                    log("source: capturing display \(display.displayID) at \(size.width)x\(size.height) @ \(self?.fps ?? 0) fps (420v, BT.709 video range)"
                        + ((self?.capturesAudio ?? false) ? " + system audio" : ""))
                }
                completion(error)
            }
        }
    }

    /// Reconfigures the stream if the display's pixel resolution changed. Call periodically on `queue`.
    func checkDisplayMode() {
        guard let display, let stream, !stopped else { return }
        let size = computeOutputSize(for: display)
        guard size != outputSize else { return }
        log("source: display resolution changed, capturing at \(size.width)x\(size.height)")
        outputSize = size
        stream.updateConfiguration(makeConfiguration(width: size.width, height: size.height)) { error in
            if let error { log("source: updateConfiguration failed: \(error)") }
        }
    }

    /// Starts or stops audio capture on the running stream. Call on `queue`.
    func setAudioCapture(_ on: Bool) {
        let on = on && audio != nil
        guard on != capturesAudio else { return }
        capturesAudio = on
        guard let stream, !stopped else { return } // applies when the stream (re)starts
        stream.updateConfiguration(makeConfiguration(width: outputSize.width, height: outputSize.height)) { error in
            if let error {
                log("source: switching audio capture \(on ? "on" : "off") failed: \(error)")
            } else {
                log("source: system audio capture \(on ? "on" : "off")")
            }
        }
    }

    // MARK: SCStreamOutput

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        if type == .audio {
            if let audio { audio.handle(sampleBuffer) } // on audio.queue
            return
        }
        guard type == .screen, sampleBuffer.isValid else { return }
        guard let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false)
                as? [[SCStreamFrameInfo: Any]],
              let rawStatus = attachments.first?[.status] as? Int,
              SCFrameStatus(rawValue: rawStatus) == .complete,
              let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else {
            return // .idle / .blank / .suspended etc. carry no new image.
        }
        handler(pixelBuffer, HostClock.micros(CMSampleBufferGetPresentationTimeStamp(sampleBuffer)))
    }

    // MARK: SCStreamDelegate

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        queue.async { [weak self] in
            guard let self, !self.stopped else { return }
            log("source: stream stopped: \(error); restarting in 1 s")
            self.stream = nil
            self.scheduleRestart()
        }
    }

    private func scheduleRestart() {
        queue.asyncAfter(deadline: .now() + 1) { [weak self] in
            guard let self, !self.stopped, let display = self.display else { return }
            self.startStream(on: display) { error in
                if let error {
                    log("source: restart failed: \(error); retrying in 1 s")
                    self.stream = nil
                    self.scheduleRestart()
                }
            }
        }
    }
}
