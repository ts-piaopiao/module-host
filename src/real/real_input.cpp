#include "core_contract.h"

#include <windows.h>

#include <cstdlib>
#include <string>

namespace {

HANDLE g_port = INVALID_HANDLE_VALUE;

std::string Trim(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r' || s[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(begin, end - begin);
}

bool ParseInt(const std::string& text, int* out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

void ParseConfig(const char* text, std::string* port, DWORD* baud) {
    port->assign("COM6");
    *baud = 115200;
    if (text == nullptr) {
        return;
    }
    size_t pos = 0;
    const std::string content(text);
    while (pos <= content.size()) {
        size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) {
            nl = content.size();
        }
        std::string line = content.substr(pos, nl - pos);
        pos = nl + 1;
        line = Trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));
        int num = 0;
        if (key == "input_port") {
            if (!value.empty()) {
                port->assign(value);
            }
        } else if (key == "input_baud") {
            if (ParseInt(value, &num) && num > 0) {
                *baud = static_cast<DWORD>(num);
            }
        }
    }
}

bool SendCommand(HANDLE h, const std::string& cmd) {
    std::string payload = cmd;
    payload.push_back('\n');
    DWORD written = 0;
    if (!WriteFile(h, payload.data(), static_cast<DWORD>(payload.size()),
                   &written, nullptr)) {
        return false;
    }
    return true;
}

const char* KeyName(int32_t vk) {
    if (vk >= 0x41 && vk <= 0x5A) {
        static thread_local char buf[2];
        buf[0] = static_cast<char>(vk - 0x41 + 'a');
        buf[1] = '\0';
        return buf;
    }
    if (vk >= 0x30 && vk <= 0x39) {
        static thread_local char buf[2];
        buf[0] = static_cast<char>(vk - 0x30 + '0');
        buf[1] = '\0';
        return buf;
    }
    switch (vk) {
    case 0x0D: return "enter";
    case 0x1B: return "esc";
    case 0x20: return "space";
    case 0x09: return "tab";
    case 0x08: return "backspace";
    case 0x10: return "shift";
    case 0x11: return "ctrl";
    case 0x12: return "alt";
    case 0x26: return "up";
    case 0x28: return "down";
    case 0x25: return "left";
    case 0x27: return "right";
    default: return nullptr;
    }
}

const char* ButtonName(int32_t id) {
    switch (id) {
    case 1: return "LEFT";
    case 2: return "RIGHT";
    case 3: return "MIDDLE";
    default: return nullptr;
    }
}

}  // namespace

extern "C" {

const char* plugin_meta(void) {
    return "real_input|1.0.0|4|input";
}

core_error plugin_init(uint32_t host_abi, const char* config) {
    if (host_abi != CORE_ABI_VERSION) {
        return CORE_ERR_ABI_MISMATCH;
    }
    if (g_port != INVALID_HANDLE_VALUE) {
        return CORE_ERR_INIT;
    }

    std::string port;
    DWORD baud = 115200;
    ParseConfig(config, &port, &baud);

    std::wstring path = L"\\\\.\\";
    for (size_t i = 0; i < port.size(); ++i) {
        path.push_back(static_cast<wchar_t>(static_cast<unsigned char>(port[i])));
    }

    HANDLE h = CreateFileW(path.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           0,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return CORE_ERR_INIT;
    }

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        CloseHandle(h);
        return CORE_ERR_INIT;
    }
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        return CORE_ERR_INIT;
    }

    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 500;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(h, &timeouts);

    SendCommand(h, "mk.release");

    g_port = h;
    return CORE_OK;
}

core_error plugin_release(void) {
    if (g_port != INVALID_HANDLE_VALUE) {
        SendCommand(g_port, "mk.release");
        Sleep(200);
        CloseHandle(g_port);
        g_port = INVALID_HANDLE_VALUE;
    }
    return CORE_OK;
}

core_error plugin_capture(core_frame* out) {
    (void)out;
    return CORE_OK;
}

core_error plugin_decide(const core_intent* intent, core_decision* out) {
    (void)intent;
    (void)out;
    return CORE_OK;
}

// TODO(脚本层): Combat 需要在状态切换时显式输出 release 动作，
// 由脚本层维护"脚本按下的键"的生命周期。
// 本插件仅做纯翻译（action → mk 命令），不做状态推断。
core_error plugin_execute(const core_decision* decision, core_execute_result* out) {
    if (g_port == INVALID_HANDLE_VALUE || decision == nullptr ||
        decision->out_count > CORE_DECISION_CAPACITY) {
        if (out != nullptr) {
            out->status = CORE_ERR_EXECUTE;
            out->detail = 0;
        }
        return CORE_ERR_EXECUTE;
    }

    for (uint32_t i = 0; i < decision->out_count; ++i) {
        const core_action& act = decision->actions[i];
        switch (act.kind) {
        case CORE_ACTION_NONE:
        case CORE_ACTION_CUSTOM:
            break;
        case CORE_ACTION_POINTER_MOVE: {
            std::string cmd = "mk.move " + std::to_string(act.a) + " " + std::to_string(act.b);
            if (!SendCommand(g_port, cmd)) {
                if (out != nullptr) {
                    out->status = CORE_ERR_EXECUTE;
                    out->detail = 0;
                }
                return CORE_ERR_EXECUTE;
            }
            break;
        }
        case CORE_ACTION_POINTER_BUTTON: {
            const char* btn = ButtonName(act.a);
            if (btn == nullptr) {
                break;
            }
            std::string cmd;
            if (act.b == 1) {
                cmd = std::string("mk.mdown ") + btn;
            } else if (act.b == 0) {
                cmd = std::string("mk.mup ") + btn;
            } else {
                break;
            }
            if (!SendCommand(g_port, cmd)) {
                if (out != nullptr) {
                    out->status = CORE_ERR_EXECUTE;
                    out->detail = 0;
                }
                return CORE_ERR_EXECUTE;
            }
            break;
        }
        case CORE_ACTION_KEY: {
            const char* name = KeyName(act.a);
            if (name == nullptr) {
                break;
            }
            std::string cmd;
            if (act.b == 1) {
                cmd = std::string("mk.press ") + name;
            } else if (act.b == 0) {
                cmd = std::string("mk.release ") + name;
            } else {
                break;
            }
            if (!SendCommand(g_port, cmd)) {
                if (out != nullptr) {
                    out->status = CORE_ERR_EXECUTE;
                    out->detail = 0;
                }
                return CORE_ERR_EXECUTE;
            }
            break;
        }
        case CORE_ACTION_WAIT:
            if (act.a > 0) {
                Sleep(static_cast<DWORD>(act.a));
            }
            break;
        default:
            break;
        }
    }

    if (out != nullptr) {
        out->status = CORE_OK;
        out->detail = 0;
    }
    return CORE_OK;
}

}  // extern "C"
