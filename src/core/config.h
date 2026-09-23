#ifndef MODULE_HOST_CORE_CONFIG_H
#define MODULE_HOST_CORE_CONFIG_H

#include <string>

struct CoreConfig {
    std::string plugins_dir;
    int frames = 5;
    std::string log_path;
    int remote_port = 0;
    int remote_jpeg_quality = 80;
    bool has_plugins_dir = false;
    bool has_frames = false;
    bool has_log_path = false;
    bool has_remote_port = false;
    bool has_remote_jpeg_quality = false;
};

bool LoadConfigFile(const std::string& path, CoreConfig* out, std::string* err);

#endif
