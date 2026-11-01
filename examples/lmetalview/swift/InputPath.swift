// SPDX-License-Identifier: Apache-2.0
//
// lmetalview - resolve command-line paths to an existing file.
//
// A macOS .app launched from Finder or via `open --args file.usda` inherits the
// launchd working directory ("/"), not the shell's, so a relative path that
// works when running the executable directly would otherwise fail. We keep the
// direct-CWD behavior and fall back to the shell's PWD, the executable's
// ancestors (useful when the app is inside a build tree) and the home folder.

import Foundation

enum InputPath {
    static func resolve(_ path: String) -> String {
        guard !path.isEmpty else { return path }
        var candidatePath = path
        if candidatePath == "~" || candidatePath.hasPrefix("~/") {
            candidatePath = (NSHomeDirectory() as NSString)
                .appendingPathComponent(String(candidatePath.dropFirst(2)))
        }
        let manager = FileManager.default
        if candidatePath.hasPrefix("/") {
            return (candidatePath as NSString).standardizingPath
        }

        var directories: [String] = []
        func add(_ directory: String?) {
            guard let directory, !directory.isEmpty else { return }
            if !directories.contains(directory) { directories.append(directory) }
        }

        add(manager.currentDirectoryPath)
        add(ProcessInfo.processInfo.environment["PWD"])
        if let executable = Bundle.main.executableURL?.resolvingSymlinksInPath() {
            var directory: URL? = executable.deletingLastPathComponent()
            for _ in 0..<12 {
                guard let current = directory else { break }
                add(current.path)
                if current.path == "/" { break }
                directory = current.deletingLastPathComponent()
            }
        }
        if let bundle = Bundle.main.bundleURL as URL? {
            add(bundle.path)
            add(bundle.deletingLastPathComponent().path)
            add(bundle.deletingLastPathComponent().deletingLastPathComponent().path)
        }
        add(NSHomeDirectory())

        for directory in directories {
            let candidate = (directory as NSString).appendingPathComponent(candidatePath)
            var isDirectory: ObjCBool = false
            if manager.fileExists(atPath: candidate, isDirectory: &isDirectory),
               !isDirectory.boolValue {
                return (candidate as NSString).standardizingPath
            }
        }
        return path
    }
}
