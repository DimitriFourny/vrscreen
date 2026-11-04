import CoreVideo
import Foundation
import MirrorProtocol

setvbuf(stdout, nil, _IOLBF, 0)

let options: SenderOptions
do {
    options = try SenderOptions.parse(Array(CommandLine.arguments.dropFirst()))
} catch {
    FileHandle.standardError.write("error: \(error)\n\n\(SenderOptions.usage)\n".data(using: .utf8)!)
    exit(2)
}

let queue = DispatchQueue(label: "quest-mirror.sender", qos: .userInteractive)
let encoder = VideoEncoder(codec: options.codec, bitrate: Int(options.bitrateMbps * 1_000_000),
                           fps: options.fps, keyframeIntervalSeconds: options.keyframeIntervalSeconds)
let server = MirrorServer(port: options.port, queue: queue)
let pipeline = SenderPipeline(queue: queue, encoder: encoder, server: server)
let deliver: (CVPixelBuffer, UInt64) -> Void = { buffer, micros in pipeline.handleCapture(buffer, captureMicros: micros) }

var testSource: TestPatternSource?
var screenSource: ScreenSource?

queue.async {
    do {
        try server.start()
        switch options.source {
        case .test:
            let source = TestPatternSource(width: options.width, height: options.height, fps: options.fps,
                                           queue: queue, handler: deliver)
            try source.start()
            testSource = source
        case .screen:
            let source = ScreenSource(displayIndex: options.displayIndex, fps: options.fps,
                                      maxWidth: options.maxWidth, queue: queue, handler: deliver)
            screenSource = source
            source.start { error in
                guard let error else { return }
                log("source: screen capture failed to start: \(error)")
                log("source: grant Screen Recording to the app running this binary (System Settings → Privacy & Security → Screen & System Audio Recording), restart it, or use --source test")
                exit(1)
            }
        }
    } catch {
        log("startup failed: \(error)")
        exit(1)
    }
}

let statsTimer = DispatchSource.makeTimerSource(queue: queue)
statsTimer.schedule(deadline: .now() + 1, repeating: 1)
statsTimer.setEventHandler {
    pipeline.logStats()
    screenSource?.checkDisplayMode()
}
statsTimer.resume()

signal(SIGINT, SIG_IGN)
signal(SIGTERM, SIG_IGN)
var signalSources: [DispatchSourceSignal] = []
for sig in [SIGINT, SIGTERM] {
    let source = DispatchSource.makeSignalSource(signal: sig, queue: queue)
    source.setEventHandler {
        log("shutting down")
        statsTimer.cancel()
        testSource?.stop()
        server.stop()
        pipeline.stop()
        if let screen = screenSource {
            screen.stop { exit(0) }
            // Do not hang on a stuck capture stop.
            queue.asyncAfter(deadline: .now() + 2) { exit(0) }
        } else {
            exit(0)
        }
    }
    source.resume()
    signalSources.append(source)
}

dispatchMain()
