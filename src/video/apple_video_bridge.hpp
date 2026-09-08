#pragma once

#include <cstddef>
#include <cstdint>

struct AVBufferRef;
struct AVFrame;

// This header deliberately contains only C-compatible opaque handles. The
// Objective-C++ implementation owns CoreVideo/Metal references and keeps the
// module interface portable to Linux.
struct WavsenAppleVideoFrame {
    void*    pixel_buffer { nullptr };
    void*    io_surface { nullptr };
    uint32_t width { 0 };
    uint32_t height { 0 };
    uint32_t pixel_format { 0 };
    uint32_t plane_count { 0 };
    double   pts_seconds { -1.0 };
    uint32_t colorspace { 0 };
    uint32_t color_range { 0 };
};

extern "C" {

bool wavsen_apple_create_videotoolbox_device(AVBufferRef** out_device,
                                             char* error,
                                             std::size_t error_size);
bool wavsen_apple_retain_videotoolbox_frame(const AVFrame* frame,
                                            WavsenAppleVideoFrame* out,
                                            char* error,
                                            std::size_t error_size);
void wavsen_apple_release_video_frame(WavsenAppleVideoFrame* frame);

void* wavsen_apple_create_metal_texture(const WavsenAppleVideoFrame* frame,
                                        void* metal_device,
                                        void* reusable_texture,
                                        char* error,
                                        std::size_t error_size);
void wavsen_apple_release_metal_texture(void* texture);

}
