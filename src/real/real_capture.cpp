#include "core_contract.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct CaptureConfig {
    int device = 0;
    UINT32 width = 1920;
    UINT32 height = 1080;
    UINT32 fps = 30;
    std::string format = "auto";
};

struct FormatInfo {
    UINT32 width = 0;
    UINT32 height = 0;
    double fps = 0.0;
};

bool g_inited = false;
bool g_need_com_uninit = false;
IMFMediaSource* g_source = nullptr;
IMFSourceReader* g_reader = nullptr;
UINT32 g_out_width = 0;
UINT32 g_out_height = 0;
ULONGLONG g_first_pts = 0;
std::vector<uint8_t> g_frame_buf;

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

void ParseConfig(const char* text, CaptureConfig* cfg) {
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
        if (key == "capture_device") {
            if (ParseInt(value, &num)) {
                cfg->device = num;
            }
        } else if (key == "capture_width") {
            if (ParseInt(value, &num) && num > 0) {
                cfg->width = static_cast<UINT32>(num);
            }
        } else if (key == "capture_height") {
            if (ParseInt(value, &num) && num > 0) {
                cfg->height = static_cast<UINT32>(num);
            }
        } else if (key == "capture_fps") {
            if (ParseInt(value, &num) && num > 0) {
                cfg->fps = static_cast<UINT32>(num);
            }
        } else if (key == "capture_format") {
            if (!value.empty()) {
                cfg->format = value;
            }
        }
    }
}

bool FormatNameToGuid(const std::string& name, GUID* out) {
    if (_stricmp(name.c_str(), "yuy2") == 0) {
        *out = MFVideoFormat_YUY2;
        return true;
    }
    if (_stricmp(name.c_str(), "mjpg") == 0) {
        *out = MFVideoFormat_MJPG;
        return true;
    }
    if (_stricmp(name.c_str(), "nv12") == 0) {
        *out = MFVideoFormat_NV12;
        return true;
    }
    return false;
}

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
                        const GUID& preferredSubtype, const CaptureConfig& cfg) {
    const double targetW = static_cast<double>(cfg.width);
    const double targetH = static_cast<double>(cfg.height);
    const double targetFps = static_cast<double>(cfg.fps);

    UINT32 bestW = cfg.width;
    UINT32 bestH = cfg.height;
    UINT32 bestNum = cfg.fps;
    UINT32 bestDen = 1;
    double bestScore = -1.0;

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
        if (static_cast<double>(w) == targetW && static_cast<double>(h) == targetH) {
            score += 1000.0;
        } else {
            const double resDiff = std::abs(static_cast<double>(w) * static_cast<double>(h) - targetW * targetH);
            score -= resDiff / 1000000.0;
        }
        score -= std::abs(fps - targetFps) * 10.0;

        if (score > bestScore) {
            bestScore = score;
            bestW = w;
            bestH = h;
            bestNum = num;
            bestDen = den;
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
        hr = TrySetMediaType(reader, true, true, cfg.width, cfg.height, cfg.fps, 1);
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

    UINT64 frameSizeOut = 0;
    if (SUCCEEDED(current->GetUINT64(MF_MT_FRAME_SIZE, &frameSizeOut))) {
        info->width = static_cast<UINT32>(frameSizeOut >> 32);
        info->height = static_cast<UINT32>(frameSizeOut & 0xFFFFFFFFULL);
    }
    UINT64 frameRateOut = 0;
    if (SUCCEEDED(current->GetUINT64(MF_MT_FRAME_RATE, &frameRateOut))) {
        const UINT32 n = static_cast<UINT32>(frameRateOut >> 32);
        const UINT32 d = static_cast<UINT32>(frameRateOut & 0xFFFFFFFFULL);
        if (d != 0) {
            info->fps = static_cast<double>(n) / static_cast<double>(d);
        }
    }
    current->Release();

    if (info->width == 0 || info->height == 0) {
        return E_FAIL;
    }
    return S_OK;
}

void CleanupInit() {
    if (g_reader != nullptr) {
        g_reader->Release();
        g_reader = nullptr;
    }
    if (g_source != nullptr) {
        g_source->Shutdown();
        g_source->Release();
        g_source = nullptr;
    }
    if (g_inited) {
        MFShutdown();
        g_inited = false;
    }
    if (g_need_com_uninit) {
        CoUninitialize();
        g_need_com_uninit = false;
    }
    g_out_width = 0;
    g_out_height = 0;
    g_first_pts = 0;
    g_frame_buf.clear();
}

core_error DoInit(uint32_t host_abi, const char* config) {
    if (g_inited || g_reader != nullptr) {
        return CORE_ERR_INIT;
    }
    if (host_abi != CORE_ABI_VERSION) {
        return CORE_ERR_ABI_MISMATCH;
    }

    CaptureConfig cfg;
    ParseConfig(config, &cfg);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == S_OK) {
        g_need_com_uninit = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        g_need_com_uninit = false;
    } else if (FAILED(hr)) {
        return CORE_ERR_INIT;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        if (g_need_com_uninit) {
            CoUninitialize();
            g_need_com_uninit = false;
        }
        return CORE_ERR_INIT;
    }
    g_inited = true;

    IMFAttributes* attributes = nullptr;
    hr = MFCreateAttributes(&attributes, 1);
    if (FAILED(hr)) {
        CleanupInit();
        return CORE_ERR_INIT;
    }
    hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                             MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) {
        attributes->Release();
        CleanupInit();
        return CORE_ERR_INIT;
    }

    IMFActivate** devices = nullptr;
    UINT32 deviceCount = 0;
    hr = MFEnumDeviceSources(attributes, &devices, &deviceCount);
    attributes->Release();
    if (FAILED(hr)) {
        CleanupInit();
        return CORE_ERR_INIT;
    }

    if (devices == nullptr || deviceCount == 0 ||
        cfg.device < 0 || static_cast<UINT32>(cfg.device) >= deviceCount ||
        devices[cfg.device] == nullptr) {
        if (devices != nullptr) {
            for (UINT32 i = 0; i < deviceCount; ++i) {
                if (devices[i] != nullptr) {
                    devices[i]->Release();
                }
            }
            CoTaskMemFree(devices);
        }
        CleanupInit();
        return CORE_ERR_INIT;
    }

    hr = devices[cfg.device]->ActivateObject(IID_PPV_ARGS(&g_source));
    for (UINT32 i = 0; i < deviceCount; ++i) {
        if (devices[i] != nullptr) {
            devices[i]->Release();
        }
    }
    CoTaskMemFree(devices);
    if (FAILED(hr) || g_source == nullptr) {
        CleanupInit();
        return CORE_ERR_INIT;
    }

    IMFAttributes* readerAttr = nullptr;
    hr = MFCreateAttributes(&readerAttr, 2);
    if (SUCCEEDED(hr)) {
        readerAttr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        readerAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        hr = MFCreateSourceReaderFromMediaSource(g_source, readerAttr, &g_reader);
        readerAttr->Release();
    }
    if (FAILED(hr) || g_reader == nullptr) {
        CleanupInit();
        return CORE_ERR_INIT;
    }

    GUID preferredSubtype = GUID_NULL;
    if (cfg.format != "auto") {
        FormatNameToGuid(cfg.format, &preferredSubtype);
    }

    FormatInfo info;
    hr = ConfigureOutput(g_reader, &info, preferredSubtype, cfg);
    if (FAILED(hr)) {
        CleanupInit();
        return CORE_ERR_INIT;
    }

    g_out_width = info.width;
    g_out_height = info.height;
    g_first_pts = 0;
    g_frame_buf.clear();
    return CORE_OK;
}

}  // namespace

extern "C" {

const char* plugin_meta(void) {
    return "real_capture|1.0.0|3|capture";
}

core_error plugin_init(uint32_t host_abi, const char* config) {
    return DoInit(host_abi, config);
}

core_error plugin_release(void) {
    CleanupInit();
    return CORE_OK;
}

core_error plugin_capture(core_frame* out) {
    if (out == nullptr) {
        return CORE_ERR_CAPTURE;
    }
    if (!g_inited || g_reader == nullptr || g_out_width == 0 || g_out_height == 0) {
        return CORE_ERR_CAPTURE;
    }

    const ULONGLONG deadline = GetTickCount64() + 1000;
    IMFSample* sample = nullptr;
    for (;;) {
        sample = nullptr;
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        HRESULT hr = g_reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                          &streamIndex, &flags, &timestamp, &sample);
        if (FAILED(hr)) {
            if (sample != nullptr) {
                sample->Release();
            }
            return CORE_ERR_CAPTURE;
        }
        if (sample != nullptr) {
            break;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            return CORE_ERR_CAPTURE;
        }
        if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
            return CORE_ERR_CAPTURE;
        }
        if (GetTickCount64() >= deadline) {
            return CORE_ERR_CAPTURE;
        }
    }

    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr) || buffer == nullptr) {
        if (buffer != nullptr) {
            buffer->Release();
        }
        sample->Release();
        return CORE_ERR_CAPTURE;
    }

    BYTE* data = nullptr;
    DWORD maxLen = 0;
    DWORD curLen = 0;
    hr = buffer->Lock(&data, &maxLen, &curLen);
    if (FAILED(hr)) {
        buffer->Release();
        sample->Release();
        return CORE_ERR_CAPTURE;
    }

    const size_t need = static_cast<size_t>(g_out_width) * g_out_height * 4u;
    if (g_frame_buf.size() != need) {
        g_frame_buf.assign(need, 0);
    }

    core_error result = CORE_OK;
    if (data == nullptr || curLen == 0) {
        result = CORE_ERR_CAPTURE;
    } else {
        // 按行拷贝，忽略每行尾部对齐填充，避免错位出现黑色斜线
        UINT32 src_stride = g_out_width * 4u;
        if (curLen > need && (curLen % g_out_height) == 0) {
            const UINT32 s = static_cast<UINT32>(curLen / g_out_height);
            if (s >= g_out_width * 4u && (s % 4u) == 0) {
                src_stride = s;
            }
        }
        const size_t row_bytes = static_cast<size_t>(g_out_width) * 4u;
        const size_t rows_available = static_cast<size_t>(curLen) / src_stride;
        const UINT32 rows = (rows_available < g_out_height)
                                ? static_cast<UINT32>(rows_available)
                                : g_out_height;
        for (UINT32 y = 0; y < rows; ++y) {
            std::memcpy(g_frame_buf.data() + static_cast<size_t>(y) * row_bytes,
                        data + static_cast<size_t>(y) * src_stride,
                        row_bytes);
        }
        for (size_t i = 3; i < need; i += 4) {
            g_frame_buf[i] = 0xFF;
        }

        const ULONGLONG now = GetTickCount64();
        if (g_first_pts == 0) {
            g_first_pts = now;
        }

        out->width = g_out_width;
        out->height = g_out_height;
        out->stride = g_out_width * 4u;
        out->format = CORE_PIXEL_FORMAT_BGRA8;
        out->data = g_frame_buf.data();
        out->size = need;
        out->pts_ms = static_cast<int64_t>(now - g_first_pts);
    }

    buffer->Unlock();
    buffer->Release();
    sample->Release();
    return result;
}

core_error plugin_decide(const core_intent* intent, core_decision* out) {
    (void)intent;
    (void)out;
    return CORE_OK;
}

core_error plugin_execute(const core_decision* decision, core_execute_result* out) {
    (void)decision;
    (void)out;
    return CORE_OK;
}

}  // extern "C"
