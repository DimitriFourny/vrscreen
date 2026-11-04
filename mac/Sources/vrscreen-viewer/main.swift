import CoreVideo
import Foundation
import MirrorProtocol

setvbuf(stdout, nil, _IOLBF, 0)

let options: ViewerOptions
do {
    options = try ViewerOptions.parse(Array(CommandLine.arguments.dropFirst()))
} catch {
    FileHandle.standardError.write("error: \(error)\n\n\(ViewerOptions.usage)\n".data(using: .utf8)!)
    exit(2)
}

let queue = DispatchQueue(label: "vrscreen.viewer", qos: .userInteractive)
let discoverer = options.discover ? Discoverer(port: options.discoveryPort, queue: queue) : nil
let client = ViewerClient(host: options.host, port: options.port, discoverer: discoverer, queue: queue)
let decoder = VideoDecoder()
let window: ViewerWindow? = options.headless ? nil : ViewerWindow()
let audio = AudioReceiver(wavPath: options.audioWavPath, detectOnsets: options.avSync)
/// `--av-sync` pairing state; on `queue`.
var avSync = AVSyncCheck()
audio.onBeepOnset = { micros in avSync.beep(atMicros: micros) }

/// Finishes the WAV file and the A/V sync summary, then exits. On `queue`.
func exitViewer(_ code: Int32) {
    audio.finish()
    if let summary = avSync.summary { log(summary) }
    exit(code)
}

/// Per-second counters; touched on `queue` only.
struct Stats {
    var received = 0
    var decoded = 0
    var latenciesMs: [Double] = []
}
var stats = Stats()
var lastBytesReceived = 0
var lastStatsTime = Date()
var snapshotWritten = false

decoder.onDecoded = { image, captureMicros, decodedMicros in
    let meanLuma = options.avSync ? AVSyncCheck.meanLuma(image) : 0
    queue.async {
        stats.decoded += 1
        if options.avSync { avSync.frame(captureMicros: captureMicros, meanLuma: meanLuma) }
        // Capture → decoded latency on the sender's clock, via the PING/PONG offset
        // (the same path a client on another machine needs).
        if let offset = client.clock.offsetMicros {
            let latency = Int64(decodedMicros) + offset - Int64(captureMicros)
            stats.latenciesMs.append(Double(latency) / 1000)
        }
        if let path = options.snapshotPath, !snapshotWritten {
            snapshotWritten = true
            do {
                try writePNG(image, to: path)
                log("snapshot: wrote \(path) (\(CVPixelBufferGetWidth(image))x\(CVPixelBufferGetHeight(image)))")
                if options.exitAfterSnapshot { exitViewer(0) }
            } catch {
                log("\(error)")
                if options.exitAfterSnapshot { exitViewer(1) }
            }
        }
    }
    if let window {
        DispatchQueue.main.async { window.display(image) }
    }
}

client.onMessage = { message in
    switch message {
    case .config(let config):
        do {
            try decoder.configure(config)
            if let window {
                let text = "\(config.width)x\(config.height) \(config.codec) from \(client.host)"
                DispatchQueue.main.async { window.setStatus(text) }
            }
        } catch {
            log("decoder: bad CONFIG: \(error)")
        }
    case .frame(let frame):
        stats.received += 1
        decoder.decode(frame) // handed to the decoder, or dropped while waiting for a keyframe
        client.acknowledgeFrame()
    case .control(let command):
        log("control: \(command)")
    case .audioConfig(let config):
        audio.handleConfig(config)
    case .audio(let chunk):
        audio.handle(chunk)
    default:
        break
    }
}
client.onDisconnected = {
    decoder.reset()
    audio.connectionReset()
    if let window { DispatchQueue.main.async { window.setStatus("reconnecting…") } }
}

let statsTimer = DispatchSource.makeTimerSource(queue: queue)
statsTimer.schedule(deadline: .now() + 1, repeating: 1)
statsTimer.setEventHandler {
    let now = Date()
    let dt = now.timeIntervalSince(lastStatsTime)
    lastStatsTime = now
    let bytes = client.bytesReceived - lastBytesReceived
    lastBytesReceived = client.bytesReceived
    let latency: String
    if stats.latenciesMs.isEmpty {
        latency = "latency n/a"
    } else {
        let avg = stats.latenciesMs.reduce(0, +) / Double(stats.latenciesMs.count)
        latency = String(format: "latency avg %.1f ms max %.1f ms", avg, stats.latenciesMs.max()!)
    }
    let rtt = client.clock.lastRTTMicros.map { String(format: "rtt %.2f ms", Double($0) / 1000) } ?? "rtt n/a"
    let format = decoder.config.map { "\($0.width)x\($0.height) \($0.codec)" } ?? "no stream"
    var text = String(format: "recv %.1f fps | decoded %.1f fps | %.2f Mbit/s | %@ | %@ | %@",
                      Double(stats.received) / dt, Double(stats.decoded) / dt, Double(bytes) * 8 / dt / 1e6,
                      latency, rtt, format)
    if let audioText = audio.takeStatsText(dt: dt) { text += " | " + audioText }
    log(text)
    stats = Stats()
}
statsTimer.resume()

if let duration = options.durationSeconds {
    queue.asyncAfter(deadline: .now() + duration) { exitViewer(0) }
}

// Ctrl-C / SIGTERM: finish the WAV header before exiting.
signal(SIGINT, SIG_IGN)
signal(SIGTERM, SIG_IGN)
var signalSources: [DispatchSourceSignal] = []
for sig in [SIGINT, SIGTERM] {
    let source = DispatchSource.makeSignalSource(signal: sig, queue: queue)
    source.setEventHandler { exitViewer(0) }
    source.resume()
    signalSources.append(source)
}

client.start()

if let window {
    window.run()
} else {
    dispatchMain()
}
