import QuartzCore
import UIKit

/// The view's layer is the CAMetalLayer that C++ renders into (lab/src/app.cpp via ag_app.h).
final class MetalView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }
}

/// Hosts one lab run per launch (devicectl ... -- --mode=X --flag=value): a CADisplayLink at the
/// requested refresh (--req-display_refresh_hz; 60 Hz primary, 120 Hz subset D53) drives ag_app_frame; host-only facts (battery, brightness, refresh rate) go to C++ once
/// a second. Results land in Documents/results/<run-id>; tools/device/run_ios.sh pulls them.
final class GameViewController: UIViewController {
    // Fixed internal resolution (D7): the drawable is 720 x 1560 on every device, scaled by the OS.
    private static let drawableW = 720
    private static let drawableH = 1560

    private let label = UILabel()
    private var link: CADisplayLink?
    private var stateTimer: Timer?
    private var running = false
    private var finished = false
    private var measuredHz = 0
    private var requestedHz = 60

    override func loadView() { view = MetalView() }
    override var prefersStatusBarHidden: Bool { true }
    override var prefersHomeIndicatorAutoHidden: Bool { true }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        label.textColor = UIColor(red: 0.6, green: 0.9, blue: 1, alpha: 0.8)
        label.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
        label.numberOfLines = 0
        label.textAlignment = .center
        label.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(label)
        NSLayoutConstraint.activate([
            label.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            label.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor, constant: -24),
            label.leadingAnchor.constraint(greaterThanOrEqualTo: view.leadingAnchor, constant: 16),
            label.trailingAnchor.constraint(lessThanOrEqualTo: view.trailingAnchor, constant: -16),
        ])
        label.text = "Afterglow\nlaunch with --mode=… (tools/device/run_ios.sh)"
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        if !running && !finished { start() }
    }

    private static func arg(_ args: [String], _ key: String) -> String? {
        args.first(where: { $0.hasPrefix("--\(key)=") }).map { String($0.dropFirst(key.count + 3)) }
    }

    private func start() {
        let args = Array(CommandLine.arguments.dropFirst().filter { $0.hasPrefix("--") })
        guard args.contains(where: { $0.hasPrefix("--mode=") }) else { return }
        running = true
        setvbuf(stdout, nil, _IOLBF, 0)
        UIApplication.shared.isIdleTimerDisabled = true
        // Brightness is set by the app from the launch argument (docs/04 §1, 50 %). iOS applies it
        // over about a second, so the run is created only after it has settled (state_start, D40).
        var settle = 0.0
        if let b = Self.arg(args, "brightness"), let pct = Double(b) {
            view.window?.windowScene?.screen.brightness = CGFloat(pct / 100.0)
            settle = 1.5
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + settle) { [weak self] in self?.create(args: args) }
    }

    private func create(args: [String]) {
        var argv = args
        requestedHz = Self.arg(args, "req-display_refresh_hz").flatMap(Int.init).map { max($0, 1) } ?? 60
        if Self.arg(args, "kernels") == nil { argv.append("--kernels=kernels") }
        if Self.arg(args, "levels") == nil { argv.append("--levels=levels") }
        if Self.arg(args, "brightness") != nil, Self.arg(args, "req-brightness") == nil {
            argv.append("--req-brightness=\(Self.arg(args, "brightness")!)")
        }
        let bundle = Bundle.main.resourcePath ?? "."
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].path

        let layer = (view as! MetalView).metalLayer
        layer.contentsGravity = .resizeAspect
        layer.backgroundColor = UIColor.black.cgColor
        ag_app_platform_state(platformState())   // before create: state_start needs the host facts
        let rc = ag_app_create(argv.joined(separator: " "), bundle, docs, Int32(AG_SURFACE_METAL_LAYER),
                               Unmanaged.passUnretained(layer).toOpaque(),
                               Int32(Self.drawableW), Int32(Self.drawableH))
        let msg = String(cString: ag_app_message())
        print("afterglow: \(msg)")
        label.text = msg
        if rc != 0 {
            end(code: 2)
            return
        }
        let link = CADisplayLink(target: self, selector: #selector(tick(_:)))
        let hz = Float(requestedHz)
        link.preferredFrameRateRange = CAFrameRateRange(minimum: hz, maximum: hz, preferred: hz)
        link.add(to: .main, forMode: .common)
        self.link = link
        stateTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            guard let self else { return }
            ag_app_platform_state(self.platformState())
        }
    }

    @objc private func tick(_ l: CADisplayLink) {
        let period = l.targetTimestamp - l.timestamp
        if period > 0 { measuredHz = Int((1.0 / period).rounded()) }
        let r = ag_app_frame(Int64(l.targetTimestamp * 1e9), Int64(period * 1e9))
        if r != 0 { end(code: r == 1 ? 0 : 1) }
    }

    func hostEvent(_ name: String) {
        guard running, !finished else { return }
        ag_app_event(name)
        end(code: 3)
    }

    private func end(code: Int) {
        guard !finished else { return }
        finished = true
        link?.invalidate()
        stateTimer?.invalidate()
        let msg = String(cString: ag_app_message())
        let dir = String(cString: ag_app_run_dir())
        print("afterglow: \(msg)")
        label.text = msg
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let marker: [String: Any] = ["exit_code": code, "message": msg, "run_dir": dir]
        if let data = try? JSONSerialization.data(withJSONObject: marker, options: [.prettyPrinted]) {
            try? data.write(to: docs.appendingPathComponent("last_run.json"))
        }
        ag_app_destroy()
        fflush(stdout)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { exit(Int32(code)) }
    }

    /// Facts C++ cannot read on iOS (merged into run.json and device.csv, R7).
    private func platformState() -> String {
        let dev = UIDevice.current
        dev.isBatteryMonitoringEnabled = true
        let screen = view.window?.windowScene?.screen
        let level = dev.batteryLevel
        let st = dev.batteryState
        let charging = st == .charging || st == .full
        var state: [String: Any] = [
            "display_refresh_hz": measuredHz > 0 ? measuredHz : requestedHz,
            "display_max_hz": screen?.maximumFramesPerSecond ?? 0,
            "brightness": Int(((screen?.brightness ?? 0) * 100).rounded()),
            "low_power_mode": ProcessInfo.processInfo.isLowPowerModeEnabled,
            "charging": charging,
            "power_source": charging ? "AC" : (st == .unplugged ? "battery" : "unknown"),
        ]
        state["battery_percent"] = level >= 0 ? Int((level * 100).rounded()) : "n/a: battery level unknown"
        let device: [String: Any] = [
            "idiom": dev.userInterfaceIdiom == .pad ? "pad" : "phone",
            "os": "\(dev.systemName) \(dev.systemVersion)",
        ]
        let osGame: [String: Any] = ["game_mode": "n/a: no public API reports whether Game Mode is active"]
        let root: [String: Any] = ["device": device, "state": state, "os_game": osGame]
        let data = (try? JSONSerialization.data(withJSONObject: root)) ?? Data("{}".utf8)
        return String(decoding: data, as: UTF8.self)
    }
}
