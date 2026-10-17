import SwiftUI

struct ContentView: View {
    var body: some View {
        TabView {
            WorkspaceInspectorView()
                .tabItem {
                    Label("Workspaces", systemImage: "folder.fill")
                }

            WasiSandboxView()
                .tabItem {
                    Label("WASI Sandbox", systemImage: "terminal.fill")
                }
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
