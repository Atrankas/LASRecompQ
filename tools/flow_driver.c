// flow_driver <blob.bin> <vs|fs|cs> [out.glsl]: translates a shader dumped by LARECOMP_SAVE_BLOB=1 (GLSL, as on the
// phone) and reports whether structure() took it (LARECOMP_LOG_FLOW=1: why not). For the translator's flow work
// without the phone.
//   clang -O1 -w tools/flow_driver.c runtime/nvn/maxwell/translate.c -o build/flow_driver -lm
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/nvn/maxwell/shader.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <blob.bin> <vs|fs|cs> [out.glsl]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc(n + 64);
    memset(blob, 0, n + 64);
    if (fread(blob, 1, n, f) != (size_t)n) return 1;
    fclose(f);
    int stage = !strcmp(argv[2], "vs") ? STAGE_VERTEX : !strcmp(argv[2], "cs") ? STAGE_COMPUTE : STAGE_FRAGMENT;
    maxwell_gmem_base = 0x1240000000ull;
    maxwell_gmem_size = 0xd0000000ull;
    if (getenv("FLOW_DISASM")) maxwell_disassemble(blob, n, stage, stderr);
    ShaderKey key;
    memset(&key, 0, sizeof key);
    key.glsl = 1;
    key.vs_out_mask = getenv("FLOW_VSMASK") ? (uint32_t)strtoul(getenv("FLOW_VSMASK"), NULL, 0) : 0xFFFFFFFFu;  // (a fragment shader: its varyings all provided, as on the phone)
    size_t cmax = getenv("FLOW_CMAX") ? strtoul(getenv("FLOW_CMAX"), NULL, 0) : (size_t)n;  // LARECOMP_SAVE_BLOB dumps cmax + 4 KB of constants
    ShaderInfo *si = maxwell_translate(blob, cmax, stage, &key);
    if (!si || !si->msl) { fprintf(stderr, "translation failed\n"); return 1; }
    printf("%s: %u insts, structured %d, nested %s\n", argv[1], si->num_insts, si->structured,
           strstr(si->msl, "bool exit_now = false;") ? "yes" : "no (flags or state machine)");
    if (argc > 3) {
        FILE *o = fopen(argv[3], "w");
        if (o) { fputs(si->msl, o); fclose(o); }
    }
    maxwell_free(si);
    return 0;
}
