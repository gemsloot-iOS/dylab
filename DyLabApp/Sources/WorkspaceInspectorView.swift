import SwiftUI
import UniformTypeIdentifiers

struct WorkspaceInspectorView: View {
    @State private var workspaces: [URL] = []
    @State private var statusMessage = ""
    @State private var isBusy = false
    @State private var showDocumentPicker = false
    @State private var workspacePendingDelete: URL?
    @State private var confirmDelete = false

    private var workspacesRoot: URL {
        DyLabFileImport.workspacesRoot
    }

    var body: some View {
        NavigationStack {
            List {
                if !statusMessage.isEmpty {
                    Section {
                        Text(statusMessage)
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                }

                if workspaces.isEmpty {
                    Section {
                        VStack(spacing: 8) {
                            Image(systemName: "folder.fill")
                                .font(.largeTitle)
                                .foregroundStyle(.secondary)
                            Text("No Workspaces")
                                .font(.headline)
                            Text("Extract the sample tweak or import a .deb / .dylib.")
                                .font(.footnote)
                                .foregroundStyle(.secondary)
                                .multilineTextAlignment(.center)
                        }
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 24)
                    }
                } else {
                    ForEach(workspaces, id: \.path) { workspace in
                        Section {
                            ForEach(dylibs(in: workspace), id: \.path) { dylib in
                                NavigationLink {
                                    MachODetailView(dylibURL: dylib, workspaceURL: workspace)
                                } label: {
                                    Label {
                                        Text(dylib.lastPathComponent)
                                            .font(.body.monospaced())
                                    } icon: {
                                        Image(systemName: "doc.text.fill")
                                            .foregroundStyle(.secondary)
                                    }
                                }
                            }
                        } header: {
                            Label(workspace.lastPathComponent, systemImage: "folder.fill")
                        }
                        .swipeActions(edge: .trailing, allowsFullSwipe: true) {
                            Button(role: .destructive) {
                                workspacePendingDelete = workspace
                                confirmDelete = true
                            } label: {
                                Label("Delete", systemImage: "trash")
                            }
                        }
                        .contextMenu {
                            Button(role: .destructive) {
                                workspacePendingDelete = workspace
                                confirmDelete = true
                            } label: {
                                Label("Delete Workspace", systemImage: "trash")
                            }
                        }
                    }
                }
            }
            .listStyle(.insetGrouped)
            .scrollContentBackground(.hidden)
            .background(Color(.systemGroupedBackground))
            .navigationTitle("Workspaces")
            .toolbar { workspacesToolbar }
            .confirmationDialog(
                "Delete Workspace?",
                isPresented: $confirmDelete,
                titleVisibility: .visible
            ) {
                Button("Delete", role: .destructive) {
                    if let workspacePendingDelete {
                        deleteWorkspace(workspacePendingDelete)
                    }
                    workspacePendingDelete = nil
                }
                Button("Cancel", role: .cancel) {
                    workspacePendingDelete = nil
                }
            } message: {
                if let workspacePendingDelete {
                    Text("Remove \(workspacePendingDelete.lastPathComponent) and its files?")
                }
            }
            .onAppear(perform: refreshWorkspaces)
            .onReceive(NotificationCenter.default.publisher(for: .dylabImportStatus)) { note in
                if let message = note.userInfo?["message"] as? String {
                    isBusy = false
                    statusMessage = message
                    refreshWorkspaces()
                }
            }
            .onReceive(NotificationCenter.default.publisher(for: .dylabImportURL)) { _ in
                isBusy = true
                statusMessage = "Importing via Open in DyLab…"
            }
            .sheet(isPresented: $showDocumentPicker) {
                DocumentPicker(
                    onPick: { url in
                        showDocumentPicker = false
                        importPickedFile(url)
                    },
                    onCancel: {
                        showDocumentPicker = false
                    }
                )
                .ignoresSafeArea()
            }
        }
    }

    @ToolbarContentBuilder
    private var workspacesToolbar: some ToolbarContent {
        ToolbarItem(placement: .navigationBarLeading) {
            Button {
                extractBundledSample()
            } label: {
                Label("Sample", systemImage: "shippingbox.fill")
            }
            .disabled(isBusy)
        }
        ToolbarItem(placement: .navigationBarTrailing) {
            Button {
                showDocumentPicker = true
            } label: {
                Label("Import", systemImage: "square.and.arrow.up")
            }
            .disabled(isBusy)
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
                && !dylibs(in: url).isEmpty
        }
        .sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    private func deleteWorkspace(_ workspace: URL) {
        try? FileManager.default.removeItem(at: workspace)
        refreshWorkspaces()
        statusMessage = "Deleted workspace \(workspace.lastPathComponent)"
    }

    private func extractBundledSample() {
        guard let debURL = Bundle.main.url(forResource: "sample_tweak", withExtension: "deb") else {
            statusMessage = "sample_tweak.deb not found in app bundle"
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
                    statusMessage = "Extracted sample_tweak"
                } else {
                    statusMessage = "Extract failed: \(String(cString: err))"
                }
                refreshWorkspaces()
            }
        }
    }

    private func importPickedFile(_ url: URL) {
        isBusy = true
        statusMessage = "Importing \(url.lastPathComponent)…"
        DyLabFileImport.importURLAsync(url) { message in
            isBusy = false
            statusMessage = message
            refreshWorkspaces()
        }
    }
}
