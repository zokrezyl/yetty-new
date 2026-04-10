#include <yetty/core/types.h>
#include <webgpu/webgpu.h>

size_t yetty_texture_get_size(const struct yetty_texture *texture)
{
    if (!texture || texture->width == 0 || texture->height == 0)
        return 0;

    size_t bpp;
    switch ((WGPUTextureFormat)texture->format) {
    case WGPUTextureFormat_R8Unorm:
        bpp = 1;
        break;
    case WGPUTextureFormat_RG8Unorm:
        bpp = 2;
        break;
    case WGPUTextureFormat_RGBA8Unorm:
    case WGPUTextureFormat_RGBA8UnormSrgb:
    case WGPUTextureFormat_BGRA8Unorm:
    case WGPUTextureFormat_BGRA8UnormSrgb:
        bpp = 4;
        break;
    default:
        bpp = 4;
        break;
    }

    return (size_t)texture->width * (size_t)texture->height * bpp;
}
