import SwiftUI

/// Mach-O inspector: Summary | Classes & Functions | Source Editor.
struct MachODetailView: View {
    let dylibURL: URL
    let workspaceURL: URL

    @State private var selectedTab = 0
    @State private var summaryJSON = "Loading…"
    @State private var classesJSON = "Loading…"
    @State private var tweakSource = ""
    @State private var tweakPath: URL?
    @State private var statusMessage = ""
    @State private var isBusy = false
    @State private var classEntries: [ObjCClassEntry] = []
    @State private var callableEntries: [CallableEntry] = []

    private let tabLabels = ["Summary", "Classes", "Source"]

    var body: some View {
        List {
            Section {
                Picker("Tab", selection: $selectedTab) {
                    ForEach(0..<tabLabels.count, id: \.self) { i in
                        Text(tabLabels[i]).tag(i)
                    }
                }
                .pickerStyle(.segmented)
                .listRowInsets(EdgeInsets(top: 8, leading: 16, bottom: 8, trailing: 16))

                if !statusMessage.isEmpty {
                    Text(statusMessage)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }

            switch selectedTab {
            case 0:
                summarySections
            case 1:
                classesSections
            default:
                sourceSections
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .background(Color(.systemGroupedBackground))
        .navigationTitle(dylibURL.lastPathComponent)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar { machOToolbar }
        .onAppear(perform: loadAll)
    }

    @ToolbarContentBuilder
    private var machOToolbar: some ToolbarContent {
        ToolbarItemGroup(placement: .navigationBarTrailing) {
            if selectedTab == 2 {
                Button {
                    generateSource()
                } label: {
                    Label("Generate", systemImage: "doc.text.fill")
                }
                .disabled(isBusy)

                Button {
                    saveSource()
                } label: {
                    Label("Save", systemImage: "square.and.arrow.down")
                }
                .disabled(isBusy || tweakPath == nil)
            }
        }
    }

    @ViewBuilder
    private var summarySections: some View {
        Section {
            Text(summaryJSON)
                .font(.system(.caption, design: .monospaced))
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        } header: {
            Label("Mach-O Summary", systemImage: "doc.text.fill")
        }
    }

    @ViewBuilder
    private var classesSections: some View {
        Section {
            if classEntries.isEmpty {
                Text("No ObjC classes found")
                    .foregroundStyle(.secondary)
            }
            ForEach(classEntries) { cls in
                DisclosureGroup {
                    ForEach(cls.methods) { method in
                        NavigationLink {
                            FunctionDecompilerView(
                                dylibPath: dylibURL.path,
                                title: "\(cls.name).\(method.name)",
                                fileOffset: method.fileOffset,
                                vma: method.vma,
                                size: method.size
                            )
                        } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(method.name)
                                    .font(.body.monospaced())
                                Text("imp \(method.vmaHex)  off \(method.fileOffset)  size \(method.size)")
                                    .font(.caption2.monospaced())
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                    if !cls.properties.isEmpty {
                        Text("props: " + cls.properties.joined(separator: ", "))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    if !cls.ivars.isEmpty {
                        Text("ivars: " + cls.ivars.joined(separator: ", "))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                } label: {
                    Text(cls.name)
                        .font(.body.monospaced())
                }
            }
        } header: {
            Label("ObjC Classes", systemImage: "doc.text.fill")
        }

        Section {
            if callableEntries.isEmpty {
                Text("No callables")
                    .foregroundStyle(.secondary)
            }
            ForEach(callableEntries) { fn in
                NavigationLink {
                    FunctionDecompilerView(
                        dylibPath: dylibURL.path,
                        title: fn.name,
                        fileOffset: fn.fileOffset,
                        vma: fn.vma,
                        size: fn.size
                    )
                } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(fn.name)
                            .font(.body.monospaced())
                        Text("\(fn.kind)  \(fn.vmaHex)  off \(fn.fileOffset)  size \(fn.size)")
                            .font(.caption2.monospaced())
                            .foregroundStyle(.secondary)
                    }
                }
            }
        } header: {
            Text("Callables (__TEXT,__text)")
        }
    }

    @ViewBuilder
    private var sourceSections: some View {
        Section {
            HStack(spacing: 12) {
                Button {
                    generateSource()
                } label: {
                    Label("Generate Source", systemImage: "doc.text.fill")
                }
                .buttonStyle(.borderedProminent)
                .disabled(isBusy)

                Button {
                    saveSource()
                } label: {
                    Label("Save", systemImage: "square.and.arrow.down")
                }
                .buttonStyle(.bordered)
                .disabled(isBusy || tweakPath == nil)
            }
            .listRowInsets(EdgeInsets(top: 10, leading: 16, bottom: 10, trailing: 16))

            if let tweakPath {
                LabeledContent("Path") {
                    Text(tweakPath.lastPathComponent)
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                }
            }
        } header: {
            Label("Tweak.m", systemImage: "doc.text.fill")
        }

        Section {
            TextEditor(text: $tweakSource)
                .font(.system(.body, design: .monospaced))
                .frame(minHeight: 320)
                .listRowInsets(EdgeInsets(top: 8, leading: 12, bottom: 8, trailing: 12))
        }
    }

    private func loadAll() {
        loadSummary()
        loadClasses()
        loadExistingTweak()
    }

    private func loadSummary() {
        DispatchQueue.global(qos: .userInitiated).async {
            var buf = [CChar](repeating: 0, count: 512 * 1024)
            let rc = inspect_macho(dylibURL.path, &buf, buf.count)
            let text = String(cString: buf)
            let pretty = Self.prettyJSON(text) ?? text
            DispatchQueue.main.async {
                summaryJSON = rc == 0 ? pretty : "inspect_macho failed (\(rc))\n\(pretty)"
            }
        }
    }

    private func loadClasses() {
        DispatchQueue.global(qos: .userInitiated).async {
            var buf = [CChar](repeating: 0, count: 1024 * 1024)
            let rc = objc_dump_classes_json(dylibURL.path, &buf, buf.count)
            let text = String(cString: buf)
            let pretty = Self.prettyJSON(text) ?? text
            let parsed = Self.parseDump(text)
            DispatchQueue.main.async {
                classesJSON = rc == 0 ? pretty : "objc_dump failed (\(rc))\n\(pretty)"
                classEntries = parsed.classes
                callableEntries = parsed.callables
            }
        }
    }

    private func srcDir() -> URL {
        workspaceURL.appendingPathComponent("src", isDirectory: true)
    }

    private func loadExistingTweak() {
        let path = srcDir().appendingPathComponent("Tweak.m")
        if let data = try? String(contentsOf: path, encoding: .utf8) {
            tweakSource = data
            tweakPath = path
        }
    }

    private func generateSource() {
        isBusy = true
        statusMessage = "Generating Tweak.m…"
        let out = srcDir()
        DispatchQueue.global(qos: .userInitiated).async {
            try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
            var hdr = [CChar](repeating: 0, count: 1024)
            var twk = [CChar](repeating: 0, count: 1024)
            var err = [CChar](repeating: 0, count: 1024)
            let rc = objc_generate_header_and_skeleton(
                dylibURL.path,
                out.path,
                &hdr,
                hdr.count,
                &twk,
                twk.count,
                &err,
                err.count
            )
            let tweakWritten = String(cString: twk)
            let errText = String(cString: err)
            var source = ""
            if rc == 0, !tweakWritten.isEmpty {
                source = (try? String(contentsOfFile: tweakWritten, encoding: .utf8)) ?? ""
            }
            DispatchQueue.main.async {
                isBusy = false
                if rc == 0 {
                    tweakPath = URL(fileURLWithPath: tweakWritten)
                    tweakSource = source
                    selectedTab = 2
                    statusMessage = "Wrote \(tweakWritten)"
                } else {
                    statusMessage = "Generate failed: \(errText.isEmpty ? "rc=\(rc)" : errText)"
                }
            }
        }
    }

    private func saveSource() {
        guard let tweakPath else { return }
        do {
            try tweakSource.write(to: tweakPath, atomically: true, encoding: .utf8)
            statusMessage = "Saved \(tweakPath.lastPathComponent)"
        } catch {
            statusMessage = "Save failed: \(error.localizedDescription)"
        }
    }

    static func prettyJSON(_ raw: String) -> String? {
        guard let data = raw.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data),
              let pretty = try? JSONSerialization.data(withJSONObject: obj, options: [.prettyPrinted, .sortedKeys]),
              let text = String(data: pretty, encoding: .utf8) else {
            return nil
        }
        return text
    }

    private static func parseDump(_ raw: String) -> (classes: [ObjCClassEntry], callables: [CallableEntry]) {
        guard let data = raw.data(using: .utf8),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return ([], [])
        }
        var classes: [ObjCClassEntry] = []
        if let arr = root["classes"] as? [[String: Any]] {
            for (idx, c) in arr.enumerated() {
                let name = c["name"] as? String ?? "Class\(idx)"
                let vma = parseHex(c["vma"] as? String)
                var methods: [ObjCMethodEntry] = []
                if let ms = c["methods"] as? [[String: Any]] {
                    for (mi, m) in ms.enumerated() {
                        let mname = m["name"] as? String ?? "meth\(mi)"
                        let mVma = parseHex(m["imp"] as? String)
                        let off = (m["file_offset"] as? NSNumber)?.uint64Value
                            ?? UInt64(m["file_offset"] as? Int ?? 0)
                        let sz = (m["size"] as? NSNumber)?.uint64Value
                            ?? UInt64(m["size"] as? Int ?? 0)
                        methods.append(ObjCMethodEntry(
                            id: "\(name).\(mname).\(mi)",
                            name: mname,
                            vma: mVma,
                            fileOffset: off,
                            size: sz
                        ))
                    }
                }
                let props = ((c["properties"] as? [[String: Any]]) ?? []).compactMap { $0["name"] as? String }
                let ivars = ((c["ivars"] as? [[String: Any]]) ?? []).compactMap { $0["name"] as? String }
                classes.append(ObjCClassEntry(
                    id: "\(name).\(idx)",
                    name: name,
                    vma: vma,
                    methods: methods,
                    properties: props,
                    ivars: ivars
                ))
            }
        }
        var callables: [CallableEntry] = []
        if let arr = root["callables"] as? [[String: Any]] {
            for (idx, f) in arr.enumerated() {
                let name = f["name"] as? String ?? "sub_\(idx)"
                let kind = f["kind"] as? String ?? "symbol"
                let vma = parseHex(f["vma"] as? String)
                let off = (f["file_offset"] as? NSNumber)?.uint64Value
                    ?? UInt64(f["file_offset"] as? Int ?? 0)
                let sz = (f["size"] as? NSNumber)?.uint64Value
                    ?? UInt64(f["size"] as? Int ?? 0)
                callables.append(CallableEntry(
                    id: "\(kind).\(name).\(idx)",
                    name: name,
                    kind: kind,
                    vma: vma,
                    fileOffset: off,
                    size: sz
                ))
            }
        }
        return (classes, callables)
    }

    private static func parseHex(_ s: String?) -> UInt64 {
        guard var t = s else { return 0 }
        if t.hasPrefix("0x") || t.hasPrefix("0X") {
            t = String(t.dropFirst(2))
        }
        return UInt64(t, radix: 16) ?? 0
    }
}

struct ObjCClassEntry: Identifiable {
    let id: String
    let name: String
    let vma: UInt64
    let methods: [ObjCMethodEntry]
    let properties: [String]
    let ivars: [String]
}

struct ObjCMethodEntry: Identifiable {
    let id: String
    let name: String
    let vma: UInt64
    let fileOffset: UInt64
    let size: UInt64

    var vmaHex: String { String(format: "0x%llx", vma) }
}

struct CallableEntry: Identifiable {
    let id: String
    let name: String
    let kind: String
    let vma: UInt64
    let fileOffset: UInt64
    let size: UInt64

    var vmaHex: String { String(format: "0x%llx", vma) }
}
