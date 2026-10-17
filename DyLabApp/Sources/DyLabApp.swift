import SwiftUI

@main
struct DyLabApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
                .onOpenURL { url in
                    handleOpenInDyLab(url)
                }
        }
    }

    private func handleOpenInDyLab(_ url: URL) {
        let ext = url.pathExtension.lowercased()
        guard ext == "deb" || ext == "dylib" else {
            NotificationCenter.default.post(
                name: .dylabImportStatus,
                object: nil,
                userInfo: ["message": "Open in DyLab: unsupported type .\(ext)"]
            )
            return
        }

        // Notify Workspaces tab / status, then run the same import path as the picker.
        NotificationCenter.default.post(name: .dylabImportURL, object: url)
        DyLabFileImport.importURLAsync(url) { message in
            NotificationCenter.default.post(
                name: .dylabImportStatus,
                object: nil,
                userInfo: ["message": message]
            )
        }
    }
}
