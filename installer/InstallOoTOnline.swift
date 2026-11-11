// Install OoT Online: the Mac app a player downloads. It asks for the player's own game file (a .nup), then runs setup/install.py with the Python
// it carries (Contents/Resources/payload: python/, prebuilt/, oot-online/), which builds ~/Applications/Ocarina of Time Online.app on this Mac.
// No Xcode, Homebrew or Terminal: everything the build compiles was compiled by GitHub Actions (prebuilt/), and the shaders go through macOS's own
// Metal compiler. The payload is copied out of the app first (to ~/Library/Application Support/OoT Online/installer/<payload id>), without the
// download's quarantine mark, so the app may sit anywhere (macOS runs a downloaded app from a read-only place until it is moved).
// Built by installer/build_app.sh: swiftc -parse-as-library, macOS 26.6+.
import AppKit
import Darwin
import IOKit.pwr_mgt
import SwiftUI
import UniformTypeIdentifiers

#if !SNAPSHOT   // installer screens rendered offscreen for review: compiled with -D SNAPSHOT and a main of its own
@main
#endif
struct InstallerApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    var body: some Scene {
        Window("Install OoT Online", id: "main") {
            ContentView().environmentObject(Model.shared).frame(width: 620, height: 480)
        }
        .windowResizability(.contentSize)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard Model.shared.state == .running else { return .terminateNow }
        let a = NSAlert()
        a.messageText = "Stop building?"
        a.informativeText = "The build stops where it is. What it already made is kept, so running the installer again continues from there."
        a.addButton(withTitle: "Keep building"); a.addButton(withTitle: "Stop and quit")
        if a.runModal() == .alertFirstButtonReturn { return .terminateCancel }
        Model.shared.stop()
        return .terminateNow
    }
}

enum Phase { case welcome, running, done, failed }

final class Model: ObservableObject {
    static let shared = Model()
    init() {
        if let s = Model.testEnv["OOT_INSTALLER_SERVER"] { server = s }
        if let f = Model.testEnv["OOT_INSTALLER_NUP"] {   // a test: this file, and the build starts once the window is up
            nup = URL(fileURLWithPath: f); nupSize = "test"
            DispatchQueue.main.async { self.start() }
        }
    }
    @Published var state: Phase = .welcome
    @Published var nup: URL?
    @Published var nupSize = ""
    @Published var server = ""
    @Published var phaseTitle = ""
    @Published var phaseCount = ""
    @Published var stage = ""
    var stageName = ""
    @Published var count = ""
    @Published var progress: Double?
    @Published var elapsed = ""
    @Published var game = ""
    @Published var log: [String] = []
    @Published var error = ""
    @Published var warning = ""
    @Published var appPath = ""
    var started = Date()
    var proc: Process?
    var buffer = Data()
    var sleepAssertion: IOPMAssertionID = 0
    var timer: Timer?
    let support = Model.testEnv["OOT_INSTALLER_SUPPORT"].map { URL(fileURLWithPath: $0) } ?? FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/OoT Online")

    func choose() {
        let p = NSOpenPanel()
        p.message = "Choose your own dump of Ocarina of Time for Switch 2 (a .nup file)"
        p.prompt = "Choose"
        p.allowsMultipleSelection = false; p.canChooseDirectories = false
        if let t = UTType(filenameExtension: "nup") { p.allowedContentTypes = [t] }
        guard p.runModal() == .OK, let u = p.url else { return }
        nup = u
        let bytes = (try? u.resourceValues(forKeys: [.fileSizeKey]).fileSize).flatMap { $0 } ?? 0
        nupSize = ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file)
    }

    func add(_ line: String) {
        log.append(line)
        if log.count > 400 { log.removeFirst(log.count - 400) }
    }

    // The payload, copied out of the app once per payload id, without extended attributes (the download's quarantine mark among them)
    func payload() throws -> URL {
        guard let res = Bundle.main.resourceURL?.appendingPathComponent("payload"), FileManager.default.fileExists(atPath: res.path) else {
            throw NSError(domain: "OoTOnline", code: 1, userInfo: [NSLocalizedDescriptionKey: "This installer is incomplete (no payload): download it again."])
        }
        let id = (Bundle.main.infoDictionary?["OOTPayloadID"] as? String) ?? "dev"
        let root = support.appendingPathComponent("installer"), dest = root.appendingPathComponent(id)
        let ready = dest.appendingPathComponent(".ready")
        if FileManager.default.fileExists(atPath: ready.path) { return dest }
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let tmp = root.appendingPathComponent(id + ".partial")
        try? FileManager.default.removeItem(at: tmp)
        let d = Process()
        d.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        d.arguments = ["--noextattr", "--noqtn", res.path, tmp.path]
        try d.run(); d.waitUntilExit()
        guard d.terminationStatus == 0 else { throw NSError(domain: "OoTOnline", code: 2, userInfo: [NSLocalizedDescriptionKey: "Could not copy the installer's files to \(root.path)."]) }
        try? FileManager.default.removeItem(at: dest)
        try FileManager.default.moveItem(at: tmp, to: dest)
        try Data(id.utf8).write(to: dest.appendingPathComponent(".ready"))
        for old in (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? [] where old != id {   // earlier installers' copies
            try? FileManager.default.removeItem(at: root.appendingPathComponent(old))
        }
        return dest
    }

    func start() {
        guard let nup = nup else { return }
        state = .running; log = []; error = ""; warning = ""; game = ""
        phaseTitle = "Getting ready"; phaseCount = ""; stage = "Copying the installer's files"; count = ""; progress = nil
        started = Date()
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.tick() }
        IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep as CFString, IOPMAssertionLevel(kIOPMAssertionLevelOn),
                                    "Building Ocarina of Time Online" as CFString, &sleepAssertion)
        let server = self.server.trimmingCharacters(in: .whitespaces)
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                let p = try self.payload()
                DispatchQueue.main.async { self.launch(payload: p, nup: nup, server: server) }
            } catch { DispatchQueue.main.async { self.fail(error.localizedDescription) } }
        }
    }

    // Test runs (the app's launch environment): OOT_INSTALLER_NUP=<file> picks the game file and starts at once; OOT_INSTALLER_APP, _WORK, _DATA and _SUPPORT
    // put the game app, the build folder, the game's data folder and OoT Online's folder elsewhere (install.py --app, --work, --data, --support), so a test leaves a
    // player's own app and saves alone; OOT_INSTALLER_SERVER is the server
    static let testEnv = ProcessInfo.processInfo.environment
    func testArgs() -> [String] {
        [("OOT_INSTALLER_APP", "--app"), ("OOT_INSTALLER_WORK", "--work"), ("OOT_INSTALLER_DATA", "--data"), ("OOT_INSTALLER_SUPPORT", "--support")]
            .flatMap { k, opt in Model.testEnv[k].map { [opt, $0] } ?? [] }
    }

    func launch(payload p: URL, nup: URL, server: String) {
        let proc = Process()
        proc.executableURL = p.appendingPathComponent("python/bin/python3")
        proc.arguments = ["-I", "-B", p.appendingPathComponent("oot-online/setup/install.py").path, "--nup", nup.path, "--json"] + (server.isEmpty ? [] : ["--server", server]) + testArgs()
        proc.environment = ["HOME": NSHomeDirectory(), "USER": NSUserName(), "LOGNAME": NSUserName(), "TMPDIR": NSTemporaryDirectory(),
                            "PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "DEVELOPER_DIR": "/var/empty/no-xcode", "LANG": "en_US.UTF-8", "LC_ALL": "en_US.UTF-8"]
        let out = Pipe(), err = Pipe()
        proc.standardOutput = out; proc.standardError = err
        out.fileHandleForReading.readabilityHandler = { [weak self] h in
            let d = h.availableData
            guard !d.isEmpty else { h.readabilityHandler = nil; return }
            DispatchQueue.main.async { self?.feed(d) }
        }
        let errLog = support.appendingPathComponent("logs/installer-stderr.log")
        try? FileManager.default.createDirectory(at: errLog.deletingLastPathComponent(), withIntermediateDirectories: true)
        FileManager.default.createFile(atPath: errLog.path, contents: nil)
        let errFile = try? FileHandle(forWritingTo: errLog)
        err.fileHandleForReading.readabilityHandler = { h in
            let d = h.availableData
            if d.isEmpty { h.readabilityHandler = nil; try? errFile?.close(); return }
            errFile?.write(d)
        }
        proc.terminationHandler = { [weak self] pr in
            DispatchQueue.main.async {
                guard let self = self, self.state == .running else { return }
                self.fail(pr.terminationStatus == 15 ? "Stopped." : "The build stopped unexpectedly (exit code \(pr.terminationStatus)). The logs are in \(self.support.appendingPathComponent("logs").path).")
            }
        }
        do { try proc.run(); self.proc = proc } catch { fail("Could not start the build: \(error.localizedDescription)") }
    }

    func feed(_ d: Data) {
        buffer.append(d)
        while let nl = buffer.firstIndex(of: 0x0a) {
            let line = buffer.subdata(in: buffer.startIndex..<nl)
            buffer.removeSubrange(buffer.startIndex...nl)
            if let obj = try? JSONSerialization.jsonObject(with: line) as? [String: Any] { handle(obj) }
            else if let s = String(data: line, encoding: .utf8), !s.isEmpty { add(s) }
        }
    }

    func handle(_ e: [String: Any]) {
        let ev = e["ev"] as? String ?? ""
        let name = e["name"] as? String ?? ""
        if Model.testEnv["OOT_INSTALLER_NUP"] != nil, ["phase", "stage_end", "done", "error"].contains(ev) {   // a test run: what the window shows, on standard output
            print("installer: \(ev) \(name.isEmpty ? (e["msg"] as? String ?? e["app"] as? String ?? "") : name)"); fflush(stdout)
        }
        switch ev {
        case "phase":
            phaseTitle = name; phaseCount = "Step \(e["k"] as? Int ?? 0) of \(e["n"] as? Int ?? 0)"
            stage = ""; count = ""; progress = nil
            add("— \(name)")
        case "stage", "step":
            let step = e["step"] as? String ?? ""
            stageName = name
            stage = step.isEmpty ? Model.plain(name) : "\(Model.plain(name)) · \(step)"; count = ""; progress = nil
        case "progress":
            progress = e["frac"] as? Double
            var parts: [String] = []
            if let done = e["done"] as? Double {
                let unit = (e["unit"] as? String ?? "").isEmpty && stageName.hasPrefix("unpack") ? "B" : e["unit"] as? String ?? ""   // the unpacking counts bytes
                let total = e["total"] as? Double
                if unit == "B" {
                    let f = { (v: Double) in ByteCountFormatter.string(fromByteCount: Int64(v), countStyle: .file) }
                    parts.append(total != nil ? "\(f(done)) of \(f(total!))" : f(done))
                } else {
                    let n = NumberFormatter(); n.numberStyle = .decimal
                    let s = { (v: Double) in n.string(from: NSNumber(value: v)) ?? "\(Int(v))" }
                    parts.append((total != nil ? "\(s(done)) of \(s(total!))" : s(done)) + (unit.isEmpty ? "" : " \(unit)"))
                }
            }
            if let eta = e["eta"] as? Double { parts.append("about \(Model.duration(eta)) left") }
            count = parts.joined(separator: " · ")
        case "stage_end":
            let ok = e["ok"] as? Bool ?? true, warn = e["warn"] as? Bool ?? false
            let t = (e["elapsed"] as? Double).map { " (\(Model.duration($0)))" } ?? ""
            let sum = e["summary"] as? String ?? ""
            add("\(warn ? "⚠" : ok ? "✓" : "✗") \(Model.plain(name))\(t)\(sum.isEmpty ? "" : ": \(sum)")")
        case "say":
            let m = e["msg"] as? String ?? ""
            add(m)
            if m.hasPrefix("[RED]") { warning = String(m.dropFirst(6)) }
        case "game":
            let v = e["version"] as? String ?? ""
            game = (e["name"] as? String ?? "") + (v.isEmpty ? "" : " \(v)")
        case "done":
            appPath = e["app"] as? String ?? ""
            finish(); state = .done
        case "error":
            fail(e["msg"] as? String ?? "The build stopped.")
        default: break
        }
    }

    func tick() { elapsed = Model.duration(Date().timeIntervalSince(started)) }

    // nxnative's stage names, in a player's words
    static func plain(_ name: String) -> String {
        let words: [(String, String)] = [("unpack ", "Unpacking the game"), ("check ", "Checking the game's files"), ("rewrite", "Preparing the game's code"),
                                         ("shader split", "Finding the game's shaders"), ("Mac shaders", "Translating and compiling the shaders"),
                                         ("Mac host", "Setting up the game's host"), ("Mac app", "Putting the app together"), ("DLC", "Unpacking the add-on content")]
        for (prefix, plain) in words where name.hasPrefix(prefix) {
            let rest = name.dropFirst(prefix.count).trimmingCharacters(in: .whitespaces)
            return prefix.hasSuffix(" ") && !rest.isEmpty ? "\(plain) (\(rest))" : plain
        }
        return name
    }

    static func duration(_ s: Double) -> String {
        let t = Int(s.rounded())
        return t >= 3600 ? String(format: "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60) : String(format: "%d:%02d", t / 60, t % 60)
    }

    func finish() {
        timer?.invalidate(); timer = nil
        if sleepAssertion != 0 { IOPMAssertionRelease(sleepAssertion); sleepAssertion = 0 }
    }

    func fail(_ msg: String) {
        finish()
        error = msg; state = .failed
        if let p = proc, p.isRunning { killTree(p.processIdentifier) }
    }

    func stop() {
        guard let p = proc, p.isRunning else { return }
        state = .failed; error = "Stopped. What was already built is kept: run the installer again to continue."
        killTree(p.processIdentifier)
        finish()
    }

    // The build's process and everything it started (nxnative's build, the shader compiler, the translator), children first
    func killTree(_ pid: pid_t) {
        var kids = [pid_t](repeating: 0, count: 256)
        let n = proc_listchildpids(pid, &kids, Int32(kids.count * MemoryLayout<pid_t>.size))
        if n > 0 { for k in kids.prefix(Int(n)) where k > 0 { killTree(k) } }
        kill(pid, SIGTERM)
    }

    func openApp() { if !appPath.isEmpty { NSWorkspace.shared.open(URL(fileURLWithPath: appPath)) } }
    func showApp() { if !appPath.isEmpty { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: appPath)]) } }
    func showLogs() { NSWorkspace.shared.open(support.appendingPathComponent("logs")) }
}

struct ContentView: View {
    @EnvironmentObject var m: Model
    @State var details = false
    @State var options = false
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(spacing: 14) {
                Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 56, height: 56)
                VStack(alignment: .leading, spacing: 2) {
                    Text("OoT Online").font(.system(size: 28, weight: .bold))
                    Text("Ocarina of Time, together.").font(.title3).foregroundStyle(.secondary)
                }
            }
            switch m.state {
            case .welcome: welcome
            case .running: running
            case .done: done
            case .failed: failed
            }
            Spacer(minLength: 0)
            Text("A fan project, not affiliated with or endorsed by Nintendo. It contains nothing from the game, and nothing from the game leaves this Mac.")
                .font(.footnote).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .padding(28)
    }

    var welcome: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("This builds **Ocarina of Time Online** from your own copy of the game: a **.nup** file you dumped from your own Switch 2. Everything is made here, on this Mac.")
                .fixedSize(horizontal: false, vertical: true)
            Text("The first build takes one to three hours and about 60 GB of free space. Your Mac stays awake meanwhile. Building again later, to update, takes minutes.")
                .foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            if let nup = m.nup {
                HStack {
                    Image(systemName: "doc.fill").foregroundStyle(.secondary)
                    Text(nup.lastPathComponent).fontWeight(.medium)
                    Text(m.nupSize).foregroundStyle(.secondary)
                    Spacer()
                    Button("Choose another…") { m.choose() }
                }
                .padding(10).background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: 8))
            }
            HStack(spacing: 12) {
                if m.nup == nil {
                    Button { m.choose() } label: { Text("Choose your game file…").padding(.horizontal, 6) }
                        .controlSize(.large).buttonStyle(.borderedProminent)
                } else {
                    Button { m.start() } label: { Text("Build Ocarina of Time Online").padding(.horizontal, 6) }
                        .controlSize(.large).buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
                }
                Spacer()
                Button(options ? "Hide options" : "Options") { options.toggle() }.buttonStyle(.link)
            }
            if options {
                HStack {
                    Text("Server")
                    TextField("the public one", text: $m.server).textFieldStyle(.roundedBorder).frame(width: 260)
                    Text("address:port, for a friends' server").font(.caption).foregroundStyle(.secondary)
                }
            }
        }
    }

    var running: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                Text(m.phaseTitle).font(.title2.weight(.semibold))
                Spacer()
                Text(m.phaseCount).foregroundStyle(.secondary)
            }
            if !m.game.isEmpty { Text(m.game).foregroundStyle(.secondary) }
            Text(m.stage.isEmpty ? " " : m.stage).lineLimit(1).truncationMode(.middle)
            if let p = m.progress { ProgressView(value: p) } else { ProgressView().progressViewStyle(.linear) }
            HStack {
                Text(m.count).foregroundStyle(.secondary).monospacedDigit()
                Spacer()
                Text("Running for \(m.elapsed)").foregroundStyle(.secondary).monospacedDigit()
            }.font(.callout)
            DisclosureGroup("Details", isExpanded: $details) { logView.frame(height: 150) }
            HStack {
                Text("You can use your Mac meanwhile. Closing this window stops the build.").font(.callout).foregroundStyle(.secondary)
                Spacer()
                Button("Stop") { m.stop() }
            }
        }
    }

    var done: some View {
        VStack(alignment: .leading, spacing: 14) {
            Label("Ocarina of Time Online is ready", systemImage: "checkmark.circle.fill").font(.title2.weight(.semibold)).foregroundStyle(.green)
            Text("It is in the Applications folder of your home folder. Load your save, or start a New Game: online, a new game drops you in Hyrule Field. Your save is never written by online play.")
                .fixedSize(horizontal: false, vertical: true)
            HStack(spacing: 12) {
                Button { m.openApp() } label: { Text("Open Ocarina of Time Online").padding(.horizontal, 6) }
                    .controlSize(.large).buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
                Button("Show in Finder") { m.showApp() }
            }
            Text("To update later, open this installer again: it reuses what it built.").font(.callout).foregroundStyle(.secondary)
            DisclosureGroup("Details", isExpanded: $details) { logView.frame(height: 120) }
        }
    }

    var failed: some View {
        VStack(alignment: .leading, spacing: 14) {
            Label("The build stopped", systemImage: "exclamationmark.triangle.fill").font(.title2.weight(.semibold)).foregroundStyle(.orange)
            ScrollView { Text(m.error).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading) }.frame(maxHeight: 90)
            HStack(spacing: 12) {
                Button { m.start() } label: { Text("Try again").padding(.horizontal, 6) }.controlSize(.large).buttonStyle(.borderedProminent).disabled(m.nup == nil)
                Button("Show the logs") { m.showLogs() }
                Button("Choose another file…") { m.state = .welcome; m.choose() }
            }
            DisclosureGroup("Details", isExpanded: $details) { logView.frame(height: 120) }
        }
    }

    var logView: some View {
        ScrollViewReader { r in
            ScrollView {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(Array(m.log.enumerated()), id: \.offset) { i, l in
                        Text(l).font(.system(.caption, design: .monospaced)).foregroundStyle(l.hasPrefix("[RED]") ? Color.orange : Color.primary)
                            .textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading).id(i)
                    }
                }.padding(6)
            }
            .background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 6))
            .onChange(of: m.log.count) { _, n in if n > 0 { r.scrollTo(n - 1, anchor: .bottom) } }
        }
    }
}
