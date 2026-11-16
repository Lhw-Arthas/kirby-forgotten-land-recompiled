// NVN command buffers (runtime/nvn/cmdbuf.c) whose command memory the game sized for NVN's own, smaller encoding: a
// list longer than the chunk, with an out-of-memory callback that adds nothing, records whole and in order.
#include "../runtime/nvn/cmdbuf.c"
#undef NDEBUG
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

static int callbacks;
void guest_call(Cpu *c, uint64_t addr) { (void)c; (void)addr; callbacks++; }
void rt_fatal_msg(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
}
void rt_fatal_trace(void) { exit(1); }
void backend_texture_view_format(NvnTexture *tex, uint32_t format) { (void)tex; (void)format; }

static Cpu cpu;
static uint64_t call(void (*fn)(Cpu *), uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    cpu.x[0] = a0, cpu.x[1] = a1, cpu.x[2] = a2, cpu.x[3] = a3;
    fn(&cpu);
    return cpu.x[0];
}

// the alpha reference values the list holds, in order, following its jumps
static int list_values(uint64_t handle, float *out, int max) {
    int n = 0;
    for (uint64_t p = handle;;) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        if (r->op == CMD_END) return n;
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
        if (r->op == CMD_SET_ALPHA_REF && n < max) out[n++] = r->f[0];
        p += r->size;
    }
}

static uint64_t first_jump(uint64_t handle) {
    const CmdRec *r = (const CmdRec *)(uintptr_t)handle;
    while (r->op != CMD_JUMP) r = (const CmdRec *)((const uint8_t *)r + r->size);
    return r->x[0];
}

static uint64_t record_list(uint64_t cb, int count) {
    call(hle_nvnCommandBufferBeginRecording, cb, 0, 0, 0);
    for (int i = 0; i < count; i++) {
        *(float *)&cpu.v[0] = (float)i;
        call(hle_nvnCommandBufferSetAlphaRef, cb, 0, 0, 0);
    }
    return call(hle_nvnCommandBufferEndRecording, cb, 0, 0, 0);
}

int main(void) {
    static uint8_t storage[1024] __attribute__((aligned(8)));
    NvnPool pool = {.storage = (uint64_t)(uintptr_t)storage, .size = sizeof storage};
    uint64_t pool_obj = (uint64_t)(uintptr_t)&pool, cb_obj = 0;
    uint64_t cb = (uint64_t)(uintptr_t)&cb_obj, pool_ref = (uint64_t)(uintptr_t)&pool_obj;
    call(hle_nvnCommandBufferInitialize, cb, 0, 0, 0);
    call(hle_nvnCommandBufferSetMemoryCallback, cb, 0x7000516290, 0, 0);
    call(hle_nvnCommandBufferAddCommandMemory, cb, pool_ref, 0, sizeof storage);

    float v[64];
    uint64_t list = record_list(cb, 40);  // 40 records of 80 bytes: three times the chunk
    assert(callbacks > 0 && "the game's callback is asked first");
    assert(list_values(list, v, 64) == 40);
    for (int i = 0; i < 40; i++) assert(v[i] == (float)i);

    // the next frame: the game adds its chunk again; the lists in it are done with, and so is what they spilled into
    uint64_t spilled = first_jump(list);
    call(hle_nvnCommandBufferAddCommandMemory, cb, pool_ref, 0, sizeof storage);
    uint64_t list2 = record_list(cb, 40);
    assert(list_values(list2, v, 64) == 40 && v[39] == 39.0f);
    assert(first_jump(list2) == spilled && "the spill memory is reused, not grown each frame");
    printf("cmdbuf: ok\n");
    return 0;
}
