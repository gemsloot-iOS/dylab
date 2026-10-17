import Darwin
import SwiftUI

struct ContentView: View {
    @State private var consoleText = "DyLab WASI console ready.\nTap Run Hello.wasm Test to execute the bundled tool.\n"
    @State private var isRunning = false

    var body: some View {
        VStack(spacing: 12) {
            Text("DyLab")
                .font(.title2.weight(.semibold))
                .frame(maxWidth: .infinity, alignment: .leading)

            ScrollView {
                Text(consoleText)
                    .font(.system(.body, design: .monospaced))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
                    .padding(8)
            }
            .background(Color(white: 0.08))
            .foregroundStyle(Color(white: 0.92))
            .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))

            Button {
                runHelloWasmTest()
            } label: {
                Text(isRunning ? "Running…" : "Run Hello.wasm Test")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(isRunning)
        }
        .padding()
    }

    private func appendConsole(_ line: String) {
        consoleText.append(line)
        if !line.hasSuffix("\n") {
            consoleText.append("\n")
        }
    }

    private func runHelloWasmTest() {
        guard !isRunning else { return }
        isRunning = true
        appendConsole("\n--- Run Hello.wasm Test ---")

        DispatchQueue.global(qos: .userInitiated).async {
            var messages: [String] = []

            guard let wasmURL = Bundle.main.url(forResource: "hello", withExtension: "wasm") else {
                messages.append("error: hello.wasm not found in Bundle.main")
                finish(messages: messages)
                return
            }

            let docsURL = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
            let sandboxRoot = docsURL.path
            let wasmPath = wasmURL.path

            messages.append("wasm: \(wasmPath)")
            messages.append("sandbox: \(sandboxRoot)")

            FileManager.default.changeCurrentDirectoryPath(sandboxRoot)

            let args = ["hello.wasm", "phase1", "ios"]
            var cArgv = args.map { strdup($0) }
            defer {
                for ptr in cArgv {
                    free(ptr)
                }
            }

            var outputBuf = [CChar](repeating: 0, count: 64 * 1024)
            let rc = outputBuf.withUnsafeMutableBufferPointer { buf -> Int32 in
                cArgv.withUnsafeMutableBufferPointer { argvBuf in
                    run_wasi_tool(
                        wasmPath,
                        sandboxRoot,
                        Int32(argvBuf.count),
                        argvBuf.baseAddress,
                        buf.baseAddress,
                        buf.count
                    )
                }
            }

            let captured = String(cString: outputBuf)
            if !captured.isEmpty {
                messages.append("stdout/stderr:")
                messages.append(captured)
            } else {
                messages.append("(no captured stdout/stderr)")
            }
            messages.append("run_wasi_tool returned \(rc)")

            let testFile = docsURL.appendingPathComponent("workspace_test.txt")
            if let contents = try? String(contentsOf: testFile, encoding: .utf8) {
                messages.append("workspace_test.txt:")
                messages.append(contents)
            } else {
                messages.append("error: could not read workspace_test.txt from Documents")
            }

            finish(messages: messages)
        }
    }

    private func finish(messages: [String]) {
        DispatchQueue.main.async {
            for message in messages {
                appendConsole(message)
            }
            isRunning = false
        }
    }
}

#if DEBUG
struct ContentView_Previews: PreviewProvider {
    static var previews: some View {
        ContentView()
    }
}
#endif
