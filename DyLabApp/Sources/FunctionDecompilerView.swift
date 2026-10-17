import SwiftUI
import UIKit

/// Per-function decompiler: Pseudo-C / ARM64 / Structured.
struct FunctionDecompilerView: View {
    let dylibPath: String
    let title: String
    let fileOffset: UInt64
    let vma: UInt64
    let size: UInt64

    @State private var modeIndex = 0
    @State private var output = "Decompiling…"
    @State private var isBusy = false

    private let modeLabels = ["Pseudo-C", "ARM64", "Structured"]
    private let modes: [DecompileMode] = [
        DECOMPILE_MODE_HEURISTIC_C,
        DECOMPILE_MODE_DISASM,
        DECOMPILE_MODE_STRUCTURED
    ]

    var body: some View {
        List {
            Section {
                Picker("Mode", selection: $modeIndex) {
                    ForEach(0..<modeLabels.count, id: \.self) { i in
                        Text(modeLabels[i]).tag(i)
                    }
                }
                .pickerStyle(.segmented)
                .listRowInsets(EdgeInsets(top: 8, leading: 16, bottom: 8, trailing: 16))
                .onChange(of: modeIndex) { _ in
                    runDecompile()
                }

                LabeledContent("Offset") {
                    Text("\(fileOffset)")
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                }
                LabeledContent("VMA") {
                    Text(String(format: "0x%llx", vma))
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                }
                LabeledContent("Size") {
                    Text("\(size)")
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                }
            }

            Section {
                if isBusy {
                    HStack(spacing: 10) {
                        ProgressView()
                        Text("Decompiling…")
                            .foregroundStyle(.secondary)
                    }
                }
                Text(output)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            } header: {
                Label("Output", systemImage: "terminal.fill")
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .background(Color(.systemGroupedBackground))
        .navigationTitle(title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button {
                    UIPasteboard.general.string = output
                } label: {
                    Label("Copy", systemImage: "doc.on.doc")
                }
                .disabled(isBusy || output.isEmpty)
            }
        }
        .onAppear(perform: runDecompile)
    }

    private func runDecompile() {
        isBusy = true
        output = "Decompiling…"
        let mode = modes[modeIndex]
        let path = dylibPath
        let off = fileOffset
        let sz = size
        let v = vma
        DispatchQueue.global(qos: .userInitiated).async {
            var buf = [CChar](repeating: 0, count: 512 * 1024)
            let rc = decompile_function(path, off, sz, v, mode, &buf, buf.count)
            let text = String(cString: buf)
            DispatchQueue.main.async {
                isBusy = false
                if rc == 0 || !text.isEmpty {
                    output = text
                } else {
                    output = "decompile_function failed (\(rc))\n\(text)"
                }
            }
        }
    }
}
