#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>

namespace fumar {

class Window;

/// One entry of the "files of type" list: a name and extensions separated by
/// ';' without dots, as SDL takes them - {"Images", "png;jpg;jpeg;tga"}.
struct FileFilter {
    const char* name;
    const char* pattern;
};

/// The system's open-file dialog, without stopping the frame loop.
///
/// The dialog runs on its own: the answer arrives later, possibly on another
/// thread, and is held here until the editor asks for it with takeResult on a
/// frame of its own - so nothing reaches into the editor's state from a thread
/// that does not own it. One dialog at a time.
class FileDialog {
public:
    FileDialog();
    ~FileDialog();

    FileDialog(const FileDialog&) = delete;
    FileDialog& operator=(const FileDialog&) = delete;

    /// Opens the dialog over `window`, in `start` if it exists. Does nothing
    /// while one is already open.
    void openFile(Window& window, std::span<const FileFilter> filters, const std::filesystem::path& start);

    /// The file chosen, once - then nothing until the next dialog. Nothing too
    /// when the dialog was cancelled.
    std::optional<std::filesystem::path> takeResult();

    /// A dialog is open and has not been answered yet.
    bool pending() const;

    struct Shared;

private:
    std::shared_ptr<Shared> m_shared;
};

} // namespace fumar
