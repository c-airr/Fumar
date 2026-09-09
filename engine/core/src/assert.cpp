#include "fumar/core/assert.hpp"

namespace fumar::detail {

void reportAssertion(std::string_view expression, std::string_view file, int line,
                     std::string_view message) {
    if (message.empty()) {
        ::fumar::log::detail::write(log::Level::Fatal, file, line,
                                    std::format("assertion failed: {}", expression));
    } else {
        ::fumar::log::detail::write(log::Level::Fatal, file, line,
                                    std::format("assertion failed: {} - {}", expression, message));
    }
}

} // namespace fumar::detail
