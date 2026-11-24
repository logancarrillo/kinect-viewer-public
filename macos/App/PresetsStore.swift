import Foundation

struct ViewerPreset: Codable, Equatable, Identifiable {
    var name: String
    var isBuiltIn: Bool

    var modeRaw: Int
    var sbsLayoutRaw: Int
    var pipelineRaw: Int

    // IR mapping
    var irAutoScale: Bool
    var irGamma: Double
    var irSmoothingAlpha: Double
    var irLowPercentile: Double
    var irHighPercentile: Double
    var irManualMin: Double
    var irManualMax: Double

    // Depth colormap & distance window
    var depthColormapRaw: Int
    var depthMinMm: Double
    var depthMaxMm: Double
    var depthInvertNearWarm: Bool

    // Exposure & gain
    var exposureModeRaw: Int
    var autoExposureComp: Double
    var semiAutoPseudoMs: Double
    var manualIntegrationMs: Double
    var manualAnalogGain: Double

    // Orientation
    var mirrorHorizontal: Bool
    var rotate180: Bool

    // Optional Phase 3 settings (backward compatible with earlier saved JSON presets)
    var cutoffEnabled: Bool? = nil
    var cutoffNearMm: Double? = nil
    var cutoffFarMm: Double? = nil
    var pointCloudSize: Double? = nil
    var pointCloudColorSourceRaw: Int? = nil

    var id: String { "\(isBuiltIn ? "builtin" : "user"):\(name)" }

    static let builtInDesk = ViewerPreset(
        name: "Desk",
        isBuiltIn: true,
        modeRaw: ViewerModeOption.depth.rawValue,
        sbsLayoutRaw: SbsLayoutOption.irDepth.rawValue,
        pipelineRaw: PipelineOption.openGL.rawValue,
        irAutoScale: true,
        irGamma: 0.45,
        irSmoothingAlpha: 0.12,
        irLowPercentile: 0.01,
        irHighPercentile: 0.995,
        irManualMin: 0.0,
        irManualMax: 20000.0,
        depthColormapRaw: ColormapOption.turbo.rawValue,
        depthMinMm: 400.0,
        depthMaxMm: 2200.0,
        depthInvertNearWarm: true,
        exposureModeRaw: ExposureModeOption.semiAuto.rawValue,
        autoExposureComp: 0.0,
        semiAutoPseudoMs: 16.0,
        manualIntegrationMs: 16.6,
        manualAnalogGain: 1.5,
        mirrorHorizontal: false,
        rotate180: false
    )

    static let builtInRoom = ViewerPreset(
        name: "Room",
        isBuiltIn: true,
        modeRaw: ViewerModeOption.depth.rawValue,
        sbsLayoutRaw: SbsLayoutOption.irDepth.rawValue,
        pipelineRaw: PipelineOption.openGL.rawValue,
        irAutoScale: true,
        irGamma: 0.45,
        irSmoothingAlpha: 0.12,
        irLowPercentile: 0.01,
        irHighPercentile: 0.995,
        irManualMin: 0.0,
        irManualMax: 20000.0,
        depthColormapRaw: ColormapOption.viridis.rawValue,
        depthMinMm: 500.0,
        depthMaxMm: 6000.0,
        depthInvertNearWarm: true,
        exposureModeRaw: ExposureModeOption.auto.rawValue,
        autoExposureComp: 0.0,
        semiAutoPseudoMs: 16.0,
        manualIntegrationMs: 16.6,
        manualAnalogGain: 1.5,
        mirrorHorizontal: false,
        rotate180: false
    )

    static let builtInDarkRoomIR = ViewerPreset(
        name: "Dark room IR",
        isBuiltIn: true,
        modeRaw: ViewerModeOption.ir.rawValue,
        sbsLayoutRaw: SbsLayoutOption.irDepth.rawValue,
        pipelineRaw: PipelineOption.openGL.rawValue,
        irAutoScale: true,
        irGamma: 0.36,
        irSmoothingAlpha: 0.16,
        irLowPercentile: 0.005,
        irHighPercentile: 0.995,
        irManualMin: 0.0,
        irManualMax: 14000.0,
        depthColormapRaw: ColormapOption.inferno.rawValue,
        depthMinMm: 500.0,
        depthMaxMm: 4500.0,
        depthInvertNearWarm: true,
        exposureModeRaw: ExposureModeOption.manual.rawValue,
        autoExposureComp: 0.0,
        semiAutoPseudoMs: 33.0,
        manualIntegrationMs: 33.3,
        manualAnalogGain: 3.0,
        mirrorHorizontal: false,
        rotate180: false
    )

    static let builtInPresets: [ViewerPreset] = [
        .builtInDesk,
        .builtInRoom,
        .builtInDarkRoomIR,
    ]
}

private struct PersistedPresetsEnvelope: Codable {
    var customPresets: [ViewerPreset]
    var lastUsedSettings: ViewerPreset?
}

final class PresetsStore {
    let fileURL: URL
    private(set) var customPresets: [ViewerPreset] = []
    private(set) var lastUsedSettings: ViewerPreset?

    var allPresets: [ViewerPreset] {
        ViewerPreset.builtInPresets + customPresets
    }

    init(stateDirectoryURL: URL? = nil) {
        let dir = stateDirectoryURL ?? HostStreamServer.defaultStateDirectoryURL
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        self.fileURL = dir.appendingPathComponent("presets.json")
        reload()
    }

    func reload() {
        guard let data = try? Data(contentsOf: fileURL),
              let decoded = try? JSONDecoder().decode(PersistedPresetsEnvelope.self, from: data) else {
            customPresets = []
            lastUsedSettings = nil
            return
        }
        customPresets = decoded.customPresets.map { p in
            var copy = p
            copy.isBuiltIn = false
            return copy
        }
        lastUsedSettings = decoded.lastUsedSettings
    }

    @discardableResult
    func saveCustomPreset(_ preset: ViewerPreset) -> Bool {
        let trimmed = preset.name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return false }
        if ViewerPreset.builtInPresets.contains(where: { $0.name.caseInsensitiveCompare(trimmed) == .orderedSame }) {
            return false
        }
        var clean = preset
        clean.name = trimmed
        clean.isBuiltIn = false

        if let idx = customPresets.firstIndex(where: { $0.name.caseInsensitiveCompare(trimmed) == .orderedSame }) {
            customPresets[idx] = clean
        } else {
            customPresets.append(clean)
        }
        persist()
        return true
    }

    func deleteCustomPreset(named name: String) {
        customPresets.removeAll { $0.name == name && !$0.isBuiltIn }
        persist()
    }

    func saveLastUsedSettings(_ settings: ViewerPreset) {
        var copy = settings
        copy.name = "_last_used"
        copy.isBuiltIn = false
        lastUsedSettings = copy
        persist()
    }

    private func persist() {
        let env = PersistedPresetsEnvelope(customPresets: customPresets, lastUsedSettings: lastUsedSettings)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        guard let data = try? encoder.encode(env) else { return }
        let tmp = fileURL.appendingPathExtension("tmp")
        do {
            try data.write(to: tmp, options: .atomic)
            try? FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: tmp.path)
            if FileManager.default.fileExists(atPath: fileURL.path) {
                _ = try FileManager.default.replaceItemAt(fileURL, withItemAt: tmp)
            } else {
                try FileManager.default.moveItem(at: tmp, to: fileURL)
            }
        } catch {
            try? FileManager.default.removeItem(at: tmp)
        }
    }
}
