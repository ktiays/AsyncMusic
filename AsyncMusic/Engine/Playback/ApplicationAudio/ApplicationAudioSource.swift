#if os(macOS)
import AppKit
import Observation

@Observable
final class ApplicationAudioSource {
    private(set) var selected: AudioApplication?
    private(set) var isCapturing = false
    private(set) var isStarting = false
    private(set) var isWaitingForAudio = false
    private(set) var error: String?
    private(set) var sourceID: UInt64?
    @ObservationIgnored var onReady: ((ProcessTapStreamFeeder.Stream, UInt64) -> Void)?
    @ObservationIgnored var onFailure: ((String) -> Void)?
    @ObservationIgnored private var feeder: ProcessTapStreamFeeder?
    @ObservationIgnored private var token: UUID?
    @ObservationIgnored private var sleepObserver: NSObjectProtocol?
    @ObservationIgnored private var waitingTask: Task<Void, Never>?

    init() {
        sleepObserver = NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.willSleepNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, self.isCapturing else { return }
                self.failed("App audio stopped because this Mac is going to sleep.")
            }
        }
    }

    func select(_ application: AudioApplication?) {
        stop()
        selected = application
        error = nil
    }

    func start() {
        guard let selected else { return }
        stop()
        error = nil
        isCapturing = true
        isStarting = true
        isWaitingForAudio = true
        let token = UUID()
        self.token = token
        let source = UInt64.random(in: PlaybackSource.firstFile ... .max)
        sourceID = source
        waitingTask = Task { [weak self] in
            while !Task.isCancelled {
                guard self?.startWhenAvailable(selection: selected, token: token, source: source) == false else { return }
                do { try await Task.sleep(for: .milliseconds(200)) } catch { return }
            }
        }
    }

    /// Wait without opening a HAL graph while the selected app is silent. Resolve audio
    /// object IDs afresh, including helpers created only when playback begins.
    private func startWhenAvailable(selection: AudioApplication, token: UUID, source: UInt64) -> Bool {
        guard self.token == token else { return true }
        let catalog = AudioApplicationCatalog()
        catalog.refresh()
        if let error = catalog.error {
            failed(error)
            return true
        }
        guard let target = catalog.applications.first(where: { $0.id == selection.id }) else {
            failed("This app is no longer running. Open it, then choose it again.")
            return true
        }
        if selected != target { selected = target }
        guard target.isPlaying, !target.processIDs.isEmpty else { return false }
        waitingTask = nil
        let feeder = ProcessTapStreamFeeder(application: target)
        feeder.onReady = { [weak self] stream in
            DispatchQueue.main.async {
                guard let self, self.token == token else { return }
                self.isStarting = false
                self.isWaitingForAudio = false
                self.onReady?(stream, source)
            }
        }
        feeder.onFailure = { [weak self] message in
            DispatchQueue.main.async {
                guard let self, self.token == token else { return }
                self.failed(message)
            }
        }
        self.feeder = feeder
        feeder.start()
        return true
    }

    func stop() {
        token = nil
        waitingTask?.cancel()
        waitingTask = nil
        feeder?.stop()
        feeder = nil
        isCapturing = false
        isStarting = false
        isWaitingForAudio = false
        sourceID = nil
    }

    private func failed(_ message: String) {
        stop()
        error = message
        onFailure?(message)
    }

    deinit {
        waitingTask?.cancel()
        if let sleepObserver { NSWorkspace.shared.notificationCenter.removeObserver(sleepObserver) }
        feeder?.stop()
    }
}
#endif
