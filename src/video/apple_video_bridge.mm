#include "apple_video_bridge.hpp"

#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#include <Metal/Metal.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{

void SetError(char* output, std::size_t capacity, const char* message) {
    if (output == nullptr || capacity == 0) return;
    std::snprintf(output, capacity, "%s", message != nullptr ? message : "unknown error");
}

struct MetalState {
    id<MTLCommandQueue>       command_queue { nil };
    id<MTLComputePipelineState> nv12_pipeline { nil };
    CVMetalTextureCacheRef    texture_cache { nullptr };
};

MetalState* GetMetalState(id<MTLDevice> device, char* error, std::size_t error_size) {
    static std::mutex mutex;
    static std::unordered_map<void*, MetalState> states;
    const auto key = (__bridge void*)device;
    std::lock_guard lock(mutex);
    auto [iter, inserted] = states.try_emplace(const_cast<void*>(key));
    if (! inserted) return &iter->second;

    auto& state = iter->second;
    state.command_queue = [device newCommandQueue];
    if (state.command_queue == nil) {
        SetError(error, error_size, "failed to create Metal command queue");
        return &state;
    }
    if (CVMetalTextureCacheCreate(kCFAllocatorDefault,
                                  nullptr,
                                  device,
                                  nullptr,
                                  &state.texture_cache) != kCVReturnSuccess ||
        state.texture_cache == nullptr) {
        SetError(error, error_size, "failed to create CVMetalTextureCache");
        return &state;
    }

    static constexpr const char* shader = R"(
#include <metal_stdlib>
using namespace metal;

struct Params {
    float y_offset;
    float y_scale;
    float r_cr;
    float g_cb;
    float g_cr;
    float b_cb;
};

kernel void nv12_to_bgra(texture2d<float, access::sample> y_texture [[texture(0)]],
                         texture2d<float, access::sample> uv_texture [[texture(1)]],
                         texture2d<half, access::write> output_texture [[texture(2)]],
                         constant Params& params [[buffer(0)]],
                         uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= output_texture.get_width() || gid.y >= output_texture.get_height()) return;
    constexpr sampler sampler_state(coord::normalized, address::clamp_to_edge, filter::linear);
    const float2 uv = (float2(gid) + 0.5f) /
                      float2(output_texture.get_width(), output_texture.get_height());
    const float y = y_texture.sample(sampler_state, uv).r;
    const float2 cbcr = uv_texture.sample(sampler_state, uv).rg - float2(0.5f, 0.5f);
    const float luma = clamp((y - params.y_offset) * params.y_scale, 0.0f, 1.0f);
    const float r = saturate(luma + params.r_cr * cbcr.y);
    const float g = saturate(luma + params.g_cb * cbcr.x + params.g_cr * cbcr.y);
    const float b = saturate(luma + params.b_cb * cbcr.x);
    // MTLTexture shader values are logical RGBA even for BGRA storage.  The
    // Vulkan import uses VK_FORMAT_B8G8R8A8_UNORM, so writing (r,g,b) here
    // preserves the original video colors instead of swapping red and blue.
    output_texture.write(half4(half(r), half(g), half(b), half(1.0f)), gid);
}
)";

    NSError* library_error = nil;
    NSString* source = [NSString stringWithUTF8String:shader];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&library_error];
    if (library == nil) {
        SetError(error, error_size, "failed to compile Metal NV12 conversion shader");
        return &state;
    }
    id<MTLFunction> function = [library newFunctionWithName:@"nv12_to_bgra"];
    if (function == nil) {
        SetError(error, error_size, "failed to load Metal NV12 conversion function");
        return &state;
    }
    NSError* pipeline_error = nil;
    state.nv12_pipeline = [device newComputePipelineStateWithFunction:function
                                                                   error:&pipeline_error];
    if (state.nv12_pipeline == nil) {
        SetError(error, error_size, "failed to create Metal NV12 conversion pipeline");
    }
    return &state;
}

id<MTLTexture> CreateBgraTextureFromPixelBuffer(id<MTLDevice> device,
                                                MetalState* state,
                                                CVPixelBufferRef pixel_buffer,
                                                uint32_t width,
                                                uint32_t height,
                                                char* error,
                                                std::size_t error_size) {
    CVMetalTextureRef texture_ref = nullptr;
    const auto result = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault,
                                                                    state->texture_cache,
                                                                    pixel_buffer,
                                                                    nullptr,
                                                                    MTLPixelFormatBGRA8Unorm,
                                                                    width,
                                                                    height,
                                                                    0,
                                                                    &texture_ref);
    if (result != kCVReturnSuccess || texture_ref == nullptr) {
        SetError(error, error_size, "failed to create Metal texture from BGRA pixel buffer");
        return nil;
    }
    id<MTLTexture> texture = CVMetalTextureGetTexture(texture_ref);
    if (texture == nil) SetError(error, error_size, "CVMetalTextureCache returned null BGRA texture");
    // CVMetalTextureGetTexture returns a non-owned object. Keep the Metal
    // texture alive after releasing the CoreVideo wrapper; the caller owns
    // this retain and releases it through wavsen_apple_release_metal_texture.
    if (texture != nil) [texture retain];
    if (texture_ref != nullptr) CFRelease(texture_ref);
    return texture;
}

id<MTLTexture> CreateBgraTextureFromNv12(id<MTLDevice> device,
                                         MetalState* state,
                                         CVPixelBufferRef pixel_buffer,
                                         OSType pixel_format,
                                         uint32_t width,
                                         uint32_t height,
                                         void* reusable_texture,
                                         char* error,
                                         std::size_t error_size) {
    if (state->command_queue == nil || state->nv12_pipeline == nil) {
        SetError(error, error_size, "Metal NV12 conversion state is unavailable");
        return nil;
    }

    CVMetalTextureRef y_ref = nullptr;
    CVMetalTextureRef uv_ref = nullptr;
    const auto y_result = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault,
                                                                     state->texture_cache,
                                                                     pixel_buffer,
                                                                     nullptr,
                                                                     MTLPixelFormatR8Unorm,
                                                                     width,
                                                                     height,
                                                                     0,
                                                                     &y_ref);
    const auto uv_result = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault,
                                                                       state->texture_cache,
                                                                       pixel_buffer,
                                                                       nullptr,
                                                                       MTLPixelFormatRG8Unorm,
                                                                       width / 2,
                                                                       height / 2,
                                                                       1,
                                                                       &uv_ref);
    if (y_result != kCVReturnSuccess || uv_result != kCVReturnSuccess || y_ref == nullptr ||
        uv_ref == nullptr) {
        if (y_ref != nullptr) CFRelease(y_ref);
        if (uv_ref != nullptr) CFRelease(uv_ref);
        SetError(error, error_size, "failed to create Metal textures for NV12 planes");
        return nil;
    }

    id<MTLTexture> y_texture = CVMetalTextureGetTexture(y_ref);
    id<MTLTexture> uv_texture = CVMetalTextureGetTexture(uv_ref);
    if (y_texture == nil || uv_texture == nil) {
        CFRelease(y_ref);
        CFRelease(uv_ref);
        SetError(error, error_size, "CVMetalTextureCache returned null NV12 plane texture");
        return nil;
    }

    id<MTLTexture> output = reusable_texture != nullptr
                                ? (__bridge id<MTLTexture>)reusable_texture
                                : nil;
    if (output != nil && (output.width != width || output.height != height ||
                          output.pixelFormat != MTLPixelFormatBGRA8Unorm)) {
        output = nil;
    }
    const bool owns_output = output == nil;
    if (output == nil) {
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                width:width
                                                               height:height
                                                            mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        descriptor.storageMode = MTLStorageModeShared;
        output = [device newTextureWithDescriptor:descriptor];
    }
    if (output == nil) {
        CFRelease(y_ref);
        CFRelease(uv_ref);
        SetError(error, error_size, "failed to allocate Metal BGRA video texture");
        return nil;
    }

    id<MTLCommandBuffer> command_buffer = [state->command_queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (command_buffer == nil || encoder == nil) {
        if (owns_output) [output release];
        CFRelease(y_ref);
        CFRelease(uv_ref);
        SetError(error, error_size, "failed to allocate Metal NV12 command encoder");
        return nil;
    }

    struct Params {
        float y_offset;
        float y_scale;
        float r_cr;
        float g_cb;
        float g_cr;
        float b_cb;
    } params {
        .y_offset = pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
                        ? 16.0f / 255.0f
                        : 0.0f,
        .y_scale = pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
                       ? 255.0f / 219.0f
                       : 1.0f,
        .r_cr = 1.402f,
        .g_cb = -0.344136f,
        .g_cr = -0.714136f,
        .b_cb = 1.772f,
    };

    CFTypeRef matrix_attachment =
        CVBufferCopyAttachment(pixel_buffer, kCVImageBufferYCbCrMatrixKey, nullptr);
    if (matrix_attachment != nullptr &&
        CFEqual(matrix_attachment, kCVImageBufferYCbCrMatrix_ITU_R_709_2)) {
        params.r_cr = 1.5748f;
        params.g_cb = -0.187324f;
        params.g_cr = -0.468124f;
        params.b_cb = 1.8556f;
    } else if (matrix_attachment != nullptr &&
               CFEqual(matrix_attachment, kCVImageBufferYCbCrMatrix_ITU_R_2020)) {
        params.r_cr = 1.4746f;
        params.g_cb = -0.164553f;
        params.g_cr = -0.571353f;
        params.b_cb = 1.8814f;
    }
    if (matrix_attachment != nullptr) CFRelease(matrix_attachment);

    [encoder setComputePipelineState:state->nv12_pipeline];
    [encoder setTexture:y_texture atIndex:0];
    [encoder setTexture:uv_texture atIndex:1];
    [encoder setTexture:output atIndex:2];
    [encoder setBytes:&params length:sizeof(params) atIndex:0];
    const auto thread_width = std::max<NSUInteger>(1, std::min<NSUInteger>(16, state->nv12_pipeline.threadExecutionWidth));
    const auto thread_height = std::max<NSUInteger>(1, state->nv12_pipeline.maxTotalThreadsPerThreadgroup / thread_width);
    [encoder dispatchThreads:MTLSizeMake(width, height, 1)
        threadsPerThreadgroup:MTLSizeMake(thread_width, std::min<NSUInteger>(16, thread_height), 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];

    CFRelease(y_ref);
    CFRelease(uv_ref);
    if (command_buffer.status == MTLCommandBufferStatusError) {
        if (owns_output) [output release];
        SetError(error, error_size, "Metal NV12 conversion command failed");
        return nil;
    }
    return output;
}

} // namespace

extern "C" bool wavsen_apple_create_videotoolbox_device(AVBufferRef** out_device,
                                                         char* error,
                                                         std::size_t error_size) {
    if (out_device == nullptr) {
        SetError(error, error_size, "VideoToolbox device output is null");
        return false;
    }
    *out_device = nullptr;
    const auto result = av_hwdevice_ctx_create(out_device,
                                               AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                               nullptr,
                                               nullptr,
                                               0);
    if (result < 0 || *out_device == nullptr) {
        SetError(error, error_size, "failed to create VideoToolbox hardware device");
        return false;
    }
    return true;
}

extern "C" bool wavsen_apple_retain_videotoolbox_frame(const AVFrame* frame,
                                                        WavsenAppleVideoFrame* out,
                                                        char* error,
                                                        std::size_t error_size) {
    if (frame == nullptr || out == nullptr) {
        SetError(error, error_size, "VideoToolbox frame arguments are null");
        return false;
    }
    if (frame->format != AV_PIX_FMT_VIDEOTOOLBOX) {
        SetError(error, error_size, "decoded frame is not backed by VideoToolbox");
        return false;
    }
    auto pixel_buffer = reinterpret_cast<CVPixelBufferRef>(frame->data[3]);
    if (pixel_buffer == nullptr) {
        SetError(error, error_size, "VideoToolbox frame has no CVPixelBuffer");
        return false;
    }

    CFRetain(pixel_buffer);
    auto io_surface = CVPixelBufferGetIOSurface(pixel_buffer);
    if (io_surface != nullptr) CFRetain(io_surface);
    out->pixel_buffer = pixel_buffer;
    out->io_surface = io_surface;
    out->width = static_cast<uint32_t>(CVPixelBufferGetWidth(pixel_buffer));
    out->height = static_cast<uint32_t>(CVPixelBufferGetHeight(pixel_buffer));
    out->pixel_format = static_cast<uint32_t>(CVPixelBufferGetPixelFormatType(pixel_buffer));
    out->plane_count = CVPixelBufferIsPlanar(pixel_buffer)
                           ? static_cast<uint32_t>(CVPixelBufferGetPlaneCount(pixel_buffer))
                           : 1;
    return true;
}

extern "C" void wavsen_apple_release_video_frame(WavsenAppleVideoFrame* frame) {
    if (frame == nullptr) return;
    if (frame->io_surface != nullptr) {
        CFRelease(reinterpret_cast<IOSurfaceRef>(frame->io_surface));
        frame->io_surface = nullptr;
    }
    if (frame->pixel_buffer != nullptr) {
        CFRelease(reinterpret_cast<CVPixelBufferRef>(frame->pixel_buffer));
        frame->pixel_buffer = nullptr;
    }
}

extern "C" void* wavsen_apple_create_metal_texture(const WavsenAppleVideoFrame* frame,
                                                    void* metal_device,
                                                    void* reusable_texture,
                                                    char* error,
                                                    std::size_t error_size) {
    if (frame == nullptr || frame->pixel_buffer == nullptr || frame->width == 0 ||
        frame->height == 0) {
        SetError(error, error_size, "invalid VideoToolbox frame for Metal import");
        return nullptr;
    }

    @autoreleasepool {
        id<MTLDevice> device = metal_device != nullptr
                                   ? (__bridge id<MTLDevice>)metal_device
                                   : MTLCreateSystemDefaultDevice();
        if (device == nil) {
            SetError(error, error_size, "failed to obtain Metal device");
            return nullptr;
        }
        auto* state = GetMetalState(device, error, error_size);
        if (state == nullptr || state->texture_cache == nullptr) return nullptr;

        const auto pixel_format = static_cast<OSType>(frame->pixel_format);
        auto pixel_buffer = reinterpret_cast<CVPixelBufferRef>(frame->pixel_buffer);
        id<MTLTexture> texture = nil;
        if (pixel_format == kCVPixelFormatType_32BGRA) {
            texture = CreateBgraTextureFromPixelBuffer(device,
                                                       state,
                                                       pixel_buffer,
                                                       frame->width,
                                                       frame->height,
                                                       error,
                                                       error_size);
        } else if (pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
                   pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
            texture = CreateBgraTextureFromNv12(device,
                                                state,
                                                pixel_buffer,
                                                pixel_format,
                                                frame->width,
                                                frame->height,
                                                reusable_texture,
                                                error,
                                                error_size);
        } else {
            SetError(error, error_size, "unsupported VideoToolbox pixel format");
        }
        if (texture == nil) return nullptr;
        // Both paths return one owned Objective-C retain: the BGRA path
        // retained the CVMetalTexture's underlying texture before releasing
        // its wrapper, while the NV12 destination comes from
        // newTextureWithDescriptor: already owned by this function.
        return static_cast<void*>(texture);
    }
}

extern "C" void wavsen_apple_release_metal_texture(void* texture) {
    if (texture == nullptr) return;
    [(id)texture release];
}
