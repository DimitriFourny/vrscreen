import CoreMedia

/// The sender clock used for `captureTimeMicros` and PONG `serverTimeMicros`:
/// the CoreMedia host time clock (mach absolute time, monotonic, not wall time),
/// in microseconds. ScreenCaptureKit sample timestamps are on this clock.
public enum HostClock {
    public static func nowMicros() -> UInt64 {
        micros(CMClockGetTime(CMClockGetHostTimeClock()))
    }

    public static func micros(_ time: CMTime) -> UInt64 {
        UInt64(max(0, CMTimeConvertScale(time, timescale: 1_000_000, method: .roundHalfAwayFromZero).value))
    }

    public static func time(micros: UInt64) -> CMTime {
        CMTime(value: CMTimeValue(micros), timescale: 1_000_000)
    }
}
