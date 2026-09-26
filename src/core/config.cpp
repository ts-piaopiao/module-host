#include "config.h"

#include <fstream>
#include <sstream>

namespace {

std::string Trim(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' ||
                                   text[begin] == '\r' || text[begin] == '\n')) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool IsBlank(const std::string& text) {
    for (char ch : text) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            return false;
        }
    }
    return true;
}

bool IsValidKey(const std::string& key) {
    if (key.empty()) {
        return false;
    }
    for (char ch : key) {
        const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        const bool digit = ch >= '0' && ch <= '9';
        if (!alpha && !digit && ch != '_') {
            return false;
        }
    }
    return true;
}

bool ParseFrames(const std::string& value, int* out, std::string* err) {
    if (value.empty()) {
        *err = "frames 不是数字";
        return false;
    }
    for (char ch : value) {
        if (ch < '0' || ch > '9') {
            *err = "frames 不是数字";
            return false;
        }
    }
    long long parsed = 0;
    for (char ch : value) {
        parsed = parsed * 10 + (ch - '0');
        if (parsed > 1000000000LL) {
            break;
        }
    }
    if (parsed < 0 || parsed > 100000) {
        *err = "frames 超出范围: " + value;
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

bool ParseIntStrict(const std::string& value, int* out, std::string* err, const char* name) {
    if (value.empty()) {
        *err = std::string(name) + " 不是数字";
        return false;
    }
    for (char ch : value) {
        if (ch < '0' || ch > '9') {
            *err = std::string(name) + " 不是数字";
            return false;
        }
    }
    long long parsed = 0;
    for (char ch : value) {
        parsed = parsed * 10 + (ch - '0');
        if (parsed > 1000000000LL) {
            break;
        }
    }
    *out = static_cast<int>(parsed);
    return true;
}

}  // namespace

bool LoadConfigFile(const std::string& path, CoreConfig* out, std::string* err) {
    err->clear();
    std::ifstream file(path);
    if (!file.is_open()) {
        *err = "配置文件不存在: " + path;
        return false;
    }

    CoreConfig config;
    std::string line;
    while (std::getline(file, line)) {
        const std::string trimmed = Trim(line);
        if (IsBlank(line) || trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            *err = "配置行格式错误";
            return false;
        }

        const std::string key = Trim(trimmed.substr(0, eq));
        const std::string value = Trim(trimmed.substr(eq + 1));

        if (!IsValidKey(key)) {
            *err = "配置行格式错误";
            return false;
        }

        if (key == "plugins_dir") {
            config.plugins_dir = value;
            config.has_plugins_dir = true;
        } else if (key == "frames") {
            int frames = 5;
            if (!ParseFrames(value, &frames, err)) {
                return false;
            }
            config.frames = frames;
            config.has_frames = true;
        } else if (key == "log_path") {
            config.log_path = value;
            config.has_log_path = true;
        } else if (key == "remote_port") {
            int remote_port = 0;
            if (!ParseIntStrict(value, &remote_port, err, "remote_port")) {
                return false;
            }
            if (remote_port < 0 || remote_port > 65535) {
                *err = "remote_port 超出范围: " + value;
                return false;
            }
            config.remote_port = remote_port;
            config.has_remote_port = true;
        } else if (key == "remote_jpeg_quality") {
            int remote_jpeg_quality = 80;
            if (!ParseIntStrict(value, &remote_jpeg_quality, err, "remote_jpeg_quality")) {
                return false;
            }
            if (remote_jpeg_quality < 1 || remote_jpeg_quality > 100) {
                *err = "remote_jpeg_quality 超出范围: " + value;
                return false;
            }
            config.remote_jpeg_quality = remote_jpeg_quality;
            config.has_remote_jpeg_quality = true;
        } else if (key == "decider_dry_run") {
            int decider_dry_run = 0;
            if (!ParseIntStrict(value, &decider_dry_run, err, "decider_dry_run")) {
                return false;
            }
            if (decider_dry_run != 0 && decider_dry_run != 1) {
                *err = "decider_dry_run 必须为 0 或 1: " + value;
                return false;
            }
            config.decider_dry_run = decider_dry_run;
            config.has_decider_dry_run = true;
        } else if (key == "script_enabled") {
            int script_enabled = 1;
            if (!ParseIntStrict(value, &script_enabled, err, "script_enabled")) {
                return false;
            }
            config.has_script_enabled = true;
            config.script_enabled = (script_enabled != 0);
        } else if (key.rfind("capture_", 0) == 0 ||
                   key.rfind("policy_", 0) == 0 ||
                   key.rfind("input_", 0) == 0 ||
                   key.rfind("combat_", 0) == 0 ||
                   key.rfind("output_", 0) == 0) {
            continue;
        } else {
            *err = "未知配置项: " + key;
            return false;
        }
    }

    *out = config;
    return true;
}
