#include "dynamic_library.hpp"

#include <utility>

#if defined(_WIN32)
// NOMINMAX, or windows.h defines min and max as macros and every std::min in
// the translation unit stops compiling. WIN32_LEAN_AND_MEAN drops the half of
// the header nobody here wants.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fumar::native {
namespace {

#if defined(_WIN32)
std::string lastWindowsError() {
    const DWORD code = GetLastError();
    if (code == 0) {
        return "unknown error";
    }

    char* buffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);

    std::string message = length != 0 && buffer != nullptr ? std::string(buffer, length)
                                                           : std::string("error ") +
                                                                 std::to_string(code);
    if (buffer != nullptr) {
        LocalFree(buffer);
    }

    // FormatMessage ends its strings with a newline, which reads badly inside a
    // log line that has its own.
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return message;
}
#endif

} // namespace

DynamicLibrary::~DynamicLibrary() {
    close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr)) {}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        close();
        m_handle = std::exchange(other.m_handle, nullptr);
    }
    return *this;
}

bool DynamicLibrary::open(const std::filesystem::path& path, std::string& error) {
    close();

#if defined(_WIN32)
    m_handle = LoadLibraryW(path.wstring().c_str());
    if (m_handle == nullptr) {
        error = lastWindowsError();
        return false;
    }
#else
    // RTLD_NOW rather than lazy: an unresolved symbol should be reported when
    // the library is loaded, not the first time somebody happens to call the
    // function that needs it, halfway through a frame.
    m_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (m_handle == nullptr) {
        const char* message = dlerror();
        error = message != nullptr ? message : "dlopen failed";
        return false;
    }
#endif

    return true;
}

void DynamicLibrary::close() {
    if (m_handle == nullptr) {
        return;
    }

#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(m_handle));
#else
    dlclose(m_handle);
#endif

    m_handle = nullptr;
}

void* DynamicLibrary::symbol(const char* name) const {
    if (m_handle == nullptr) {
        return nullptr;
    }

#if defined(_WIN32)
    // The cast is through void(*)() rather than straight to void*: converting a
    // function pointer to an object pointer is not something the standard
    // blesses, and this is the spelling that says "I know" in one place.
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_handle), name));
#else
    return dlsym(m_handle, name);
#endif
}

std::string sharedLibraryFileName(const std::string& stem) {
#if defined(_WIN32)
    return stem + ".dll";
#elif defined(__APPLE__)
    return "lib" + stem + ".dylib";
#else
    return "lib" + stem + ".so";
#endif
}

} // namespace fumar::native
