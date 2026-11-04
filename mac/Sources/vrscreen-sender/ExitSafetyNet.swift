import Darwin
import Foundation

/// Restore actions that must run on every exit path, for system state the sender changes
/// temporarily (the frozen cursor, the muted Mac output): `exit()` (Ctrl-C, SIGTERM, SIGHUP
/// and Quit all shut down through it) via atexit, and crash signals via handlers that run
/// the actions and then re-raise the signal. SIGKILL cannot be caught.
///
/// Actions run from a signal handler, so they must only read plain globals and make the
/// system call that undoes their change; each must be harmless when there is nothing to undo.
enum ExitSafetyNet {
    private static let lock = NSLock()
    private static var installed = false

    /// Adds `action`; installs the atexit and crash-signal handlers on first use. Any thread.
    static func register(_ action: @escaping @convention(c) () -> Void) {
        lock.lock()
        defer { lock.unlock() }
        exitActions.append(action)
        guard !installed else { return }
        installed = true
        atexit { runExitActions() }
        for signalNumber in [SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE, SIGTRAP] {
            signal(signalNumber, fatalSignalHandler)
        }
    }
}

nonisolated(unsafe) private var exitActions: [@convention(c) () -> Void] = []

private func runExitActions() {
    for action in exitActions { action() }
}

private func fatalSignalHandler(_ signalNumber: Int32) {
    runExitActions()
    signal(signalNumber, SIG_DFL)
    raise(signalNumber)
}
