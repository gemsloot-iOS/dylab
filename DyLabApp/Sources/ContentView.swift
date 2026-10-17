import SwiftUI

struct ContentView: View {
    var body: some View {
        TabView {
            WorkspaceInspectorView()
                .tabItem {
                    Label("Workspaces", systemImage: "folder")
                }

            WasiSandboxView()
                .tabItem {
                    Label("WASI Sandbox", systemImage: "terminal")
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
