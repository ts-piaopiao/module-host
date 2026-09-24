#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <imm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 540;

struct AppConfig {
    std::string host = "127.0.0.1";
    int port = 0;
};

SOCKET g_socket = INVALID_SOCKET;
std::mutex g_send_mutex;
HWND g_hwnd = nullptr;

std::mutex g_frame_mutex;
std::vector<uint8_t> g_frame_bgra;
UINT g_frame_w = 0;
UINT g_frame_h = 0;
std::atomic<int> g_fps{0};
std::atomic<int> g_fps_count{0};

int g_center_screen_x = 0;
int g_center_screen_y = 0;

HDC g_mem_dc = nullptr;
HBITMAP g_mem_bmp = nullptr;
HBITMAP g_mem_old_bmp = nullptr;
int g_mem_w = 0;
int g_mem_h = 0;

bool g_control_mode = false;

void WriteU32BE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

bool SendMsg(uint32_t type, uint32_t a, uint32_t b) {
    uint8_t buf[16];
    WriteU32BE(buf, 12);
    WriteU32BE(buf + 4, type);
    WriteU32BE(buf + 8, a);
    WriteU32BE(buf + 12, b);
    std::lock_guard<std::mutex> lk(g_send_mutex);
    if (g_socket == INVALID_SOCKET) {
        return false;
    }
    int sent = 0;
    while (sent < 16) {
        const int n = send(g_socket, reinterpret_cast<const char*>(buf + sent), 16 - sent, 0);
        if (n == SOCKET_ERROR || n == 0) {
            return false;
        }
        sent += n;
    }
    return true;
}

bool SendControlMode(uint32_t on) {
    // type=4 = ControlMode, payload: int32 on
    return SendMsg(4, on, 0);
}

bool SendKeyboard(uint32_t vk, uint32_t down) {
    return SendMsg(1, vk, down);
}

bool SendMouseButton(uint32_t button_id, uint32_t down) {
    return SendMsg(3, button_id, down);
}

bool SendMouseMove(int32_t dx, int32_t dy) {
    return SendMsg(2, static_cast<uint32_t>(dx), static_cast<uint32_t>(dy));
}

uint32_t ReadU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

class WicDecoder {
public:
    bool Init() {
        const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory_));
        return SUCCEEDED(hr);
    }

    ~WicDecoder() {
        if (factory_) {
            factory_->Release();
        }
    }

    bool Decode(const uint8_t* data, size_t size, std::vector<uint8_t>& out_bgra,
                UINT& out_w, UINT& out_h) {
        if (!factory_) {
            return false;
        }
        IStream* stream = SHCreateMemStream(data, static_cast<UINT>(size));
        if (!stream) {
            return false;
        }
        IWICBitmapDecoder* decoder = nullptr;
        HRESULT hr = factory_->CreateDecoderFromStream(
            stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
        stream->Release();
        if (FAILED(hr)) {
            return false;
        }
        IWICBitmapFrameDecode* frame = nullptr;
        hr = decoder->GetFrame(0, &frame);
        decoder->Release();
        if (FAILED(hr)) {
            return false;
        }
        UINT w = 0, h = 0;
        hr = frame->GetSize(&w, &h);
        if (FAILED(hr) || w == 0 || h == 0) {
            frame->Release();
            return false;
        }
        IWICFormatConverter* converter = nullptr;
        hr = factory_->CreateFormatConverter(&converter);
        if (FAILED(hr)) {
            frame->Release();
            return false;
        }
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
        frame->Release();
        if (FAILED(hr)) {
            converter->Release();
            return false;
        }
        const SIZE_T stride = static_cast<SIZE_T>(w) * 4;
        const SIZE_T buf_size = stride * h;
        out_bgra.resize(buf_size);
        hr = converter->CopyPixels(nullptr, static_cast<UINT>(stride),
                                   static_cast<UINT>(buf_size), out_bgra.data());
        converter->Release();
        if (FAILED(hr)) {
            return false;
        }
        out_w = w;
        out_h = h;
        return true;
    }

private:
    IWICImagingFactory* factory_ = nullptr;
};

WicDecoder g_wic;

void StoreFrame(std::vector<uint8_t>&& bgra, UINT w, UINT h) {
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_frame_mutex);
        g_frame_bgra = std::move(bgra);
        g_frame_w = w;
        g_frame_h = h;
        hwnd = g_hwnd;
    }
    if (hwnd != nullptr) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

struct FrameCopy {
    std::vector<uint8_t> bgra;
    UINT w = 0;
    UINT h = 0;
};

bool CopyLatestFrame(FrameCopy& out) {
    std::lock_guard<std::mutex> lk(g_frame_mutex);
    if (g_frame_bgra.empty()) {
        return false;
    }
    out.bgra = g_frame_bgra;
    out.w = g_frame_w;
    out.h = g_frame_h;
    return true;
}

void RecvThread() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_ok = SUCCEEDED(hr);

    std::vector<uint8_t> rx_buf;
    uint8_t buf[65536];
    int recv_errors = 0;
    while (true) {
        const int n = recv(g_socket, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n < 0) {
            if (++recv_errors >= 3) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (n == 0) {
            break;
        }
        recv_errors = 0;
        rx_buf.insert(rx_buf.end(), buf, buf + n);
        while (rx_buf.size() >= 8) {
            const uint32_t len = ReadU32BE(rx_buf.data());
            if (len < 4 || len > (16u << 20)) {
                if (com_ok) CoUninitialize();
                return;
            }
            if (rx_buf.size() < 4ull + len) {
                break;
            }
            const uint32_t type = ReadU32BE(rx_buf.data() + 4);
            const uint8_t* payload = rx_buf.data() + 8;
            const uint32_t payload_len = len - 4;
            if (type == 1 && payload_len > 0) {
                std::vector<uint8_t> jpeg(payload, payload + payload_len);
                std::vector<uint8_t> bgra;
                UINT w = 0, h = 0;
                if (g_wic.Decode(jpeg.data(), jpeg.size(), bgra, w, h)) {
                    StoreFrame(std::move(bgra), w, h);
                    g_fps_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // type=2 heartbeat: ignore
            rx_buf.erase(rx_buf.begin(), rx_buf.begin() + 4 + len);
        }
    }

    if (com_ok) CoUninitialize();
}

std::string g_title_host_port;

void UpdateTitleFps(HWND hwnd) {
    const int fps = g_fps.load(std::memory_order_relaxed);
    char title[256];
    if (g_control_mode) {
        std::snprintf(title, sizeof(title), "remote_client - %s - [控制中] - %d fps",
                      g_title_host_port.c_str(), fps);
    } else {
        std::snprintf(title, sizeof(title), "remote_client - %s - %d fps",
                      g_title_host_port.c_str(), fps);
    }
    wchar_t wtitle[256];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);
    SetWindowTextW(hwnd, wtitle);
}

void RecomputeCenter(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    POINT center = { (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
    ClientToScreen(hwnd, &center);
    g_center_screen_x = center.x;
    g_center_screen_y = center.y;
}

void SetControlMode(HWND hwnd, bool enable) {
    g_control_mode = enable;
    if (enable) {
        SetCapture(hwnd);
        RecomputeCenter(hwnd);
        SetCursorPos(g_center_screen_x, g_center_screen_y);
        ShowCursor(FALSE);
        // 新增：通知服务端进入控制模式
        SendControlMode(1);
    } else {
        ReleaseCapture();
        ShowCursor(TRUE);
        // 新增：通知服务端退出控制模式
        SendControlMode(0);
    }
    UpdateTitleFps(hwnd);
}

void EnsureBackbuffer(HDC hdc, int w, int h) {
    if (g_mem_dc == nullptr) {
        g_mem_dc = CreateCompatibleDC(hdc);
    }
    if (g_mem_bmp == nullptr || g_mem_w != w || g_mem_h != h) {
        if (g_mem_bmp != nullptr) {
            SelectObject(g_mem_dc, g_mem_old_bmp);
            DeleteObject(g_mem_bmp);
            g_mem_bmp = nullptr;
        }
        g_mem_bmp = CreateCompatibleBitmap(hdc, w, h);
        g_mem_old_bmp = static_cast<HBITMAP>(SelectObject(g_mem_dc, g_mem_bmp));
        g_mem_w = w;
        g_mem_h = h;
    }
}

void PaintFrame(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;

    if (cw <= 0 || ch <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }

    EnsureBackbuffer(hdc, cw, ch);

    RECT mem_rc = { 0, 0, cw, ch };
    HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    FillRect(g_mem_dc, &mem_rc, black);

    FrameCopy frame;
    if (CopyLatestFrame(frame) && frame.w > 0 && frame.h > 0) {
        const double scale = std::min(static_cast<double>(cw) / frame.w,
                                      static_cast<double>(ch) / frame.h);
        const int dw = static_cast<int>(frame.w * scale);
        const int dh = static_cast<int>(frame.h * scale);
        const int dx = (cw - dw) / 2;
        const int dy = (ch - dh) / 2;

        BITMAPINFO bmi;
        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(frame.w);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(frame.h);
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode(g_mem_dc, HALFTONE);
        SetBrushOrgEx(g_mem_dc, 0, 0, nullptr);

        StretchDIBits(g_mem_dc, dx, dy, dw, dh, 0, 0,
                      static_cast<int>(frame.w), static_cast<int>(frame.h),
                      frame.bgra.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
    }

    BitBlt(hdc, 0, 0, cw, ch, g_mem_dc, 0, 0, SRCCOPY);

    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintFrame(hwnd);
        return 0;
    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN: {
        if (wParam == VK_F12) {
            if (!(lParam & 0x40000000)) {
                SetControlMode(hwnd, !g_control_mode);
            }
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (g_control_mode) {
                if (!(lParam & 0x40000000)) {
                    SendKeyboard(VK_ESCAPE, 1);
                }
            } else {
                PostQuitMessage(0);
            }
            return 0;
        }
        if (!g_control_mode) {
            return 0;
        }
        if (!(lParam & 0x40000000)) {
            SendKeyboard(static_cast<uint32_t>(wParam), 1);
        }
        return 0;
    }
    case WM_KEYUP: {
        if (wParam == VK_F12) {
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (g_control_mode) {
                SendKeyboard(VK_ESCAPE, 0);
            }
            return 0;
        }
        if (!g_control_mode) {
            return 0;
        }
        SendKeyboard(static_cast<uint32_t>(wParam), 0);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_control_mode) {
            return 0;
        }
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        RECT rc;
        GetClientRect(hwnd, &rc);
        const int cx = (rc.right - rc.left) / 2;
        const int cy = (rc.bottom - rc.top) / 2;
        const int dx = x - cx;
        const int dy = y - cy;
        if (dx != 0 || dy != 0) {
            SendMouseMove(dx, dy);
            SetCursorPos(g_center_screen_x, g_center_screen_y);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (g_control_mode) SendMouseButton(1, 1);
        return 0;
    case WM_LBUTTONUP:
        if (g_control_mode) SendMouseButton(1, 0);
        return 0;
    case WM_RBUTTONDOWN:
        if (g_control_mode) SendMouseButton(2, 1);
        return 0;
    case WM_RBUTTONUP:
        if (g_control_mode) SendMouseButton(2, 0);
        return 0;
    case WM_MBUTTONDOWN:
        if (g_control_mode) SendMouseButton(3, 1);
        return 0;
    case WM_MBUTTONUP:
        if (g_control_mode) SendMouseButton(3, 0);
        return 0;
    case WM_KILLFOCUS:
        if (g_control_mode) {
            SetControlMode(hwnd, false);
        }
        return 0;
    case WM_WINDOWPOSCHANGED:
        if (g_control_mode) {
            RecomputeCenter(hwnd);
            SetCursorPos(g_center_screen_x, g_center_screen_y);
        }
        return 0;
    case WM_TIMER:
        UpdateTitleFps(hwnd);
        g_fps.store(g_fps_count.exchange(0, std::memory_order_relaxed),
                    std::memory_order_relaxed);
        return 0;
    case WM_DESTROY:
        if (g_control_mode) {
            ShowCursor(TRUE);
            ReleaseCapture();
            g_control_mode = false;
        }
        if (g_mem_dc != nullptr) {
            if (g_mem_bmp != nullptr) {
                SelectObject(g_mem_dc, g_mem_old_bmp);
                DeleteObject(g_mem_bmp);
                g_mem_bmp = nullptr;
            }
            DeleteDC(g_mem_dc);
            g_mem_dc = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

bool ParseArgs(int argc, char** argv, AppConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            cfg.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            cfg.port = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: remote_client --port <port> [--host <ip>]\n");
            return false;
        }
    }
    if (cfg.port <= 0 || cfg.port > 65535) {
        std::fprintf(stderr, "usage: remote_client --port <port> [--host <ip>]\n");
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    AppConfig cfg;
    if (!ParseArgs(argc, argv, cfg)) {
        return 1;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 1;
    }

    g_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_socket == INVALID_SOCKET) {
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(cfg.port));
    if (InetPtonA(AF_INET, cfg.host.c_str(), &addr.sin_addr) != 1) {
        closesocket(g_socket);
        WSACleanup();
        return 1;
    }
    if (connect(g_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(g_socket);
        WSACleanup();
        return 1;
    }

    HRESULT hr_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool main_com_ok = SUCCEEDED(hr_com);

    if (!g_wic.Init()) {
        if (main_com_ok) CoUninitialize();
        closesocket(g_socket);
        WSACleanup();
        return 1;
    }

    char hp[128];
    std::snprintf(hp, sizeof(hp), "%s:%d", cfg.host.c_str(), cfg.port);
    g_title_host_port = hp;

    const HINSTANCE hinst = GetModuleHandle(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "remote_client_wc";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(
        0, "remote_client_wc", "remote_client",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        kWindowWidth, kWindowHeight, nullptr, nullptr, hinst, nullptr);
    if (!hwnd) {
        if (main_com_ok) CoUninitialize();
        closesocket(g_socket);
        WSACleanup();
        return 1;
    }

    // 客户端无文本输入，永久禁用 IME，避免输入法拦截快捷键
    ImmAssociateContext(hwnd, nullptr);

    SetTimer(hwnd, 1, 1000, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    g_hwnd = hwnd;

    std::thread recv_thr(RecvThread);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    shutdown(g_socket, SD_BOTH);
    g_hwnd = nullptr;
    recv_thr.join();
    closesocket(g_socket);
    g_socket = INVALID_SOCKET;
    if (main_com_ok) CoUninitialize();
    WSACleanup();
    return 0;
}
