#pragma once

#include <filesystem>
#include <string>

namespace fumar::native {

/// A shared library, opened at runtime.
///
/// Thirty lines rather than a dependency, because the whole of what is needed
/// is three calls that every operating system has under different names:
/// open, look up a symbol, close.
class DynamicLibrary {
public:
    DynamicLibrary() = default;
    ~DynamicLibrary();

    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

    /// Returns false and fills `error` on failure.
    bool open(const std::filesystem::path& path, std::string& error);

    void close();

    /// The address of an exported symbol, or null if it is not there.
    void* symbol(const char* name) const;

    bool valid() const { return m_handle != nullptr; }

private:
    // void* rather than HMODULE, so windows.h stays out of this header.
    void* m_handle = nullptr;
};

/// The file name the platform gives a shared library called `stem`.
///
/// Windows: `stem.dll`. Linux: `libstem.so`. The engine looks for the library
/// by name, so it has to know which one it is on.
std::string sharedLibraryFileName(const std::string& stem);

} // namespace fumar::native
