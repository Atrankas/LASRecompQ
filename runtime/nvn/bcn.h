// BC1-7 to uncompressed texels on the CPU (runtime/nvn/bcn.c), for GPUs without BC formats.
#pragma once
#include <stdint.h>

// what is decoded: BC1-3 / BC7 -> RGBA8, BC4 -> R8, BC5 -> RG8 (unsigned / signed), BC6H -> RGBA16F
enum { BCN_NONE, BCN_1, BCN_2, BCN_3, BCN_4U, BCN_4S, BCN_5U, BCN_5S, BCN_6HS, BCN_6HU, BCN_7 };

int bcn_block_bytes(int kind);  // 8 or 16
int bcn_texel_bytes(int kind);  // of the decoded texels
// blocks: row-major blocks covering ceil(width/4) x ceil(height/4); out: width*height texels, tightly packed rows
void bcn_decode(int kind, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *out);
