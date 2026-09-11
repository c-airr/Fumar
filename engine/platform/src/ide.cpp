#include "fumar/platform/ide.hpp"

#include "fumar/core/log.hpp"

#define SDL_MAIN_HANDLED
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_process.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace fumar {
namespace {

/// Not named `exists`: argument-dependent lookup would then find
/// std::filesystem::exists as well, because the argument lives in that
/// namespace, and every call would be ambiguous.
bool pathExists(const std::filesystem::path& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path, ec);
}

/// The value of an environment variable as a path, or empty.
std::filesystem::path environmentPath(const char* name) {
#if defined(_WIN32)
    // _dupenv_s rather than getenv: the latter is deprecated on MSVC and the
    // warning is one we have turned into an error elsewhere.
    char* value = nullptr;
    usize length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::filesystem::path result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::filesystem::path(value) : std::filesystem::path{};
#endif
}

/// Walks PATH looking for an executable.
///
/// Needed because a launcher installed per-user - which is how VS Code installs
/// by default - is not in any fixed location, only on PATH.
std::filesystem::path findOnPath(const std::string& name) {
    const std::filesystem::path pathVariable = environmentPath("PATH");
    if (pathVariable.empty()) {
        return {};
    }

    const std::string text = pathVariable.string();
#if defined(_WIN32)
    constexpr char kSeparator = ';';
#else
    constexpr char kSeparator = ':';
#endif

    usize start = 0;
    while (start <= text.size()) {
        const usize end = text.find(kSeparator, start);
        const std::string entry = text.substr(start, end == std::string::npos ? end : end - start);

        if (!entry.empty()) {
            const std::filesystem::path candidate = std::filesystem::path(entry) / name;
            if (pathExists(candidate)) {
                return candidate;
            }
        }

        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return {};
}

#if defined(_WIN32)
/// Asks vswhere where Visual Studio is.
///
/// There is no fixed path and no registry key worth trusting - an installation
/// can be anywhere, and several versions can coexist. vswhere is the tool
/// Microsoft ships for exactly this question, and it IS at a fixed path,
/// because it is installed by the VS installer rather than by VS.
std::filesystem::path findVisualStudio() {
    const std::filesystem::path installer =
        environmentPath("ProgramFiles(x86)") / "Microsoft Visual Studio" / "Installer" /
        "vswhere.exe";
    if (!pathExists(installer)) {
        return {};
    }

    // Through a temporary file rather than a pipe: this runs once, and a pipe
    // would mean either a second thread or a blocking read that can deadlock if
    // the child writes more than the buffer holds.
    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "fumar_vswhere.txt";

    const std::string command = "\"\"" + installer.string() +
                                "\" -latest -prerelease -property productPath > \"" +
                                output.string() + "\"\"";

    if (std::system(command.c_str()) != 0) {
        return {};
    }

    std::error_code ec;
    std::filesystem::path result;
    {
        std::FILE* file = nullptr;
        if (fopen_s(&file, output.string().c_str(), "r") == 0 && file != nullptr) {
            std::array<char, 1024> buffer{};
            if (std::fgets(buffer.data(), static_cast<int>(buffer.size()), file) != nullptr) {
                std::string line(buffer.data());
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                    line.pop_back();
                }
                result = line;
            }
            std::fclose(file);
        }
    }
    std::filesystem::remove(output, ec);

    return pathExists(result) ? result : std::filesystem::path{};
}
#endif

} // namespace

std::vector<CodeEditor> findCodeEditors() {
    std::vector<CodeEditor> found;

#if defined(_WIN32)
    if (const std::filesystem::path devenv = findVisualStudio(); !devenv.empty()) {
        // The version is in the path rather than reported by vswhere's
        // productPath, so the name stays generic instead of claiming a year it
        // might have guessed wrong.
        found.push_back(CodeEditor{"Visual Studio", devenv, true});
    }

    // The per-user install, then the machine-wide one, then PATH. In that order
    // because the first is what the installer picks by default.
    const std::array<std::filesystem::path, 2> vscodePaths{
        environmentPath("LOCALAPPDATA") / "Programs" / "Microsoft VS Code" / "Code.exe",
        environmentPath("ProgramFiles") / "Microsoft VS Code" / "Code.exe",
    };
    for (const std::filesystem::path& path : vscodePaths) {
        if (pathExists(path)) {
            found.push_back(CodeEditor{"VS Code", path, true});
            break;
        }
    }
    if (found.empty() || found.back().name != "VS Code") {
        if (const std::filesystem::path path = findOnPath("code.cmd"); !path.empty()) {
            found.push_back(CodeEditor{"VS Code", path, true});
        }
    }
#else
    if (const std::filesystem::path path = findOnPath("code"); !path.empty()) {
        found.push_back(CodeEditor{"VS Code", path, true});
    }
    for (const char* name : {"clion", "subl", "gedit"}) {
        if (const std::filesystem::path path = findOnPath(name); !path.empty()) {
            found.push_back(CodeEditor{name, path, true});
        }
    }
#endif

    return found;
}

bool openInEditor(const CodeEditor& editor, const std::filesystem::path& path) {
    if (editor.launcher.empty()) {
        return false;
    }

    const std::string launcher = editor.launcher.string();
    const std::string target = path.string();

    // A null-terminated argv, which is what every exec-shaped API in existence
    // wants. SDL's process API rather than std::system: system() goes through a
    // shell, which on Windows means a console window flashing up in front of
    // whatever the user was doing.
    const std::array<const char*, 3> args{launcher.c_str(), target.c_str(), nullptr};

    SDL_Process* process = SDL_CreateProcess(args.data(), false);
    if (process == nullptr) {
        FUMAR_WARN("could not launch '{}': {}", launcher, SDL_GetError());
        return false;
    }

    // Destroys the HANDLE, not the process - an editor is supposed to outlive
    // whatever opened it.
    SDL_DestroyProcess(process);

    FUMAR_INFO("opened '{}' in {}", target, editor.name);
    return true;
}

bool revealInFileBrowser(const std::filesystem::path& path) {
    // A file: URL through the desktop's own handler, which is Explorer on
    // Windows and whatever xdg-open resolves to elsewhere. Spelling out
    // explorer.exe and nautilus and dolphin by hand would be a list that is
    // wrong on somebody's machine from the day it is written.
    std::string url = "file:///" + path.generic_string();

    if (!SDL_OpenURL(url.c_str())) {
        FUMAR_WARN("could not open '{}': {}", path.string(), SDL_GetError());
        return false;
    }
    return true;
}

} // namespace fumar
