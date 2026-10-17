import SwiftUI
import UniformTypeIdentifiers

struct WorkspaceInspectorView: View {
    @State private var workspaces: [URL] = []
    @State private var selectedDylib: URL?
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

                if selectedDylib != NULL {
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
            .padding()
            .navigationTitle("Workspaces & Inspector")
            .onAppear(perform: refreshWorkspaces)
            .fileImporter(
                isPresented: $showImporter,
                allowedContentTypes: [.data, .unixExecutable, .item],
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
        let accessed = url.startAccessingSecurityScopedResource()
        defer {
            if accessed {
                url.stopAccessingSecurityScopedResource()
            }
        }

        let ext = url.pathExtension.lowercased()
        isBusy = true

        DispatchQueue.global(qos: .userInitiated).async {
            var err = [CChar](repeating: 0, count: 1024)
            var message = ""
            if ext == "deb" {
                let name = url.deletingPathExtension().lastPathComponent
                let rc = extract_deb_to_workspace(
                    url.path,
                    workspacesRoot.path,
                    name,
                    &err,
                    err.count
                )
                message = rc == 0
                    ? "Extracted \(url.lastPathComponent) → Workspaces/\(name)"
                    : "Extract failed: \(String(cString: err))"
            } else if ext == "dylib" {
                let ws = workspacesRoot.appendingPathComponent("imported", isDirectory: true)
                try? FileManager.default.createDirectory(at: ws, withIntermediateDirectories: true)
                let rc = import_dylib_to_workspace(url.path, ws.path, &err, err.count)
                message = rc == 0
                    ? "Imported \(url.lastPathComponent) → Workspaces/imported"
                    : "Import failed: \(String(cString: err))"
            } else {
                message = "Unsupported file type: .\(ext) (use .deb or .dylib)"
            }

            DispatchQueue.main.async {
                isBusy = false
                statusMessage = message
                refreshWorkspaces()
            }
        }
    }

    private func inspect(dylib: URL) {
        selectedDylib = dylib
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
