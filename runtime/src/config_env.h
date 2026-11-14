#pragma once
#include <cctype>
#include <istream>
#include <string>

namespace nx::plat {
inline std::string config_trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
// Apply in file order: matches the launcher's map semantics (last duplicate wins).
template <class Apply>
void load_config_environment(std::istream& input, Apply apply) {
    std::string line;
    bool first = true;
    while (std::getline(input, line)) {
        if (first && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
        first = false;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = config_trim(line.substr(0, eq));
        if (key.size() <= 3 || key.rfind("NX_", 0) != 0 ||
            key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
            continue;
        apply(key, config_trim(line.substr(eq + 1)));
    }
}
}  // namespace nx::plat
