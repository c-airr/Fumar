#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fumar {

/// A code editor installed on this machine.
struct CodeEditor {
    /// What to put on a button: "Visual Studio 2022", "VS Code".
    std::string name;

    /// The executable to launch, already resolved to a full path.
    std::filesystem::path launcher;

    /// Whether it wants the FOLDER or the FILE.
    ///
    /// Visual Studio opens a CMake project by being handed the directory
    /// containing CMakeLists.txt, and handed a lone .cpp it opens a text
    /// window with no project behind it and no IntelliSense. VS Code is happy
    /// either way but is far more useful given the folder, for the same reason.
    bool wantsProjectRoot = true;

    /// Arguments that go before the path.
    ///
    /// VS Code gets --new-window here, and it matters more than it looks:
    /// without it, a VS Code that is already open REPLACES the folder in the
    /// window you were working in. Nothing new appears, your own project is
    /// gone, and the button looks like it did nothing at all.
    std::vector<std::string> arguments;
};

/// Editors found on this machine, most capable first. Empty is normal.
///
/// Probing costs a filesystem walk and, for Visual Studio, running vswhere -
/// so call it once and keep the answer. Nobody installs an IDE while the editor
/// is open.
std::vector<CodeEditor> findCodeEditors();

/// Launches `editor` on `path` and returns immediately.
///
/// Detached on purpose: an IDE outlives the thing that started it, and waiting
/// for one to exit would freeze the editor for as long as somebody is working.
bool openInEditor(const CodeEditor& editor, const std::filesystem::path& path);

/// Opens a folder in the system file browser - Explorer, Finder, whatever the
/// desktop provides.
bool revealInFileBrowser(const std::filesystem::path& path);

} // namespace fumar
