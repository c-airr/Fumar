#include "fumar/core/file.hpp"

#include "fumar/core/log.hpp"

#include <fstream>
#include <ios>

namespace fumar {

std::optional<std::vector<u32>> readSpirvFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto byteSize = std::filesystem::file_size(path, ec);
    if (ec) {
        FUMAR_ERROR("cannot stat shader '{}': {}", path.string(), ec.message());
        return std::nullopt;
    }

    if (byteSize == 0 || byteSize % sizeof(u32) != 0) {
        FUMAR_ERROR("shader '{}' has an invalid size ({} bytes, expected a multiple of 4)", path.string(),
                    byteSize);
        return std::nullopt;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        FUMAR_ERROR("cannot open shader '{}'", path.string());
        return std::nullopt;
    }

    std::vector<u32> words(static_cast<usize>(byteSize) / sizeof(u32));
    file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(byteSize));
    if (!file) {
        FUMAR_ERROR("short read while loading shader '{}'", path.string());
        return std::nullopt;
    }

    return words;
}

} // namespace fumar
