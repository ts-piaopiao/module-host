#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <objbase.h>

#include "jpeg_encoder.h"

struct JpegEncoder::Impl {
    IWICImagingFactory* factory = nullptr;
    bool com_initialized = false;

    ~Impl() {
        if (factory != nullptr) {
            factory->Release();
            factory = nullptr;
        }
        if (com_initialized) {
            CoUninitialize();
            com_initialized = false;
        }
    }
};

JpegEncoder::JpegEncoder() : impl_(new Impl()) {}

JpegEncoder::~JpegEncoder() {
    delete impl_;
    impl_ = nullptr;
}

bool JpegEncoder::Init() {
    if (impl_->factory != nullptr) {
        return true;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == S_OK) {
        impl_->com_initialized = true;
    } else if (hr == S_FALSE || hr == RPC_E_CHANGED_MODE) {
        impl_->com_initialized = false;
    } else if (FAILED(hr)) {
        return false;
    }

    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&impl_->factory));
    if (FAILED(hr) || impl_->factory == nullptr) {
        impl_->factory = nullptr;
        return false;
    }
    return true;
}

bool JpegEncoder::Encode(const uint8_t* bgra, uint32_t width, uint32_t height,
                         int quality, std::vector<uint8_t>* out) {
    if (bgra == nullptr || width == 0 || height == 0 ||
        quality < 1 || quality > 100 || out == nullptr) {
        return false;
    }
    out->clear();

    if (impl_->factory == nullptr) {
        if (!Init()) {
            return false;
        }
    }

    IWICBitmap* bitmap = nullptr;
    IStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;

    do {
        HRESULT hr = impl_->factory->CreateBitmapFromMemory(
            width, height, GUID_WICPixelFormat32bppBGRA,
            width * 4, width * height * 4,
            const_cast<BYTE*>(bgra), &bitmap);
        if (FAILED(hr) || bitmap == nullptr) {
            break;
        }

        stream = SHCreateMemStream(nullptr, 0);
        if (stream == nullptr) {
            break;
        }

        hr = impl_->factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
        if (FAILED(hr) || encoder == nullptr) {
            break;
        }

        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        if (FAILED(hr)) {
            break;
        }

        IPropertyBag2* props = nullptr;
        hr = encoder->CreateNewFrame(&frame, &props);
        if (FAILED(hr) || frame == nullptr) {
            if (props != nullptr) {
                props->Release();
            }
            break;
        }

        if (props != nullptr) {
            PROPBAG2 option = {};
            option.pstrName = L"ImageQuality";
            VARIANT varValue;
            VariantInit(&varValue);
            varValue.vt = VT_R4;
            varValue.fltVal = quality / 100.0f;
            props->Write(1, &option, &varValue);
            VariantClear(&varValue);
        }

        hr = frame->Initialize(props);
        if (props != nullptr) {
            props->Release();
            props = nullptr;
        }
        if (FAILED(hr)) {
            break;
        }

        hr = frame->SetSize(width, height);
        if (FAILED(hr)) {
            break;
        }

        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&format);
        if (FAILED(hr)) {
            break;
        }

        hr = frame->WriteSource(bitmap, nullptr);
        if (FAILED(hr)) {
            break;
        }

        hr = frame->Commit();
        if (FAILED(hr)) {
            break;
        }

        hr = encoder->Commit();
        if (FAILED(hr)) {
            break;
        }

        STATSTG stat = {};
        hr = stream->Stat(&stat, STATFLAG_NONAME);
        if (FAILED(hr)) {
            break;
        }
        const ULONG size = stat.cbSize.LowPart;
        out->resize(size);

        LARGE_INTEGER zero = {};
        hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        if (FAILED(hr)) {
            out->clear();
            break;
        }

        ULONG read = 0;
        hr = stream->Read(out->data(), size, &read);
        if (FAILED(hr) || read != size) {
            out->clear();
            break;
        }

        ok = true;
    } while (false);

    if (frame != nullptr) {
        frame->Release();
    }
    if (encoder != nullptr) {
        encoder->Release();
    }
    if (stream != nullptr) {
        stream->Release();
    }
    if (bitmap != nullptr) {
        bitmap->Release();
    }

    return ok;
}
