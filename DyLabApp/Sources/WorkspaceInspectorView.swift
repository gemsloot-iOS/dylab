import SwiftUI
import UniformTypeIdentifiers

struct WorkspaceInspectorView: View {
    @State private var workspaces: [URL] = []
    @State private var selectedDylibPath: String?
    @State private var showMachODetail = false
    @State private var machoJSON = ""
    @State private var statusMessage = ""
    @State private var isBusy = false
    @State private var showImporter = false

    private var documentsURL: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
    }

    private var workspacesRoot: URL {
        documentsURL.appendingPathComponent("Workspaces", isDirectory: true)
    }

    var body: some View {
        NavigationStack {
            VStack(spacing: 12) {
                HStack(spacing: 8) {
                    Button("Extract sample_tweak.deb") {
                        extractBundledSample()
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(isBusy)

                    Button("Import .deb / .dylib") {
                        showImporter = true
                    }
                    .buttonStyle(.bordered)
                    .disabled(isBusy)
                }

                if !statusMessage.isEmpty {
                    Text(statusMessage)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }

                List {
                    ForEach(workspaces, id: \.path) { workspace in
                        Section(workspace.lastPathComponent) {
                            ForEach(dylibs(in: workspace), id: \.path) { dylib in
                                Button {
                                    inspect(dylib: dylib)
                                } label: {
                                    HStack {
                                        Text(dylib.lastPathComponent)
                                            .font(.body.monospaced())
                                        Spacer()
                                        Image(systemName: "chevron.right")
                                            .foregroundStyle(.secondary)
                                    }
                                }
                            }
                        }
                    }
                }
                .listStyle(.insetGrouped)

                // Use Bool (not Optional URL?) in ViewBuilder — avoids iOS 16
                // "URL? to ()" overload ambiguity under NavigationStack.
                if showMachODetail {
                    VStack(alignment: .leading, spacing: 6) {
                        if let selectedDylibPath {
                            Text(selectedDylibPath)
                                .font(.caption.monospaced())
                                .foregroundStyle(.secondary)
                                .lineLimit(2)
                        }
                        ScrollView {
                            Text(machoJSON.isEmpty ? "(no report)" : machoJSON)
                                .font(.system(.caption, design: .monospaced))
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .textSelection(.enabled)
                                .padding(8)
                        }
                        .frame(minHeight: 180)
                        .background(Color(white: 0.08))
                        .foregroundStyle(Color(white: 0.92))
                        .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                    }
                }
            }
            .padding()
            .navigationTitle("Workspaces & Inspector")
            .onAppear(perform: refreshWorkspaces)
            .fileImporter(
                isPresented: $showImporter,
                allowedContentTypes: [.item, .data],
                allowsMultipleSelection: false
            ) { result in
                switch result {
                case .success(let urls):
                    if let url = urls.first {
                        importFile(url)
                    }
                case .failure(let error):
                    statusMessage = "Import failed: \(error.localizedDescription)"
                }
            }
        }
    }

    private func dylibs(in workspace: URL) -> [URL] {
        let fm = FileManager.default
        guard let enumerator = fm.enumerator(at: workspace, includingPropertiesForKeys: nil) else {
            return []
        }
        var found: [URL] = []
        for case let fileURL as URL in enumerator {
            if fileURL.pathExtension.lowercased() == "dylib" {
                found.append(fileURL)
            }
        }
        return found.sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    private func refreshWorkspaces() {
        let fm = FileManager.default
        try? fm.createDirectory(at: workspacesRoot, withIntermediateDirectories: true)
        let contents = (try? fm.contentsOfDirectory(
            at: workspacesRoot,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]
        )) ?? []
        workspaces = contents.filter { url in
            (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true
        }
        .sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    private func extractBundledSample() {
        guard let debURL = Bundle.main.url(forResource: "sample_tweak", withExtension: "deb") else {
            statusMessage = "sample_tweak.deb not found in app bundle (CI builds it via scripts/build_sample_deb.sh)"
            return
        }
        isBusy = true
        statusMessage = "Extracting sample_tweak.deb…"
        DispatchQueue.global(qos: .userInitiated).async {
            var err = [CChar](repeating: 0, count: 1024)
            let rc = extract_deb_to_workspace(
                debURL.path,
                workspacesRoot.path,
                "sample_tweak",
                &err,
                err.count
            )
            DispatchQueue.main.async {
                isBusy = false
                if rc == 0 {
                    statusMessage = "Extracted sample_tweak → Workspaces/sample_tweak"
                } else {
                    statusMessage = "Extract failed: \(String(cString: err))"
                }
                refreshWorkspaces()
            }
        }
    }

    private func importFile(_ url: URL) {
        let ext = url.pathExtension.lowercased()
        guard ext == "deb" || ext == "dylib" else {
            statusMessage = "Unsupported file type: .\(ext) (use .deb or .dylib)"
            return
        }

        isBusy = true
        statusMessage = "Importing \(url.lastPathComponent)…"

        // Hold security-scoped access only long enough to copy into Documents.
        let gotAccess = url.startAccessingSecurityScopedResource()
        defer {
            if gotAccess {
                url.stopAccessingSecurityScopedResource()
            }
        }

        let stagingDir = documentsURL.appendingPathComponent("ImportStaging", isDirectory: true)
        try? FileManager.default.createDirectory(at: stagingDir, withIntermediateDirectories: true)
        let staged = stagingDir.appendingPathComponent(url.lastPathComponent)

        do {
            if FileManager.default.fileExists(atPath: staged.path) {
                try FileManager.default.removeItem(at: staged)
            }
            try FileManager.default.copyItem(at: url, to: staged)
        } catch {
            isBusy = false
            statusMessage = "Import copy failed: \(error.localizedDescription)"
            return
        }

        DispatchQueue.global(qos: .userInitiated).async {
            var err = [CChar](repeating: 0, count: 2048)
            var message = ""
            if ext == "deb" {
                let name = staged.deletingPathExtension().lastPathComponent
                let rc = extract_deb_to_workspace(
                    staged.path,
                    workspacesRoot.path,
                    name,
                    &err,
                    err.count
                )
                message = rc == 0
                    ? "Extracted \(staged.lastPathComponent) → Workspaces/\(name)"
                    : "Extract failed: \(String(cString: err))"
            } else {
                let ws = workspacesRoot.appendingPathComponent("imported", isDirectory: true)
                try? FileManager.default.createDirectory(at: ws, withIntermediateDirectories: true)
                let rc = import_dylib_to_workspace(staged.path, ws.path, &err, err.count)
                message = rc == 0
                    ? "Imported \(staged.lastPathComponent) → Workspaces/imported"
                    : "Import failed: \(String(cString: err))"
            }

            DispatchQueue.main.async {
                isBusy = false
                statusMessage = message
                refreshWorkspaces()
            }
        }
    }

    private func inspect(dylib: URL) {
        selectedDylibPath = dylib.lastPathComponent
        showMachODetail = true
        machoJSON = "Inspecting \(dylib.lastPathComponent)…"
        DispatchQueue.global(qos: .userInitiated).async {
            var buf = [CChar](repeating: 0, count: 512 * 1024)
            let rc = inspect_macho(dylib.path, &buf, buf.count)
            let text = String(cString: buf)
            let pretty = prettyJSON(text) ?? text
            DispatchQueue.main.async {
                machoJSON = rc == 0 ? pretty : "inspect_macho failed (\(rc))\n\(pretty)"
            }
        }
    }

    private func prettyJSON(_ raw: String) -> String? {
        guard let data = raw.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data),
              let pretty = try? JSONSerialization.data(withJSONObject: obj, options: [.prettyPrinted, .sortedKeys]),
              let text = String(data: pretty, encoding: .utf8) else {
            return nil
        }
        return text
    }
}
