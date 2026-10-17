import Foundation

/// Shared .deb / .dylib import into Documents/Workspaces (picker + Open in DyLab).
enum DyLabFileImport {
    static var documentsURL: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
    }

    static var workspacesRoot: URL {
        documentsURL.appendingPathComponent("Workspaces", isDirectory: true)
    }

    /// Stage `url` into Documents (asCopy temp or security-scoped), then extract/import.
    /// Returns a user-facing status string. Call off the main thread for extract work,
    /// or use `importURLAsync`.
    static func importURL(_ url: URL) -> String {
        let ext = url.pathExtension.lowercased()
        guard ext == "deb" || ext == "dylib" else {
            return "Unsupported file type: .\(ext) (use .deb or .dylib)"
        }

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
            return "Import copy failed: \(error.localizedDescription)"
        }

        var err = [CChar](repeating: 0, count: 2048)
        if ext == "deb" {
            let name = staged.deletingPathExtension().lastPathComponent
            let rc = extract_deb_to_workspace(
                staged.path,
                workspacesRoot.path,
                name,
                &err,
                err.count
            )
            return rc == 0
                ? "Extracted \(staged.lastPathComponent) → Workspaces/\(name)"
                : "Extract failed: \(String(cString: err))"
        }

        let ws = workspacesRoot.appendingPathComponent("imported", isDirectory: true)
        try? FileManager.default.createDirectory(at: ws, withIntermediateDirectories: true)
        let rc = import_dylib_to_workspace(staged.path, ws.path, &err, err.count)
        return rc == 0
            ? "Imported \(staged.lastPathComponent) → Workspaces/imported"
            : "Import failed: \(String(cString: err))"
    }

    static func importURLAsync(_ url: URL, completion: @escaping (String) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            let message = importURL(url)
            DispatchQueue.main.async {
                completion(message)
            }
        }
    }
}

extension Notification.Name {
    static let dylabImportURL = Notification.Name("dylabImportURL")
    static let dylabImportStatus = Notification.Name("dylabImportStatus")
}
