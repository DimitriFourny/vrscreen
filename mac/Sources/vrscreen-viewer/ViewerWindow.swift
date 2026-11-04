import AppKit
import AVFoundation
import CoreMedia

/// AppKit window showing decoded frames through an AVSampleBufferDisplayLayer.
/// Use from the main thread only.
final class ViewerWindow: NSObject, NSApplicationDelegate {
    private let window: NSWindow
    private let displayLayer = AVSampleBufferDisplayLayer()
    private var videoSize = CGSize.zero

    override init() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1280, height: 720),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        super.init()
        displayLayer.videoGravity = .resizeAspect
        displayLayer.backgroundColor = NSColor.black.cgColor
        let view = NSView()
        view.layer = displayLayer
        view.wantsLayer = true
        window.contentView = view
        window.title = "VRScreen Viewer — connecting…"
        window.center()
    }

    func run() {
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        app.delegate = self
        window.makeKeyAndOrderFront(nil)
        app.activate(ignoringOtherApps: true)
        app.run()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func setStatus(_ text: String) {
        window.title = "VRScreen Viewer — \(text)"
    }

    func display(_ image: CVPixelBuffer) {
        var format: CMVideoFormatDescription?
        guard CMVideoFormatDescriptionCreateForImageBuffer(allocator: kCFAllocatorDefault, imageBuffer: image,
                                                           formatDescriptionOut: &format) == noErr,
              let format else { return }
        var timing = CMSampleTimingInfo(duration: .invalid, presentationTimeStamp: CMClockGetTime(CMClockGetHostTimeClock()),
                                        decodeTimeStamp: .invalid)
        var sample: CMSampleBuffer?
        guard CMSampleBufferCreateReadyWithImageBuffer(allocator: kCFAllocatorDefault, imageBuffer: image,
                                                       formatDescription: format, sampleTiming: &timing,
                                                       sampleBufferOut: &sample) == noErr,
              let sample else { return }
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true),
           CFArrayGetCount(attachments) > 0 {
            let dict = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(dict, Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                                 Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
        }

        let size = CGSize(width: CVPixelBufferGetWidth(image), height: CVPixelBufferGetHeight(image))
        if size != videoSize {
            videoSize = size
            window.contentAspectRatio = size
        }
        let renderer = displayLayer.sampleBufferRenderer
        if renderer.status == .failed || renderer.requiresFlushToResumeDecoding {
            renderer.flush()
        }
        renderer.enqueue(sample)
    }
}
