// Hook points: instructions of the game's code that call into the runtime (tools/patches.py HOOKS). The generated
// code at a hook site runs `rt_hook(c, site)` first; what a hook does lives here, so changing it rebuilds the app,
// not the game code.
//
// rt_hook returns 0: run the site's own instruction, 1: the hook did it all, 2..: jump to the site's targets
// (HOOKS[site][1], in order).
#include <stdio.h>
#include <string.h>

#include "../cpu.h"
#include "../rt.h"

void rt_mb_seen(uint64_t o);  // miniboss.c
int rt_gohma_free(void);
uint32_t rt_gohma_heading(uint64_t gohma, uint32_t track);
int rt_crashout_skip(void);
void rt_dig_hole(Cpu *c);  // digs.c
uint64_t rt_hole_cap(void);

int rt_hook(Cpu *c, uint64_t site) {
    switch (site + GUEST_SHIFT) {  // (the sites by their unshifted address, as in tools/patches.py)
    case 0x70000ec354:  // MB_SEEN_SITE - ldr x8, [x21]: an actor seen
        rt_mb_seen(c->x[21]);
        return 0;
    case 0x7000d97e2c:  // CRASHOUT_SITE - str wzr, [x0, #0xb8]: Blaino's knock-out, no crash-out on the Field
        if (!rt_crashout_skip()) return 0;
        ST32(c->x[0] + 0xb8, 0);
        return 2;
    case 0x70003996e4:  // GOHMA_HEADING_SITES - str w8, [x19, #0x15ec]: heading at Link on the Field
    case 0x700039a2c4:
        c->x[8] = rt_gohma_heading(c->x[19], (uint32_t)c->x[8]);
        return 0;
    case 0x700039957c:  // GOHMA_ARRIVED_SITE - ldr q0, [x19, #0x1600]: no track end on the Field
        if (!rt_gohma_free()) return 0;
        c->x[0] = 0;
        return 2;
    case 0x70004b47c8:  // MSTALFOS_BODY_SITE - mov w8, #1: body collision off on the Field
        c->x[8] = rt_gohma_free() ? 0 : 1;
        return 1;
    case 0x70004bde08:  // MSTALFOS_JUMP_SITE - fsub s0, s0, s1: jump arc from his takeoff height on the Field
        if (rt_gohma_free()) {
            float s0;
            memcpy(&s0, &c->v[0], 4);
            s0 += *(const float *)(uintptr_t)(LD64(c->x[19] + 0x1160) + 0x28c);
            __uint128_t r = 0;
            memcpy(&r, &s0, 4);
            c->v[0] = r;
        }
        return 0;
    case 0x70001f8b30:  // MCHIEF_STORY_SITE - tbnz w8, #7: no story check on the Field
        return rt_gohma_free() ? 2 : 0;
    case 0x7000ddc5cc:  // DIG_HOLE_SITE - ldr x22, [sp, #8]: a hole was dug
        rt_dig_hole(c);
        return 0;
    case 0x7000d22d30:  // HOLE_CAP_SITES - cmp x8, #0x51 / cmp x8, #0x50: dug-hole limit
        (void)subs64(c, c->x[8], rt_hole_cap());
        return 1;
    case 0x7000d22dc0:
        (void)subs64(c, c->x[8], rt_hole_cap() - 1);
        return 1;
    case 0x700108de48:  // EMPTY_HANDLER_SITE - bl main+0x108dfc0: an empty std::function would throw (issue #41)
        if (c->x[0]) return 0;
        LOG("[event] empty handler skipped (issue #41)\n");
        return 2;  // (the dispatcher's catch: returns 0)
    }
    return 0;
}
