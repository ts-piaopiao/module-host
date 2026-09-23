#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmath>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

std::string HrHex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(hr));
    return std::string(buf);
}

std::string SubtypeName(const GUID& subtype) {
    struct Pair { const GUID* guid; const char* name; };
    static const Pair kPairs[] = {
        { &MFVideoFormat_NV12,   "NV12" },
        { &MFVideoFormat_YUY2,   "YUY2" },
        { &MFVideoFormat_MJPG,   "MJPG" },
        { &MFVideoFormat_RGB24,  "RGB24" },
        { &MFVideoFormat_RGB32,  "RGB32" },
        { &MFVideoFormat_ARGB32, "ARGB32" },
        { &MFVideoFormat_I420,   "I420" },
        { &MFVideoFormat_IYUV,   "IYUV" },
        { &MFVideoFormat_YV12,   "YV12" },
        { &MFVideoFormat_H264,   "H264" },
    };
    for (const Pair& p : kPairs) {
        if (IsEqualGUID(subtype, *p.guid)) {
            return std::string(p.name);
        }
    }
    return std::string("unknown");
}

bool FormatNameToGuid(const char* name, GUID* out) {
    if (_stricmp(name, "yuy2") == 0) {
        *out = MFVideoFormat_YUY2;
        return true;
    }
    if (_stricmp(name, "mjpg") == 0) {
        *out = MFVideoFormat_MJPG;
        return true;
    }
    if (_stricmp(name, "nv12") == 0) {
        *out = MFVideoFormat_NV12;
        return true;
    }
    return false;
}

int Fail(const char* step, HRESULT hr) {
    std::printf("[错误] %s 失败: hr=%s\n", step, HrHex(hr).c_str());
    return 1;
}

std::string WideToUtf8(const wchar_t* wide) {
    if (wide == nullptr || *wide == L'\0') {
        return std::string();
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return std::string();
    }
    std::string result(static_cast<size_t>(needed - 1), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), needed, nullptr, nullptr);
    if (written <= 0) {
        return std::string();
    }
    return result;
}

std::string GetFriendlyName(IMFActivate* activate) {
    WCHAR* name = nullptr;
    UINT32 nameLen = 0;
    HRESULT hr = activate->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen);
    if (FAILED(hr) || name == nullptr) {
        if (name != nullptr) {
            CoTaskMemFree(name);
        }
        return std::string("<unknown>");
    }
    std::string utf8 = WideToUtf8(name);
    CoTaskMemFree(name);
    if (utf8.empty()) {
        return std::string("<unknown>");
    }
    return utf8;
}

HWND g_hwnd = nullptr;
bool g_quit = false;
std::vector<BYTE> g_pixels;
UINT32 g_width = 0;
UINT32 g_height = 0;
bool g_haveFrame = false;
DWORD g_firstFrameBytes = 0;
DWORD g_lastFrameBytes = 0;
bool g_printedFirstInfo = false;

void PaintFrame(HDC hdc, HWND hwnd) {
    (void)hwnd;
    if (g_pixels.empty() || g_width == 0 || g_height == 0) {
        return;
    }

    SetStretchBltMode(hdc, COLORONCOLOR);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(g_width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(g_height);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(hdc, 0, 0,
                  static_cast<int>(g_width), static_cast<int>(g_height),
                  0, 0,
                  static_cast<int>(g_width), static_cast<int>(g_height),
                  g_pixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
}

LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_DESTROY:
        g_quit = true;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            g_quit = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintFrame(hdc, hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

struct FormatInfo {
    UINT32 width = 0;
    UINT32 height = 0;
    double fps = 0.0;
};

HRESULT TrySetMediaType(IMFSourceReader* reader, bool useSize, bool useRate,
                        UINT32 width, UINT32 height, UINT32 num, UINT32 den) {
    IMFMediaType* type = nullptr;
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) {
        return hr;
    }
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (useSize) {
        type->SetUINT64(MF_MT_FRAME_SIZE,
                        (static_cast<UINT64>(width) << 32) | height);
    }
    if (useRate) {
        type->SetUINT64(MF_MT_FRAME_RATE,
                        (static_cast<UINT64>(num) << 32) | den);
    }
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type);
    type->Release();
    return hr;
}

HRESULT ConfigureOutput(IMFSourceReader* reader, FormatInfo* info,
                        const GUID& preferredSubtype) {
    UINT32 bestW = 1920;
    UINT32 bestH = 1080;
    UINT32 bestNum = 30;
    UINT32 bestDen = 1;
    double bestScore = -1.0;
    GUID bestNativeSubtype = GUID_NULL;

    for (DWORD i = 0; ; ++i) {
        IMFMediaType* native = nullptr;
        HRESULT hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &native);
        if (hr == MF_E_NO_MORE_TYPES) {
            break;
        }
        if (FAILED(hr) || native == nullptr) {
            continue;
        }

        GUID major = GUID_NULL;
        native->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (!IsEqualGUID(major, MFMediaType_Video)) {
            native->Release();
            continue;
        }

        GUID subtype = GUID_NULL;
        native->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (!IsEqualGUID(preferredSubtype, GUID_NULL) &&
            !IsEqualGUID(subtype, preferredSubtype)) {
            native->Release();
            continue;
        }

        UINT64 frameSize = 0;
        if (FAILED(native->GetUINT64(MF_MT_FRAME_SIZE, &frameSize))) {
            native->Release();
            continue;
        }
        const UINT32 w = static_cast<UINT32>(frameSize >> 32);
        const UINT32 h = static_cast<UINT32>(frameSize & 0xFFFFFFFFULL);

        UINT64 frameRate = 0;
        if (FAILED(native->GetUINT64(MF_MT_FRAME_RATE, &frameRate))) {
            native->Release();
            continue;
        }
        const UINT32 num = static_cast<UINT32>(frameRate >> 32);
        const UINT32 den = static_cast<UINT32>(frameRate & 0xFFFFFFFFULL);
        if (den == 0 || num == 0) {
            native->Release();
            continue;
        }
        const double fps = static_cast<double>(num) / static_cast<double>(den);

        double score = 0.0;
        if (w == 1920 && h == 1080) {
            score += 1000.0;
        } else {
            const double resDiff = std::abs(static_cast<double>(w * h) - 1920.0 * 1080.0);
            score -= resDiff / 1000000.0;
        }
        score -= std::abs(fps - 30.0) * 10.0;

        if (score > bestScore) {
            bestScore = score;
            bestW = w;
            bestH = h;
            bestNum = num;
            bestDen = den;
            bestNativeSubtype = subtype;
        }
        native->Release();
    }

    HRESULT hr = TrySetMediaType(reader, true, true, bestW, bestH, bestNum, bestDen);
    if (FAILED(hr)) {
        hr = TrySetMediaType(reader, true, false, bestW, bestH, bestNum, bestDen);
    }
    if (FAILED(hr)) {
        hr = TrySetMediaType(reader, false, false, bestW, bestH, bestNum, bestDen);
    }
    if (FAILED(hr)) {
        hr = TrySetMediaType(reader, true, true, 1920, 1080, 30, 1);
    }
    if (FAILED(hr)) {
        hr = TrySetMediaType(reader, false, false, 0, 0, 0, 0);
    }
    if (FAILED(hr)) {
        return hr;
    }

    IMFMediaType* current = nullptr;
    hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current);
    if (FAILED(hr)) {
        return hr;
    }

    UINT64 frameSize = 0;
    if (SUCCEEDED(current->GetUINT64(MF_MT_FRAME_SIZE, &frameSize))) {
        info->width = static_cast<UINT32>(frameSize >> 32);
        info->height = static_cast<UINT32>(frameSize & 0xFFFFFFFFULL);
    }
    UINT64 frameRate = 0;
    if (SUCCEEDED(current->GetUINT64(MF_MT_FRAME_RATE, &frameRate))) {
        const UINT32 num = static_cast<UINT32>(frameRate >> 32);
        const UINT32 den = static_cast<UINT32>(frameRate & 0xFFFFFFFFULL);
        if (den != 0) {
            info->fps = static_cast<double>(num) / static_cast<double>(den);
        }
    }
    current->Release();

    {
        const std::string nativeName = SubtypeName(bestNativeSubtype);
        std::printf("请求 native 格式: %s  期望输出: RGB32\n", nativeName.c_str());
    }

    if (info->width == 0 || info->height == 0) {
        return E_FAIL;
    }
    return S_OK;
}

HRESULT ReadFrame(IMFSourceReader* reader) {
    IMFSample* sample = nullptr;
    DWORD streamIndex = 0;
    DWORD flags = 0;
    LONGLONG timestamp = 0;
    HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                    &streamIndex, &flags, &timestamp, &sample);
    if (FAILED(hr)) {
        if (sample != nullptr) {
            sample->Release();
        }
        return hr;
    }
    if (sample == nullptr) {
        return S_OK;
    }
    if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
        sample->Release();
        g_quit = true;
        return S_OK;
    }

    IMFMediaBuffer* buffer = nullptr;
    hr = sample->ConvertToContiguousBuffer(&buffer);
    if (SUCCEEDED(hr) && buffer != nullptr) {
        BYTE* data = nullptr;
        DWORD maxLen = 0;
        DWORD curLen = 0;
        hr = buffer->Lock(&data, &maxLen, &curLen);
        if (SUCCEEDED(hr)) {
            if (data != nullptr && curLen > 0) {
                g_pixels.assign(data, data + curLen);
                for (size_t i = 3; i < g_pixels.size(); i += 4) {
                    g_pixels[i] = 0xFF;
                }
                g_lastFrameBytes = curLen;
                if (!g_printedFirstInfo && g_width > 0 && g_height > 0) {
                    g_firstFrameBytes = curLen;
                    g_printedFirstInfo = true;
                    std::printf("首帧字节数: %u  预期: %u\n",
                                static_cast<unsigned int>(curLen),
                                static_cast<unsigned int>(g_width) * g_height * 4u);
                }
                g_haveFrame = true;
            }
            buffer->Unlock();
        }
        buffer->Release();
    }
    sample->Release();
    return hr;
}

bool SaveBmp32(const char* path, const std::vector<BYTE>& pixels,
               UINT32 width, UINT32 height) {
    if (width == 0 || height == 0) {
        return false;
    }
    const DWORD dataSize = width * height * 4u;
    if (pixels.size() < dataSize) {
        return false;
    }

#pragma pack(push, 1)
    struct BitmapFileHeader {
        WORD bfType;
        DWORD bfSize;
        WORD bfReserved1;
        WORD bfReserved2;
        DWORD bfOffBits;
    };
    struct BitmapInfoHeader {
        DWORD biSize;
        LONG biWidth;
        LONG biHeight;
        WORD biPlanes;
        WORD biBitCount;
        DWORD biCompression;
        DWORD biSizeImage;
        LONG biXPelsPerMeter;
        LONG biYPelsPerMeter;
        DWORD biClrUsed;
        DWORD biClrImportant;
    };
#pragma pack(pop)

    BitmapFileHeader fh = {};
    fh.bfType = 0x4D42;
    fh.bfSize = static_cast<DWORD>(sizeof(BitmapFileHeader) + sizeof(BitmapInfoHeader) + dataSize);
    fh.bfOffBits = static_cast<DWORD>(sizeof(BitmapFileHeader) + sizeof(BitmapInfoHeader));

    BitmapInfoHeader ih = {};
    ih.biSize = sizeof(BitmapInfoHeader);
    ih.biWidth = static_cast<LONG>(width);
    ih.biHeight = -static_cast<LONG>(height);
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = dataSize;

    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "wb") != 0 || fp == nullptr) {
        return false;
    }
    const bool ok =
        fwrite(&fh, sizeof(fh), 1, fp) == 1 &&
        fwrite(&ih, sizeof(ih), 1, fp) == 1 &&
        fwrite(pixels.data(), 1, dataSize, fp) == dataSize;
    fclose(fp);
    return ok;
}

bool CreatePreviewWindow(UINT32 width, UINT32 height) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PreviewWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"CapturePreviewClass";
    if (RegisterClassExW(&wc) == 0) {
        return false;
    }

    RECT rc = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, L"CapturePreviewClass",
                             L"capture_preview",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_hwnd == nullptr) {
        return false;
    }
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    GUID preferredSubtype = GUID_NULL;
    const char* dumpPath = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--format") == 0) {
            if (i + 1 >= argc) {
                std::printf("[错误] 缺失参数: --format\n");
                return 1;
            }
            if (!FormatNameToGuid(argv[i + 1], &preferredSubtype)) {
                std::printf("[错误] 未知格式: %s\n", argv[i + 1]);
                return 1;
            }
            ++i;
        } else if (strcmp(argv[i], "--dump") == 0) {
            if (i + 1 >= argc) {
                std::printf("[错误] 缺失参数: --dump\n");
                return 1;
            }
            dumpPath = argv[++i];
        } else {
            std::printf("[错误] 未知参数: %s\n", argv[i]);
            return 1;
        }
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return Fail("CoInitializeEx", hr);
    }
    const bool coInitOk = SUCCEEDED(hr);

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        if (coInitOk) {
            CoUninitialize();
        }
        return Fail("MFStartup", hr);
    }

    IMFAttributes* attributes = nullptr;
    hr = MFCreateAttributes(&attributes, 1);
    if (FAILED(hr)) {
        MFShutdown();
        if (coInitOk) {
            CoUninitialize();
        }
        return Fail("MFCreateAttributes", hr);
    }

    hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                             MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) {
        attributes->Release();
        MFShutdown();
        if (coInitOk) {
            CoUninitialize();
        }
        return Fail("SetGUID", hr);
    }

    IMFActivate** devices = nullptr;
    UINT32 deviceCount = 0;
    hr = MFEnumDeviceSources(attributes, &devices, &deviceCount);
    attributes->Release();
    if (FAILED(hr)) {
        MFShutdown();
        if (coInitOk) {
            CoUninitialize();
        }
        return Fail("MFEnumDeviceSources", hr);
    }

    if (deviceCount == 0 || devices == nullptr || devices[0] == nullptr) {
        if (devices != nullptr) {
            CoTaskMemFree(devices);
        }
        MFShutdown();
        if (coInitOk) {
            CoUninitialize();
        }
        std::printf("[错误] 未发现视频采集设备\n");
        return 1;
    }

    const std::string deviceName = GetFriendlyName(devices[0]);
    std::printf("使用设备: [0] %s\n", deviceName.c_str());

    IMFMediaSource* source = nullptr;
    hr = devices[0]->ActivateObject(IID_PPV_ARGS(&source));
    if (FAILED(hr) || source == nullptr) {
        if (source != nullptr) {
            source->Release();
        }
        devices[0]->Release();
        CoTaskMemFree(devices);
        MFShutdown();
        if (coInitOk) {
            CoUninitialize();
        }
        return Fail("ActivateObject", hr);
    }

    IMFSourceReader* reader = nullptr;
    IMFAttributes* readerAttr = nullptr;
    hr = MFCreateAttributes(&readerAttr, 2);
    if (SUCCEEDED(hr)) {
        readerAttr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        readerAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        hr = MFCreateSourceReaderFromMediaSource(source, readerAttr, &reader);
        readerAttr->Release();
    }
    if (FAILED(hr) || reader == nullptr) {
        if (reader != nullptr) {
            reader->Release();
        }
        source->Shutdown();
        source->Release();
        devices[0]->Release();
        CoTaskMemFree(devices);
        MFShutdown();
        if (coInitOk) {
            CoUninitialize();
        }
        return Fail("MFCreateSourceReaderFromMediaSource", hr);
    }

    FormatInfo info;
    hr = ConfigureOutput(reader, &info, preferredSubtype);
    if (FAILED(hr)) {
        reader->Release();
        source->Shutdown();
        source->Release();
        devices[0]->Release();
        CoTaskMemFree(devices);
        MFShutdown();
        if (coInitOk) {
            CoUninitialize();
        }
        return Fail("SetCurrentMediaType", hr);
    }

    g_width = info.width;
    g_height = info.height;
    std::printf("输出配置: %ux%u @ %.2f fps format=RGB32\n",
                g_width, g_height, info.fps);

    if (dumpPath != nullptr) {
        bool gotFrame = false;
        for (int attempt = 0; attempt < 64 && !gotFrame && !g_quit; ++attempt) {
            hr = ReadFrame(reader);
            if (FAILED(hr)) {
                reader->Release();
                source->Shutdown();
                source->Release();
                devices[0]->Release();
                CoTaskMemFree(devices);
                MFShutdown();
                if (coInitOk) {
                    CoUninitialize();
                }
                return Fail("ReadSample", hr);
            }
            gotFrame = g_haveFrame;
        }
        if (!gotFrame || g_pixels.empty()) {
            reader->Release();
            source->Shutdown();
            source->Release();
            devices[0]->Release();
            CoTaskMemFree(devices);
            MFShutdown();
            if (coInitOk) {
                CoUninitialize();
            }
            std::printf("[错误] 读取帧失败\n");
            return 1;
        }
        g_haveFrame = false;
        if (!SaveBmp32(dumpPath, g_pixels, g_width, g_height)) {
            reader->Release();
            source->Shutdown();
            source->Release();
            devices[0]->Release();
            CoTaskMemFree(devices);
            MFShutdown();
            if (coInitOk) {
                CoUninitialize();
            }
            std::printf("[错误] 保存 BMP 失败: %s\n", dumpPath);
            return 1;
        }
        const DWORD bytes = g_width * g_height * 4u;
        std::printf("已保存: %s  分辨率: %ux%u  字节数: %u\n",
                    dumpPath, g_width, g_height,
                    static_cast<unsigned int>(bytes));
        reader->Release();
        source->Shutdown();
        source->Release();
        devices[0]->Release();
        CoTaskMemFree(devices);
        MFShutdown();
        if (coInitOk) {
            CoUninitialize();
        }
        return 0;
    }

    if (!CreatePreviewWindow(g_width, g_height)) {
        const DWORD err = GetLastError();
        reader->Release();
        source->Shutdown();
        source->Release();
        devices[0]->Release();
        CoTaskMemFree(devices);
        MFShutdown();
        if (coInitOk) {
            CoUninitialize();
        }
        std::printf("[错误] CreateWindow 失败: hr=0x%08X\n",
                    static_cast<unsigned int>(err));
        return 1;
    }

    LARGE_INTEGER qpcFreq = {};
    LARGE_INTEGER qpcLast = {};
    LARGE_INTEGER qpcNow = {};
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcLast);
    long long frameCount = 0;

    while (!g_quit) {
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit) {
            break;
        }

        if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
            break;
        }

        hr = ReadFrame(reader);
        if (FAILED(hr)) {
            reader->Release();
            source->Shutdown();
            source->Release();
            devices[0]->Release();
            CoTaskMemFree(devices);
            if (g_hwnd != nullptr) {
                DestroyWindow(g_hwnd);
                g_hwnd = nullptr;
            }
            MFShutdown();
            if (coInitOk) {
                CoUninitialize();
            }
            return Fail("ReadSample", hr);
        }

        if (g_haveFrame) {
            g_haveFrame = false;
            ++frameCount;
            InvalidateRect(g_hwnd, nullptr, FALSE);
            UpdateWindow(g_hwnd);
        }

        QueryPerformanceCounter(&qpcNow);
        const double elapsed =
            static_cast<double>(qpcNow.QuadPart - qpcLast.QuadPart) /
            static_cast<double>(qpcFreq.QuadPart);
        if (elapsed >= 2.0) {
            const double fps = static_cast<double>(frameCount) / elapsed;
            const UINT32 strideHint =
                (g_height != 0) ? (g_lastFrameBytes / g_height) : 0;
            std::printf("实际帧率: %.1f fps, 分辨率: %ux%u, format=RGB32, stride_hint=%u\n",
                        fps, g_width, g_height,
                        static_cast<unsigned int>(strideHint));
            frameCount = 0;
            qpcLast = qpcNow;
        }
    }

    reader->Release();
    source->Shutdown();
    source->Release();
    devices[0]->Release();
    CoTaskMemFree(devices);
    if (g_hwnd != nullptr) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    MFShutdown();
    if (coInitOk) {
        CoUninitialize();
    }
    return 0;
}
