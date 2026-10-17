import SwiftUI
import UniformTypeIdentifiers
import UIKit

/// Native document picker that accepts .deb / .dylib (and broad fallbacks).
struct DocumentPicker: UIViewControllerRepresentable {
    var onPick: (URL) -> Void
    var onCancel: (() -> Void)?

    static var allowedTypes: [UTType] {
        var types: [UTType] = [
            .item,
            .content,
            .data,
            .archive,
            .unixExecutable,
            .executable,
        ]
        if let deb = UTType(filenameExtension: "deb") {
            types.append(deb)
        }
        if let dylib = UTType(filenameExtension: "dylib") {
            types.append(dylib)
        }
        // UTType(importedAs:) is non-optional on current SDKs.
        types.append(UTType(importedAs: "org.debian.deb-archive"))
        types.append(UTType(importedAs: "com.apple.mach-o-dylib"))
        return types
    }

    func makeCoordinator() -> Coordinator {
        Coordinator(onPick: onPick, onCancel: onCancel)
    }

    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        let picker = UIDocumentPickerViewController(
            forOpeningContentTypes: Self.allowedTypes,
            asCopy: true
        )
        picker.allowsMultipleSelection = false
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ uiViewController: UIDocumentPickerViewController, context: Context) {}

    final class Coordinator: NSObject, UIDocumentPickerDelegate {
        let onPick: (URL) -> Void
        let onCancel: (() -> Void)?

        init(onPick: @escaping (URL) -> Void, onCancel: (() -> Void)?) {
            self.onPick = onPick
            self.onCancel = onCancel
        }

        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
            guard let url = urls.first else {
                onCancel?()
                return
            }
            onPick(url)
        }

        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
            onCancel?()
        }
    }
}
