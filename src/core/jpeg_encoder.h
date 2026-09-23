#ifndef MODULE_HOST_CORE_JPEG_ENCODER_H
#define MODULE_HOST_CORE_JPEG_ENCODER_H

#include <cstdint>
#include <vector>

class JpegEncoder {
public:
    JpegEncoder();
    ~JpegEncoder();

    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;

    bool Init();

    bool Encode(const uint8_t* bgra, uint32_t width, uint32_t height,
                int quality, std::vector<uint8_t>* out);

private:
    struct Impl;
    Impl* impl_;
};

#endif
