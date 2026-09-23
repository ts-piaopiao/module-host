#ifndef MODULE_HOST_CORE_CONFIG_H
#define MODULE_HOST_CORE_CONFIG_H

#include <string>

struct CoreConfig {
    std::string plugins_dir;
    int frames = 5;
    std::string log_path;
    bool has_plugins_dir = false;
    bool has_frames = false;
    bool has_log_path = false;
};

bool LoadConfigFile(const std::string& path, CoreConfig* out, std::string* err);

#endif
