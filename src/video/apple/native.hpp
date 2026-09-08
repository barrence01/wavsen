#pragma once

struct WavsenMetalAdapter;
struct WavsenMetalTexture;

const char* wavsen_metal_create(void* device, WavsenMetalAdapter** out);
void wavsen_metal_destroy(WavsenMetalAdapter* adapter);
const char* wavsen_metal_import(WavsenMetalAdapter* adapter, void* pixel_buffer,
                               WavsenMetalTexture** texture);
void wavsen_metal_texture_destroy(WavsenMetalTexture* texture);
void* wavsen_metal_texture_handle(const WavsenMetalTexture* texture);
