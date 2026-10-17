import SwiftUI

/// Per-function decompiler: Pseudo-C / ARM64 / Ghidra (3B placeholder).
struct FunctionDecompilerView: View {
    let dylibPath: String
    let title: String
    let fileOffset: UInt64
    let vma: UInt64
    let size: UInt64

    @State private var modeIndex = 0
    @State private var output = "Decompiling…"
    @State private var isBusy = false

    private let modeLabels = ["Pseudo-C", "ARM64", "Ghidra (3B)"]
    private let modes: [DecompileMode] = [
        DECOMPILE_MODE_HEURISTIC_C,
        DECOMPILE_MODE_DISASM,
        DECOMPILE_MODE_GHIDRA_C
    ]

    var body: some View {
        VStack(spacing: 0) {
            Picker("Mode", selection: $modeIndex) {
                ForEach(0..<modeLabels.count, id: \.self) { i in
                    Text(modeLabels[i]).tag(i)
                }
            }
            .pickerStyle(.segmented)
            .padding()
            .onChange(of: modeIndex) { _ in
                runDecompile()
            }

            Text("off \(fileOffset)  vma \(String(format: "0x%llx", vma))")
                .font(.caption2.monospaced())
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal)

            ScrollView {
                Text(output)
                    .font(.system(.caption, design: .monospaced))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
                    .padding(12)
            }
            .background(Color(white: 0.07))
            .foregroundStyle(Color(white: 0.93))
        }
        .navigationTitle(title)
        .navigationBarTitleDisplayMode(.inline)
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
