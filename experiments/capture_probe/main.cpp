#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace {

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

std::string HrHex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(hr));
    return std::string(buf);
}

std::string GuidToString(const GUID& guid) {
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
                  static_cast<unsigned long>(guid.Data1),
                  static_cast<unsigned short>(guid.Data2),
                  static_cast<unsigned short>(guid.Data3),
                  guid.Data4[0], guid.Data4[1],
                  guid.Data4[2], guid.Data4[3],
                  guid.Data4[4], guid.Data4[5],
                  guid.Data4[6], guid.Data4[7]);
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
    return GuidToString(subtype);
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

void EnumerateFormats(IMFActivate* activate, UINT32 deviceIndex, const std::string& deviceName) {
    IMFMediaSource* source = nullptr;
    HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(&source));
    if (FAILED(hr) || source == nullptr) {
        std::printf("[错误] ActivateObject 失败: hr=%s\n", HrHex(hr).c_str());
        if (source != nullptr) {
            source->Release();
        }
        return;
    }

    IMFSourceReader* reader = nullptr;
    hr = MFCreateSourceReaderFromMediaSource(source, nullptr, &reader);
    if (FAILED(hr) || reader == nullptr) {
        std::printf("[错误] MFCreateSourceReaderFromMediaSource 失败: hr=%s\n", HrHex(hr).c_str());
        if (reader != nullptr) {
            reader->Release();
        }
        source->Shutdown();
        source->Release();
        return;
    }

    IMFPresentationDescriptor* presDesc = nullptr;
    hr = source->CreatePresentationDescriptor(&presDesc);
    if (FAILED(hr) || presDesc == nullptr) {
        std::printf("[错误] CreatePresentationDescriptor 失败: hr=%s\n", HrHex(hr).c_str());
        if (presDesc != nullptr) {
            presDesc->Release();
        }
        reader->Release();
        source->Shutdown();
        source->Release();
        return;
    }

    DWORD streamCount = 0;
    BOOL selected = FALSE;
    IMFStreamDescriptor* streamDesc = nullptr;
    hr = presDesc->GetStreamDescriptorCount(&streamCount);
    if (FAILED(hr) || streamCount == 0) {
        std::printf("[错误] GetStreamDescriptorCount 失败: hr=%s\n", HrHex(hr).c_str());
        presDesc->Release();
        reader->Release();
        source->Shutdown();
        source->Release();
        return;
    }

    hr = presDesc->GetStreamDescriptorByIndex(0, &selected, &streamDesc);
    if (FAILED(hr) || streamDesc == nullptr) {
        std::printf("[错误] GetStreamDescriptorByIndex 失败: hr=%s\n", HrHex(hr).c_str());
        if (streamDesc != nullptr) {
            streamDesc->Release();
        }
        presDesc->Release();
        reader->Release();
        source->Shutdown();
        source->Release();
        return;
    }

    IMFMediaTypeHandler* handler = nullptr;
    hr = streamDesc->GetMediaTypeHandler(&handler);
    if (FAILED(hr) || handler == nullptr) {
        std::printf("[错误] GetMediaTypeHandler 失败: hr=%s\n", HrHex(hr).c_str());
        if (handler != nullptr) {
            handler->Release();
        }
        streamDesc->Release();
        presDesc->Release();
        reader->Release();
        source->Shutdown();
        source->Release();
        return;
    }

    DWORD typeCount = 0;
    hr = handler->GetMediaTypeCount(&typeCount);
    if (FAILED(hr)) {
        std::printf("[错误] GetMediaTypeCount 失败: hr=%s\n", HrHex(hr).c_str());
        handler->Release();
        streamDesc->Release();
        presDesc->Release();
        reader->Release();
        source->Shutdown();
        source->Release();
        return;
    }

    std::printf("设备 [%u] %s 支持格式:\n", deviceIndex, deviceName.c_str());

    for (DWORD i = 0; i < typeCount; ++i) {
        IMFMediaType* mediaType = nullptr;
        hr = handler->GetMediaTypeByIndex(i, &mediaType);
        if (FAILED(hr) || mediaType == nullptr) {
            std::printf("[错误] GetMediaTypeByIndex 失败: hr=%s\n", HrHex(hr).c_str());
            if (mediaType != nullptr) {
                mediaType->Release();
            }
            continue;
        }

        UINT32 width = 0;
        UINT32 height = 0;
        UINT64 frameSize = 0;
        if (SUCCEEDED(mediaType->GetUINT64(MF_MT_FRAME_SIZE, &frameSize))) {
            width = static_cast<UINT32>(frameSize >> 32);
            height = static_cast<UINT32>(frameSize & 0xFFFFFFFFULL);
        }

        UINT32 num = 0;
        UINT32 den = 0;
        UINT64 frameRate = 0;
        bool hasRate = SUCCEEDED(mediaType->GetUINT64(MF_MT_FRAME_RATE, &frameRate));
        if (hasRate) {
            num = static_cast<UINT32>(frameRate >> 32);
            den = static_cast<UINT32>(frameRate & 0xFFFFFFFFULL);
        }

        GUID subtype = GUID_NULL;
        mediaType->GetGUID(MF_MT_SUBTYPE, &subtype);
        const std::string fmtName = SubtypeName(subtype);

        char rateBuf[32];
        if (!hasRate || den == 0) {
            std::snprintf(rateBuf, sizeof(rateBuf), "unknown");
        } else {
            const double fps = static_cast<double>(num) / static_cast<double>(den);
            std::snprintf(rateBuf, sizeof(rateBuf), "%.2f", fps);
        }

        std::printf("[%2u] %ux%u @ %s  fps  format=%s\n",
                    i, width, height, rateBuf, fmtName.c_str());
        mediaType->Release();
    }

    std::printf("格式总数: %u\n", typeCount);

    handler->Release();
    streamDesc->Release();
    presDesc->Release();
    reader->Release();
    source->Shutdown();
    source->Release();
}

}  // namespace

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    int deviceIndex = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--device") == 0) {
            if (i + 1 >= argc) {
                std::printf("[错误] 缺失参数: --device\n");
                return 1;
            }
            deviceIndex = std::atoi(argv[++i]);
            if (deviceIndex < 0) {
                std::printf("[错误] 无效设备索引: %d\n", deviceIndex);
                return 1;
            }
        } else {
            std::printf("[错误] 未知参数: %s\n", argv[i]);
            return 1;
        }
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::printf("[错误] CoInitializeEx 失败: hr=%s\n", HrHex(hr).c_str());
        return 1;
    }
    const bool coInitOk = SUCCEEDED(hr);

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::printf("[错误] MFStartup 失败: hr=%s\n", HrHex(hr).c_str());
        if (coInitOk) {
            CoUninitialize();
        }
        return 1;
    }

    IMFAttributes* attributes = nullptr;
    hr = MFCreateAttributes(&attributes, 1);
    if (FAILED(hr)) {
        std::printf("[错误] MFCreateAttributes 失败: hr=%s\n", HrHex(hr).c_str());
        MFShutdown();
        if (coInitOk) {
            CoUninitialize();
        }
        return 1;
    }

    hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                             MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) {
        std::printf("[错误] SetGUID 失败: hr=%s\n", HrHex(hr).c_str());
        attributes->Release();
        MFShutdown();
        if (coInitOk) {
            CoUninitialize();
        }
        return 1;
    }

    IMFActivate** devices = nullptr;
    UINT32 deviceCount = 0;
    hr = MFEnumDeviceSources(attributes, &devices, &deviceCount);
    attributes->Release();
    if (FAILED(hr)) {
        std::printf("[错误] MFEnumDeviceSources 失败: hr=%s\n", HrHex(hr).c_str());
        MFShutdown();
        if (coInitOk) {
            CoUninitialize();
        }
        return 1;
    }

    std::printf("视频采集设备列表:\n");

    if (deviceCount == 0 || devices == nullptr) {
        std::printf("未发现视频采集设备\n");
    } else {
        for (UINT32 i = 0; i < deviceCount; ++i) {
            if (devices[i] == nullptr) {
                std::printf("[%u] <null>  状态=active\n", i);
                continue;
            }
            const std::string name = GetFriendlyName(devices[i]);
            std::printf("[%u] %s  状态=active\n", i, name.c_str());
        }
    }

    std::printf("设备总数: %u\n", deviceCount);

    if (deviceCount > 0 && devices != nullptr) {
        if (static_cast<UINT32>(deviceIndex) >= deviceCount) {
            std::printf("[错误] 设备索引越界: %d (总数=%u)\n", deviceIndex, deviceCount);
        } else if (devices[deviceIndex] != nullptr) {
            const std::string name = GetFriendlyName(devices[deviceIndex]);
            EnumerateFormats(devices[deviceIndex], static_cast<UINT32>(deviceIndex), name);
        }
    }

    if (devices != nullptr) {
        for (UINT32 i = 0; i < deviceCount; ++i) {
            if (devices[i] != nullptr) {
                devices[i]->Release();
                devices[i] = nullptr;
            }
        }
        CoTaskMemFree(devices);
    }

    MFShutdown();
    if (coInitOk) {
        CoUninitialize();
    }
    return 0;
}
