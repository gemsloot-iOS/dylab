import SwiftUI
import UIKit

/// UIActivityViewController wrapper for Export .deb / .dylib.
struct ActivityView: UIViewControllerRepresentable {
    let items: [Any]

    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: items, applicationActivities: nil)
    }

    func updateUIViewController(_ uiViewController: UIActivityViewController, context: Context) {}
}

/// Retains a Swift closure for C log callbacks during a build.
final class BuildLogBox {
    private let handler: (String) -> Void
    init(_ handler: @escaping (String) -> Void) {
        self.handler = handler
    }
    func append(_ line: String) {
        handler(line)
    }
}
