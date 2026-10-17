// BC1-7 decoding on the CPU for GPUs without BC formats (phones: Mali, PowerVR), with bcdec (third_party/bcdec,
// public domain / MIT). runtime/nvn/vulkan/textures.c uploads the result in the format bcn_decoded_format names.
#include <string.h>

#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE  // (the signed BC4 / BC5 variants)
#include "../../third_party/bcdec/bcdec.h"

#include "bcn.h"

int bcn_block_bytes(int kind) { return kind == BCN_1 || kind == BCN_4U || kind == BCN_4S ? 8 : 16; }
int bcn_texel_bytes(int kind) {
    switch (kind) {
    case BCN_4U: case BCN_4S: return 1;
    case BCN_5U: case BCN_5S: return 2;
    case BCN_6HS: case BCN_6HU: return 8;  // RGBA16F
    default: return 4;
    }
}

// One 4x4 block into tmp, rows of 4 texels
static void block(int kind, const uint8_t *src, uint8_t *tmp) {
    int tb = bcn_texel_bytes(kind);
    switch (kind) {
    case BCN_1: bcdec_bc1(src, tmp, 4 * tb); break;
    case BCN_2: bcdec_bc2(src, tmp, 4 * tb); break;
    case BCN_3: bcdec_bc3(src, tmp, 4 * tb); break;
    case BCN_4U: case BCN_4S: bcdec_bc4(src, tmp, 4 * tb, kind == BCN_4S); break;
    case BCN_5U: case BCN_5S: bcdec_bc5(src, tmp, 4 * tb, kind == BCN_5S); break;
    case BCN_7: bcdec_bc7(src, tmp, 4 * tb); break;
    case BCN_6HS: case BCN_6HU: {  // RGB half -> RGBA half (alpha 1.0)
        uint16_t rgb[16 * 3];
        bcdec_bc6h_half(src, rgb, 4 * 3, kind == BCN_6HS);
        uint16_t *o = (uint16_t *)tmp;
        for (int i = 0; i < 16; i++) o[i * 4] = rgb[i * 3], o[i * 4 + 1] = rgb[i * 3 + 1], o[i * 4 + 2] = rgb[i * 3 + 2], o[i * 4 + 3] = 0x3C00;
        break;
    }
    }
}

void bcn_decode(int kind, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *out) {
    uint32_t wb = (width + 3) / 4, hb = (height + 3) / 4, bb = (uint32_t)bcn_block_bytes(kind), tb = (uint32_t)bcn_texel_bytes(kind);
    uint8_t tmp[16 * 8];
    for (uint32_t by = 0; by < hb; by++)
        for (uint32_t bx = 0; bx < wb; bx++) {
            block(kind, blocks + ((uint64_t)by * wb + bx) * bb, tmp);
            uint32_t w = width - bx * 4 < 4 ? width - bx * 4 : 4, h = height - by * 4 < 4 ? height - by * 4 : 4;
            for (uint32_t y = 0; y < h; y++)
                memcpy(out + ((uint64_t)(by * 4 + y) * width + bx * 4) * tb, tmp + y * 4 * tb, w * tb);
        }
}
