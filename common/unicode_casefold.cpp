#include "unicode_casefold.h"

#include <utf8proc.h>

#include <limits>
#include <memory>

namespace wave {

std::string unicode_casefold(std::string_view text)
{
    if (text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<utf8proc_ssize_t>::max())) {
        return {};
    }

    utf8proc_uint8_t* mapped = nullptr;
    const auto length = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(text.data()),
                                     static_cast<utf8proc_ssize_t>(text.size()), &mapped, UTF8PROC_CASEFOLD);
    const std::unique_ptr<utf8proc_uint8_t, decltype(&utf8proc_free)> folded(mapped, utf8proc_free);
    if (length < 0) {
        return {};
    }
    return {reinterpret_cast<const char*>(folded.get()), static_cast<std::size_t>(length)};
}

} // namespace wave
