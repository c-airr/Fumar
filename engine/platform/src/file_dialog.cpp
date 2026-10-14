#include "fumar/platform/file_dialog.hpp"

#include "fumar/core/log.hpp"
#include "fumar/platform/window.hpp"

#include <SDL3/SDL_dialog.h>

#include <mutex>
#include <string>
#include <vector>

namespace fumar {

/// What the dialog's callback and the editor share. Held by shared pointer:
/// the callback keeps its own reference, so a FileDialog destroyed while a
/// dialog is still open leaves the callback writing into something alive.
struct FileDialog::Shared {
    std::mutex mutex;
    bool pending = false;
    std::optional<std::filesystem::path> result;
};

namespace {

/// Carried through SDL as the callback's userdata, and deleted by it.
struct Request {
    std::shared_ptr<FileDialog::Shared> shared;
};

void onChosen(void* userdata, const char* const* files, int /*filter*/) {
    const std::unique_ptr<Request> request(static_cast<Request*>(userdata));
    std::lock_guard lock(request->shared->mutex);
    request->shared->pending = false;

    if (files == nullptr) {
        FUMAR_WARN("file dialog failed: {}", SDL_GetError());
        return;
    }
    if (files[0] == nullptr) {
        // Cancelled.
        return;
    }
    // SDL hands paths over as UTF-8; std::filesystem is told so rather than
    // left to guess with the local code page.
    const std::string utf8 = files[0];
    request->shared->result = std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

} // namespace

FileDialog::FileDialog() : m_shared(std::make_shared<Shared>()) {}

FileDialog::~FileDialog() = default;

void FileDialog::openFile(Window& window, std::span<const FileFilter> filters, const std::filesystem::path& start) {
    {
        std::lock_guard lock(m_shared->mutex);
        if (m_shared->pending) {
            return;
        }
        m_shared->pending = true;
        m_shared->result.reset();
    }

    // Copied into SDL's own struct; SDL copies them again before returning.
    std::vector<SDL_DialogFileFilter> sdlFilters;
    sdlFilters.reserve(filters.size());
    for (const FileFilter& filter : filters) {
        sdlFilters.push_back(SDL_DialogFileFilter{filter.name, filter.pattern});
    }

    std::error_code ec;
    const std::u8string startUtf8 = std::filesystem::exists(start, ec) ? start.u8string() : std::u8string{};
    const std::string startText(startUtf8.begin(), startUtf8.end());

    auto* request = new Request{m_shared};
    SDL_ShowOpenFileDialog(onChosen, request, window.handle(), sdlFilters.data(), static_cast<int>(sdlFilters.size()),
                           startText.empty() ? nullptr : startText.c_str(), false);
}

std::optional<std::filesystem::path> FileDialog::takeResult() {
    std::lock_guard lock(m_shared->mutex);
    std::optional<std::filesystem::path> result = std::move(m_shared->result);
    m_shared->result.reset();
    return result;
}

bool FileDialog::pending() const {
    std::lock_guard lock(m_shared->mutex);
    return m_shared->pending;
}

} // namespace fumar
