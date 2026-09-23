#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace {

constexpr size_t kMaxMulti = 16;

struct Options {
    std::string port;
    std::string send;
    bool hasSend = false;
    std::vector<std::string> sendMulti;
    bool hasSendMulti = false;
    DWORD waitMs = 2000;
    bool hasListen = false;
    DWORD listenMs = 0;
    DWORD baud = 115200;
    bool autoRelease = false;
};

int FailWin32(const char* step) {
    const DWORD err = GetLastError();
    std::printf("[错误] %s 失败: GetLastError=%lu\n", step,
                static_cast<unsigned long>(err));
    return 1;
}

void PrintRaw(const BYTE* data, DWORD len) {
    std::fputs("[收到]", stdout);
    for (DWORD i = 0; i < len; ++i) {
        const unsigned char c = data[i];
        if (c >= 0x20 && c <= 0x7E) {
            std::fputc(static_cast<int>(c), stdout);
        } else {
            std::printf("\\x%02X", c);
        }
    }
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

bool SplitMulti(const std::string& text, std::vector<std::string>* out) {
    size_t start = 0;
    while (start <= text.size()) {
        size_t pos = text.find(';', start);
        if (pos == std::string::npos) {
            pos = text.size();
        }
        const std::string part = text.substr(start, pos - start);
        if (part.empty()) {
            std::printf("[错误] --send-multi 含空指令\n");
            return false;
        }
        if (part.find('\n') != std::string::npos ||
            part.find('\r') != std::string::npos) {
            std::printf("[错误] --send-multi 指令里不允许包含换行符\n");
            return false;
        }
        out->push_back(part);
        if (out->size() > kMaxMulti) {
            std::printf("[错误] --send-multi 指令数超过 %u\n",
                        static_cast<unsigned>(kMaxMulti));
            return false;
        }
        if (pos >= text.size()) {
            break;
        }
        start = pos + 1;
    }
    return true;
}

bool ParseArgs(int argc, char* argv[], Options* opt) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                std::printf("[错误] 缺失参数: --port\n");
                return false;
            }
            opt->port = argv[++i];
        } else if (strcmp(argv[i], "--send") == 0) {
            if (i + 1 >= argc) {
                std::printf("[错误] 缺失参数: --send\n");
                return false;
            }
            opt->send = argv[++i];
            opt->hasSend = true;
        } else if (strcmp(argv[i], "--send-multi") == 0) {
            if (i + 1 >= argc) {
                std::printf("[错误] 缺失参数: --send-multi\n");
                return false;
            }
            opt->hasSendMulti = true;
            if (!SplitMulti(argv[++i], &opt->sendMulti)) {
                return false;
            }
        } else if (strcmp(argv[i], "--wait-ms") == 0) {
            if (i + 1 >= argc) {
                std::printf("[错误] 缺失参数: --wait-ms\n");
                return false;
            }
            opt->waitMs = static_cast<DWORD>(strtoul(argv[++i], nullptr, 10));
        } else if (strcmp(argv[i], "--listen-ms") == 0) {
            if (i + 1 >= argc) {
                std::printf("[错误] 缺失参数: --listen-ms\n");
                return false;
            }
            opt->listenMs = static_cast<DWORD>(strtoul(argv[++i], nullptr, 10));
            opt->hasListen = true;
        } else if (strcmp(argv[i], "--baud") == 0) {
            if (i + 1 >= argc) {
                std::printf("[错误] 缺失参数: --baud\n");
                return false;
            }
            opt->baud = static_cast<DWORD>(strtoul(argv[++i], nullptr, 10));
        } else if (strcmp(argv[i], "--auto-release") == 0) {
            opt->autoRelease = true;
        } else {
            std::printf("[错误] 未知参数: %s\n", argv[i]);
            return false;
        }
    }

    if (opt->port.empty()) {
        std::printf("[错误] 缺失参数: --port\n");
        return false;
    }

    int sendModes = 0;
    if (opt->hasSend) {
        ++sendModes;
    }
    if (opt->hasSendMulti) {
        ++sendModes;
    }
    if (opt->hasListen) {
        ++sendModes;
    }
    if (sendModes != 1) {
        std::printf("[错误] --send / --send-multi / --listen-ms 必须且只能指定一个\n");
        return false;
    }

    if (opt->hasSend) {
        for (size_t i = 0; i < opt->send.size(); ++i) {
            const char c = opt->send[i];
            if (c == '\n' || c == '\r') {
                std::printf("[错误] --send 里不允许包含换行符\n");
                return false;
            }
        }
    }
    return true;
}

bool SendLine(HANDLE hPort, const std::string& cmd) {
    std::printf("[发送] %s\n", cmd.c_str());
    std::fflush(stdout);
    std::string payload = cmd;
    payload.push_back('\n');
    DWORD written = 0;
    if (!WriteFile(hPort, payload.data(),
                   static_cast<DWORD>(payload.size()), &written, nullptr)) {
        return false;
    }
    return true;
}

bool DrainRead(HANDLE hPort, DWORD durationMs, DWORD* totalRx,
               ULONGLONG overallEnd) {
    const ULONGLONG end = GetTickCount64() + durationMs;
    std::vector<BYTE> buf(4096);
    while (GetTickCount64() < end && GetTickCount64() < overallEnd) {
        DWORD read = 0;
        if (!ReadFile(hPort, buf.data(), static_cast<DWORD>(buf.size()),
                      &read, nullptr)) {
            return false;
        }
        if (read > 0) {
            PrintRaw(buf.data(), read);
            *totalRx += read;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    Options opt;
    if (!ParseArgs(argc, argv, &opt)) {
        return 1;
    }

    const ULONGLONG sessionStart = GetTickCount64();
    const ULONGLONG overallEnd = sessionStart + opt.waitMs + 1000;

    std::wstring portPath = L"\\\\.\\";
    for (size_t i = 0; i < opt.port.size(); ++i) {
        portPath.push_back(static_cast<wchar_t>(
            static_cast<unsigned char>(opt.port[i])));
    }

    HANDLE hPort = CreateFileW(portPath.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               0,
                               nullptr,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (hPort == INVALID_HANDLE_VALUE) {
        std::printf("[错误] 打开串口失败: %s\n", opt.port.c_str());
        return 1;
    }

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(hPort, &dcb)) {
        CloseHandle(hPort);
        return FailWin32("GetCommState");
    }
    dcb.BaudRate = opt.baud;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(hPort, &dcb)) {
        CloseHandle(hPort);
        return FailWin32("SetCommState");
    }

    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 500;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    if (!SetCommTimeouts(hPort, &timeouts)) {
        CloseHandle(hPort);
        return FailWin32("SetCommTimeouts");
    }

    std::printf("已打开: %s 波特率=%lu 8N1\n", opt.port.c_str(),
                static_cast<unsigned long>(opt.baud));

    DWORD totalRx = 0;
    const bool isSendMode = opt.hasSend || opt.hasSendMulti;

    if (isSendMode) {
        if (!PurgeComm(hPort, PURGE_RXCLEAR)) {
            CloseHandle(hPort);
            return FailWin32("PurgeComm");
        }
    }

    if (isSendMode && opt.autoRelease) {
        if (!SendLine(hPort, "mk.release")) {
            CloseHandle(hPort);
            return FailWin32("WriteFile");
        }
        if (!DrainRead(hPort, 200, &totalRx, overallEnd)) {
            CloseHandle(hPort);
            return FailWin32("ReadFile");
        }
    }

    if (opt.hasSend) {
        if (!SendLine(hPort, opt.send)) {
            CloseHandle(hPort);
            return FailWin32("WriteFile");
        }
    } else if (opt.hasSendMulti) {
        for (size_t i = 0; i < opt.sendMulti.size(); ++i) {
            if (!SendLine(hPort, opt.sendMulti[i])) {
                CloseHandle(hPort);
                return FailWin32("WriteFile");
            }
            if (i + 1 < opt.sendMulti.size()) {
                if (!DrainRead(hPort, 50, &totalRx, overallEnd)) {
                    CloseHandle(hPort);
                    return FailWin32("ReadFile");
                }
            }
        }
    } else {
        std::printf("仅监听 %lu ms\n", static_cast<unsigned long>(opt.listenMs));
    }

    if (opt.hasListen) {
        if (!DrainRead(hPort, opt.listenMs, &totalRx, overallEnd)) {
            CloseHandle(hPort);
            return FailWin32("ReadFile");
        }
    } else if (isSendMode && opt.autoRelease) {
        if (!DrainRead(hPort, 500, &totalRx, overallEnd)) {
            CloseHandle(hPort);
            return FailWin32("ReadFile");
        }
        if (!SendLine(hPort, "mk.release")) {
            CloseHandle(hPort);
            return FailWin32("WriteFile");
        }
        if (!DrainRead(hPort, 500, &totalRx, overallEnd)) {
            CloseHandle(hPort);
            return FailWin32("ReadFile");
        }
    } else {
        if (!DrainRead(hPort, opt.waitMs, &totalRx, overallEnd)) {
            CloseHandle(hPort);
            return FailWin32("ReadFile");
        }
    }

    CloseHandle(hPort);
    std::printf("总收到字节: %lu\n", static_cast<unsigned long>(totalRx));
    return 0;
}
