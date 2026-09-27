#include "script_config.h"

#include <cstdlib>

namespace {

static void Trim(const std::string& s, std::string* out) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r' || s[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    *out = s.substr(begin, end - begin);
}

static bool ParseInt(const std::string& text, int* out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return false;
    *out = static_cast<int>(value);
    return true;
}

}  // namespace

ScriptConfig ScriptConfig::FromString(const std::string& config) {
    ScriptConfig cfg;

    if (config.empty()) return cfg;

    size_t pos = 0;
    const std::string content(config);
    while (pos <= content.size()) {
        size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) nl = content.size();
        std::string line = content.substr(pos, nl - pos);
        pos = nl + 1;

        std::string trimmed;
        Trim(line, &trimmed);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        const size_t comment = trimmed.find('#');
        if (comment != std::string::npos) {
            trimmed = trimmed.substr(0, comment);
            Trim(trimmed, &trimmed);
            if (trimmed.empty()) continue;
        }

        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trimmed.substr(0, eq);
        std::string value = trimmed.substr(eq + 1);
        Trim(key, &key);
        Trim(value, &value);

        int num = 0;
        if (!ParseInt(value, &num)) continue;

        if (key == "combat_attack_react_min_ms") {
            cfg.attack_react_min_ms = num;
        } else if (key == "combat_attack_react_max_ms") {
            cfg.attack_react_max_ms = num;
        } else if (key == "combat_recovery_chase_min_ms") {
            cfg.recovery_chase_min_ms = num;
        } else if (key == "combat_recovery_chase_max_ms") {
            cfg.recovery_chase_max_ms = num;
        }
        // 未知键忽略
    }

    return cfg;
}
