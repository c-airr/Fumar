#pragma once

#include <filesystem>

namespace fumar {

/// Directory the running executable lives in.
///
/// Assets are located relative to this rather than to the working directory,
/// because the working directory is whatever the shell or the debugger happened
/// to be in when the process started - launching from an IDE and from a
/// terminal would otherwise resolve shader paths differently.
const std::filesystem::path& executableDirectory();

} // namespace fumar
