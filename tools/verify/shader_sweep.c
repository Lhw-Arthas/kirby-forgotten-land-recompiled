// Shader sweep: every Maxwell shader program in the player's RomFS through the Vulkan backend's shader path, offline.
// Files are read as the game ships them: loose BNSH containers (*.bfsha) and every *.cmp (a 4-byte size, then a
// zstd frame) decompressed in memory and scanned for BNSH containers. Each distinct program (vertex [+ geometry] [+ fragment],
// or compute) is translated (runtime/nvn/maxwell/translate.c, GLSL as on this GPU), compiled the way
// runtime/nvn/vulkan/shaders.c does, validated with SPIRV-Tools (Vulkan 1.3), and, unless --no-vk, made into shader
// modules and a pipeline on this PC's driver. Nothing is written into the repo: the report and the failing programs'
// GLSL go to <out>.
//
// usage: shader_sweep <romfs dir> <out dir> [--no-vk] [--structured]    (tools/verify/shader_sweep.sh builds and runs it)
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <spirv-tools/libspirv.h>
#include <shaderc/shaderc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <zstd.h>

#include "../../runtime/nvn/vulkan/shaders.h"

VkG g_vk;
void rt_fatal_trace(void) { abort(); }
void vk_device_fault_report(void) {}
const char smaa_hlsl_src[] = "";
void rt_fatal_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static uint32_t rd32(const uint8_t *d, size_t o) { uint32_t v; memcpy(&v, d + o, 4); return v; }
static uint64_t rd64(const uint8_t *d, size_t o) { uint64_t v; memcpy(&v, d + o, 8); return v; }

// ---------------------------------------------------------------- the game's shaders, deduplicated by code
// A stage's code is kept from its NVN header (graphics) or first instruction (compute) to the end of its constants.
typedef struct { uint64_t hash; uint8_t *code; uint32_t len, coff, csize; int slot; } Code;  // slot: nn::gfx stage 0..5
typedef struct { int code[6]; uint64_t key; int file; double ms; } Prog;  // code[slot]: index into codes, -1 none; ms: pipeline build

static Code *codes;
static int ncodes, capcodes;
static Prog *progs;
static int nprogs, capprogs;
static char **files;
static int nfiles;

static uint64_t fnv(const uint8_t *p, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

// hash -> index + 1 (open addressing, 1 << 20 slots: far more than the game's distinct codes and programs)
#define TAB_BITS 20
static int code_tab[1 << TAB_BITS], prog_tab[1 << TAB_BITS];
static int *tab_find(int *tab, uint64_t h, bool (*same)(int, uint64_t)) {
    size_t j = (size_t)(h * 0x9E3779B97F4A7C15ull >> (64 - TAB_BITS));
    while (tab[j] && !same(tab[j] - 1, h)) j = (j + 1) & ((1u << TAB_BITS) - 1);
    return &tab[j];
}
static bool code_same(int i, uint64_t h);
static bool prog_same(int i, uint64_t h);

static int code_add(const uint8_t *d, uint32_t len, uint32_t coff, uint32_t cs, int slot) {
    uint64_t h = fnv(d, len, 0xcbf29ce484222325ull);  // (runtime/nvn/shader_prep.c shader_code_hash: the game's records)
    int *t = tab_find(code_tab, h, code_same);
    if (*t) return *t - 1;
    *t = ncodes + 1;
    if (ncodes == capcodes) codes = realloc(codes, sizeof *codes * (size_t)(capcodes = capcodes ? capcodes * 2 : 1024));
    uint8_t *c = calloc(1, (size_t)len + 0x100);  // (zero padding past the constants, as shader_prep.c's blobs)
    memcpy(c, d, len);
    codes[ncodes] = (Code){h, c, len, coff, cs, slot};
    return ncodes++;
}

static void prog_add(const int code[6], int file) {
    uint64_t k = 0xcbf29ce484222325ull;
    for (int s = 0; s < 6; s++) k = fnv((const uint8_t *)&code[s], 4, k);
    int *t = tab_find(prog_tab, k, prog_same);
    if (*t) return;
    *t = nprogs + 1;
    if (nprogs == capprogs) progs = realloc(progs, sizeof *progs * (size_t)(capprogs = capprogs ? capprogs * 2 : 1024));
    Prog *p = &progs[nprogs++];
    memcpy(p->code, code, sizeof p->code);
    p->key = k, p->file = file;
}
static bool code_same(int i, uint64_t h) { return codes[i].hash == h; }
static bool prog_same(int i, uint64_t h) { return progs[i].key == h; }

// One BNSH container at d[b..] (the layout runtime/nvn/shader_prep.c idx_bnsh reads; every offset checked)
static int bnsh(const uint8_t *d, size_t n, size_t b, int file) {
    if (b + 0x20 > n || memcmp(d + b + 0xC, "\xff\xfe", 2)) return 0;
    size_t fsz = rd32(d, b + 0x1C), first;
    if (fsz < 0x40 || b + fsz > n) return 0;
    first = (size_t)d[b + 0x16] | (size_t)d[b + 0x17] << 8;
    if (first + 0x28 > fsz || memcmp(d + b + first, "grsc", 4)) return 0;
    size_t c = first + 0x10;
    uint32_t nvar = rd32(d, b + c + 0xC);
    uint64_t pvar = rd64(d, b + c + 0x10);
    if (pvar >= fsz || nvar > (fsz - pvar) / 0x40) return 0;
    int found = 0;
    for (uint32_t v = 0; v < nvar; v++) {
        uint64_t pbin = rd64(d, b + pvar + (uint64_t)v * 0x40 + 0x10);
        if (!pbin || pbin + 0x38 > fsz) continue;
        int code[6];
        bool any = false;
        for (int s = 0; s < 6; s++) {
            code[s] = -1;
            uint64_t pc = rd64(d, b + pbin + 8 + 8 * (uint64_t)s);
            if (!pc || pc + 0x20 > fsz) continue;
            uint64_t pctl = rd64(d, b + pc + 8), pcode = rd64(d, b + pc + 0x10);
            uint32_t csz = rd32(d, b + pc + 0x18);
            if (pctl + 0x750 > fsz || pcode >= fsz || csz > fsz - pcode) continue;
            uint32_t coff = rd32(d, b + pctl + 0x700), cs = rd32(d, b + pctl + 0x6FC);
            if (!coff || coff > csz || cs > csz - coff) continue;
            code[s] = code_add(d + b + pcode, coff + cs, coff, cs, s);
            any = true;
        }
        if (any) { prog_add(code, file); found++; }
    }
    return found;
}

static void scan(const uint8_t *d, size_t n, const char *path) {
    int file = nfiles, found = 0;
    for (size_t i = 0; i + 8 <= n; i += 8)  // (containers are aligned)
        if (!memcmp(d + i, "BNSH\0\0\0\0", 8)) found += bnsh(d, n, i, file);
    if (found) {
        files = realloc(files, sizeof *files * (size_t)(nfiles + 1));
        files[nfiles++] = strdup(path);
    }
}

static long n_read, n_cmp, n_cmp_bad;

static void scan_file(const char *path, bool cmp) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) == (size_t)n) {
        n_read++;
        if (!cmp) scan(d, (size_t)n, path);
        else {
            n_cmp++;
            size_t want = n >= 8 ? rd32(d, 0) : 0;
            uint8_t *u = want ? malloc(want) : NULL;
            size_t got = u ? ZSTD_decompress(u, want, d + 4, (size_t)n - 4) : 0;
            if (u && !ZSTD_isError(got) && got == want) scan(u, want, path);
            else n_cmp_bad++;
            free(u);
        }
    }
    free(d);
    fclose(f);
}

static void walk(const char *dir) {
    DIR *dd = opendir(dir);
    if (!dd) return;
    struct dirent *e;
    while ((e = readdir(dd))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat sb;
        if (stat(p, &sb) != 0) continue;
        size_t l = strlen(e->d_name);
        if (S_ISDIR(sb.st_mode)) walk(p);
        else if (l > 4 && !strcmp(e->d_name + l - 4, ".cmp")) scan_file(p, true);
        else if (l > 6 && !strcmp(e->d_name + l - 6, ".bfsha")) scan_file(p, false);
    }
    closedir(dd);
}

// ---------------------------------------------------------------- failure classes
// An unknown instruction is named by its opcode where known (Maxwell encodings, longest match first), else by its top
// 16 bits.
static const struct { const char *bits, *name; } OPS[] = {
    {"1110111101001", "LDS"}, {"1110111101011", "STS"}, {"1110111101000", "LDL"}, {"1110111101010", "STL"},
    {"11101100", "ATOMS"}, {"111011100", "ATOMS.CAS"}, {"111011101111", "ATOM.CAS"}, {"11101101", "ATOM"},
    {"1110101111111", "RED"}, {"11101011001", "SUST"}, {"11101011000", "SULD"}, {"1110101000", "SUATOM"},
    {"1110101011010", "SUATOM.CAS"}, {"0101000011011", "VOTE"}, {"1110111100010", "SHFL"}, {"1111000010101", "BAR"},
    {"1110111110011", "MEMBAR"}, {"0101101111010", "LEA"}, {"0011011-11010", "LEA"}, {"0100101111010", "LEA"},
    {"000110", "LEA.HI"}, {"0001100", "LEA.HI"}, {"1111000011001", "S2R"}, {"111000110000", "EXIT"},
    {"110010", "TLD4"}, {"1101111100", "TLD4S"}, {"1101111000111", "TXD"}, {"1101111000110", "TXD.B"},
    {"1111000011110", "DEPBAR"}, {"0101000011111", "FSWZADD"}, {"1110111111101", "PIXLD"},
    {"1110111110100", "AL2P"}, {"1111101111100", "OUT"}, {"1110111101100", "CCTL"}, {"1110111110000", "CCTLL"},
    {"1111000010111", "B2R"}, {"1111000011000", "R2B"}, {"111000100101", "BRX"}, {"111000100100", "JMX"},
    {"1101111101011", "TMML"}, {"1101111101100", "TMML.B"}, {"1101111101001", "TXQ"}, {"1101111101010", "TXQ.B"},
    {"1101110111", "TEX.B"}, {"110000", "TEX"}, {"1101-00", "TEXS"}, {"1101-01", "TLDS"}, {"1101-10", "TLD4S?"},
    {"11011100", "TLD"}, {"11011101", "TLD.B"}, {"100", "LD"}, {"101", "ST"}, {"1110111011010", "LDG"},
    {"1110111011011", "STG"}, {"1110111110010", "LDC"}, {"1111000011111", "SYNC"}, {"0101000010010", "R2P"},
    {"0101000011010", "PSET"}, {"0101000010001", "PSETP"}, {"0101000010000", "P2R"}, {"0101110011100", "SHFL?"},
    {"0101000011100", "VOTE.VTG"}, {"0101101101011", "ICMP"}, {"0101110011110", "SHF.R"}, {"0101101111111", "SHF.L"},
    {"0101110000110", "XMAD"}, {"1110111110101", "ALD"}, {"1110111111110", "AST"}, {"1110111111011", "IPA"},
    {"0101000010011", "CSETP"}, {"0101000010100", "CSET"}, {"0101110010111", "IMADSP?"},
    {"0101110011000", "MOV"}, {"0101101110100", "FLO"}, {"0101110000001", "BFE"}, {"0101101111110", "IADD3"},
};

static const char *op_name(uint64_t w) {
    const char *best = NULL;
    size_t bl = 0;
    for (size_t i = 0; i < sizeof OPS / sizeof OPS[0]; i++) {
        const char *b = OPS[i].bits;
        size_t l = strlen(b);
        bool ok = l > bl;
        for (size_t k = 0; ok && k < l; k++) {
            int bit = (int)((w >> (63 - k)) & 1);
            if (b[k] != '-' && b[k] - '0' != bit) ok = false;
        }
        if (ok) best = OPS[i].name, bl = l;
    }
    return best;
}

// The class of a failure message: numbers and identifiers that vary per program become '#'; an unknown instruction
// becomes its opcode
static void classify(const char *msg, char *out, size_t n) {
    unsigned long long w;
    const char *u = strstr(msg, "unknown instruction ");
    if (u && sscanf(u + 20, "%llx", &w) == 1) {
        const char *nm = op_name(w);
        if (nm) snprintf(out, n, "unknown instruction %s (%04llx)", nm, w >> 48);
        else snprintf(out, n, "unknown instruction %04llx", w >> 48);
        return;
    }
    size_t o = 0;
    for (const char *p = msg; *p && *p != '\n' && o + 2 < n; p++) {
        if ((*p >= '0' && *p <= '9') || (*p == '%' && p[1] >= '0' && p[1] <= '9')) {
            while (p[1] && ((p[1] >= '0' && p[1] <= '9') || (p[1] >= 'a' && p[1] <= 'f') || p[1] == 'x')) p++;
            out[o++] = '#';
        } else out[o++] = *p;
    }
    out[o] = 0;
}

typedef struct { char what[288]; int stage; long programs; int example; } Class;
static Class classes[512];
static int nclasses;
static FILE *g_tsv;
static const char *g_out;
static const char *STAGE_NAME[] = {"VS", "FS", "GS", "TCS", "TES", "CS"};

// slot (nn::gfx: vertex, hull, domain, geometry, pixel, compute) -> translator stage
static const int SLOT_STAGE[6] = {STAGE_VERTEX, STAGE_TESS_CTRL, STAGE_TESS_EVAL, STAGE_GEOMETRY, STAGE_FRAGMENT, STAGE_COMPUTE};

static void dump(const char *dir, const char *tag, int prog, const char *a, const char *b) {
    char p[1100];
    snprintf(p, sizeof p, "%s/%s/%05d_%s.txt", g_out, dir, prog, tag);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    if (a) fputs(a, f);
    if (b) fputs("\n-----\n", f), fputs(b, f);
    fclose(f);
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;  // (the classes, the tsv, the translator's globals)

static void fail(int prog, int stage, const char *phase, const char *msg, const char *src, const char *detail) {
    char cls[256], what[288];
    classify(msg, cls, sizeof cls);
    snprintf(what, sizeof what, "%s: %s", phase, cls);
    pthread_mutex_lock(&g_lock);
    int i = 0;
    while (i < nclasses && (classes[i].stage != stage || strcmp(classes[i].what, what))) i++;
    if (i == nclasses && nclasses < 512) {
        snprintf(classes[i].what, sizeof classes[i].what, "%s", what);
        classes[i].stage = stage, classes[i].example = prog;
        nclasses++;
        char tag[32];
        snprintf(tag, sizeof tag, "%s_%s", STAGE_NAME[stage], phase);
        dump("fail", tag, prog, src, detail);
    }
    if (i < nclasses) classes[i].programs++;
    char one[256];
    snprintf(one, sizeof one, "%s", msg);
    for (char *q = one; *q; q++) if (*q == '\n' || *q == '\t') *q = ' ';
    fprintf(g_tsv, "%d\t%s\t%s\t%s\t%s\t%s\n", prog, STAGE_NAME[stage], phase, cls, files[progs[prog].file], one);
    pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------- Vulkan (the feature set runtime/nvn/vulkan/device.c enables)
static bool g_vk_on = true;
static const char *g_dump;  // SWEEP_DUMP=<text>: the GLSL of every program from files whose path has it, in <out>/dump
static long g_messages;
static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT t,
                                                 const VkDebugUtilsMessengerCallbackDataEXT *d, void *u) {
    (void)t, (void)u, (void)d;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) g_messages++;
    return VK_FALSE;
}

static void init_device(void) {
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char *ext[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    bool validate = getenv("SWEEP_VALIDATION") != NULL;
    if (validate) ci.enabledLayerCount = ci.enabledExtensionCount = 1, ci.ppEnabledLayerNames = layers, ci.ppEnabledExtensionNames = ext;
    if (vkCreateInstance(&ci, NULL, &g_vk.inst) != VK_SUCCESS) { fprintf(stderr, "no Vulkan\n"); exit(1); }
    if (validate) {
        VkDebugUtilsMessengerCreateInfoEXT mi = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                                                 .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                                                 .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, .pfnUserCallback = on_message};
        PFN_vkCreateDebugUtilsMessengerEXT f = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerEXT m;
        if (f) f(g_vk.inst, &mi, NULL, &m);
    }
    uint32_t n = 8;
    VkPhysicalDevice pd[8];
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    g_vk.phys = pd[0];
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { g_vk.phys = pd[i]; break; }
    }
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE,
                                            .synchronization2 = VK_TRUE, .maintenance4 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE, .shaderTerminateInvocation = VK_TRUE};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = VK_TRUE, .samplerMirrorClampToEdge = VK_TRUE,
                                            .uniformAndStorageBuffer8BitAccess = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE,
                                            .scalarBlockLayout = VK_TRUE, .bufferDeviceAddress = VK_TRUE};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12,
                                            .storageBuffer16BitAccess = VK_TRUE};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11, .features = have.features};
    f.features.robustBufferAccess = VK_FALSE;
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
    if (vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev) != VK_SUCCESS) { fprintf(stderr, "vkCreateDevice failed\n"); exit(1); }
    printf("device: %s (driver %#x)%s\n", g_vk.props.deviceName, g_vk.props.driverVersion, validate ? ", validation layer" : "");
}

typedef struct { VkBindingInfo b[128]; int n, nsets; VkDescriptorSetLayout sl[4]; VkPipelineLayout pl; } Layout;

static bool make_layout(Layout *l, uint32_t *const *spv, const size_t *nw, int nmod, VkShaderStageFlags all) {
    l->n = 0, l->nsets = 0;
    for (int i = 0; i < nmod; i++) l->n += vk_reflect(spv[i], nw[i], l->b + l->n, 128 - l->n);
    for (int i = 0; i < l->n; i++) if ((int)l->b[i].set + 1 > l->nsets) l->nsets = (int)l->b[i].set + 1;
    if (l->nsets > 4) return false;
    for (int s = 0; s < l->nsets; s++) l->sl[s] = vk_set_layout(l->b, l->n, (uint32_t)s, all);
    VkPushConstantRange pr = {all, 0, 128};
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)l->nsets,
                                     .pSetLayouts = l->sl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    return vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l->pl) == VK_SUCCESS;
}

static void free_layout(Layout *l) {
    vkDestroyPipelineLayout(g_vk.dev, l->pl, NULL);
    for (int s = 0; s < l->nsets; s++) vkDestroyDescriptorSetLayout(g_vk.dev, l->sl[s], NULL);
}

static const VkDynamicState DYN[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};

// gm: the geometry stage or NULL; fm NULL: a vertex stage alone, rasterizer discard (vulkan/exec.c's vertex-only draws)
static VkResult graphics_pipeline(VkShaderModule vm, VkShaderModule gm, VkShaderModule fm, const ShaderInfo *vi, const ShaderInfo *fi,
                                  VkPipelineLayout pl) {
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb = {0, 64, VK_VERTEX_INPUT_RATE_VERTEX};
    uint32_t na = 0;
    for (int i = 0; i < 32; i++)
        if (vi->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vis = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                .vertexBindingDescriptionCount = na ? 1 : 0, .pVertexBindingDescriptions = &vb,
                                                .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    VkFormat cf[8];
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    uint32_t nc = 0;
    for (int rt = 0; fi && rt < 8; rt++) if (fi->out_mask & (1u << rt)) nc = (uint32_t)rt + 1;
    for (uint32_t rt = 0; rt < nc; rt++) cf[rt] = VK_FORMAT_R16G16B16A16_SFLOAT, cba[rt].colorWriteMask = 0xF;
    VkPipelineShaderStageCreateInfo stages[3];
    uint32_t ns = 0;
    const struct { VkShaderStageFlagBits bit; VkShaderModule m; } st[3] = {
        {VK_SHADER_STAGE_VERTEX_BIT, vm}, {VK_SHADER_STAGE_GEOMETRY_BIT, gm}, {VK_SHADER_STAGE_FRAGMENT_BIT, fm}};
    for (int i = 0; i < 3; i++)
        if (st[i].m) stages[ns++] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                                       .stage = st[i].bit, .module = st[i].m, .pName = "main"};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .rasterizerDiscardEnable = !fm, .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = nc, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = sizeof DYN / sizeof DYN[0], .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = nc,
                                        .pColorAttachmentFormats = cf, .depthAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT,
                                        .stencilAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = ns,
                                       .pStages = stages, .pVertexInputState = &vis, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pl};
    VkPipeline p;
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, &p);
    if (r == VK_SUCCESS) vkDestroyPipeline(g_vk.dev, p, NULL);
    return r;
}

static VkResult compute_pipeline(VkShaderModule m, VkPipelineLayout pl) {
    uint32_t local[3] = {64, 1, 1};
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec = {3, me, 12, local};
    VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = m, .pName = "main", .pSpecializationInfo = &spec},
                                      .layout = pl};
    VkPipeline p;
    VkResult r = vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, &p);
    if (r == VK_SUCCESS) vkDestroyPipeline(g_vk.dev, p, NULL);
    return r;
}

// ---------------------------------------------------------------- one program
static spv_context g_spv;

static ShaderInfo *translate(const Code *c, int stage, ShaderKey *k) {
    pthread_mutex_lock(&g_lock);
    maxwell_const_data = c->code + c->coff, maxwell_const_size = c->csize;
    ShaderInfo *s = maxwell_translate(c->code, c->coff, stage, k);
    maxwell_const_data = NULL, maxwell_const_size = 0;
    pthread_mutex_unlock(&g_lock);
    return s;
}

// GLSL -> SPIR-V as the backend does, then SPIRV-Tools validation. NULL: failed (recorded).
static uint32_t *compile(int prog, int stage, const char *src, size_t *nw) {
    uint32_t *spv = vk_glsl_to_spirv(src, stage, "sweep", nw);
    if (!spv) {  // (the backend logs its error only: compile once more for the message)
        shaderc_compiler_t comp = shaderc_compiler_initialize();
        shaderc_compile_options_t o = shaderc_compile_options_initialize();
        shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
        shaderc_shader_kind kind = stage == STAGE_VERTEX ? shaderc_vertex_shader : stage == STAGE_FRAGMENT ? shaderc_fragment_shader
                                 : stage == STAGE_GEOMETRY ? shaderc_geometry_shader : shaderc_compute_shader;
        shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, src, strlen(src), kind, "sweep", "main", o);
        const char *m = shaderc_result_get_error_message(r), *p = strstr(m, "error: ");
        fail(prog, stage, "glsl", p ? p + 7 : m, src, m);
        shaderc_result_release(r);
        shaderc_compile_options_release(o);
        shaderc_compiler_release(comp);
        return NULL;
    }
    spv_diagnostic diag = NULL;
    spv_const_binary_t bin = {spv, *nw};
    spv_validator_options vo = spvValidatorOptionsCreate();
    spv_result_t vr = spvValidateWithOptions(g_spv, vo, &bin, &diag);
    spvValidatorOptionsDestroy(vo);
    if (vr != SPV_SUCCESS) {
        fail(prog, stage, "spirv-val", diag && diag->error ? diag->error : "?", src, diag && diag->error ? diag->error : "?");
        spvDiagnosticDestroy(diag);
        free(spv);
        return NULL;
    }
    spvDiagnosticDestroy(diag);
    return spv;
}

static atomic_long n_ok, n_graphics, n_compute, n_other;

// m[i]: the module of stage mstage[i] (STAGE_*): compute alone, or vertex [geometry] [fragment]
static void run_vk(int prog, int stage, uint32_t *const *spv, const size_t *nw, const int *mstage, int nmod, const ShaderInfo *a,
                   const ShaderInfo *b) {
    VkShaderModule m[3] = {0}, by[6] = {0};
    Layout l;
    bool ok = false;
    VkShaderStageFlags all = 0;
    for (int i = 0; i < nmod; i++) {
        if (!(m[i] = vk_module(spv[i], nw[i]))) { fail(prog, stage, "vk", "vkCreateShaderModule failed", a->msl, b ? b->msl : NULL); goto out; }
        by[mstage[i]] = m[i];
        all |= mstage[i] == STAGE_VERTEX ? VK_SHADER_STAGE_VERTEX_BIT : mstage[i] == STAGE_FRAGMENT ? VK_SHADER_STAGE_FRAGMENT_BIT
             : mstage[i] == STAGE_GEOMETRY ? VK_SHADER_STAGE_GEOMETRY_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
    }
    if (!make_layout(&l, spv, nw, nmod, all)) {
        fail(prog, stage, "vk", "pipeline layout", a->msl, b ? b->msl : NULL);
        goto out;
    }
    long before = g_messages;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    VkResult r = by[STAGE_COMPUTE] ? compute_pipeline(by[STAGE_COMPUTE], l.pl)
                                   : graphics_pipeline(by[STAGE_VERTEX], by[STAGE_GEOMETRY], by[STAGE_FRAGMENT], a, b, l.pl);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    progs[prog].ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    free_layout(&l);
    char msg[96];
    snprintf(msg, sizeof msg, "pipeline creation: VkResult %d", r);
    if (r != VK_SUCCESS) fail(prog, stage, "vk", msg, a->msl, b ? b->msl : NULL);
    else if (g_messages != before) fail(prog, stage, "vk", "validation layer error", a->msl, b ? b->msl : NULL);
    else ok = true;
out:
    for (int i = 0; i < nmod; i++) if (m[i]) vkDestroyShaderModule(g_vk.dev, m[i], NULL);
    if (ok) n_ok++;
}

// A graphics program: vertex, geometry (gs < 0: none), fragment (fs < 0: none, the vertex stage alone with rasterizer
// discard). Each stage's key gets the outputs of the stage before (shader.h); the clip space flags stay 0 here.
static void run_graphics(int pi, int vs, int gs, int fs) {
    const Prog *p = &progs[pi];
    static const int stage[3] = {STAGE_VERTEX, STAGE_GEOMETRY, STAGE_FRAGMENT};
    static const char *const tag[3] = {"vs", "gs", "fs"};
    const int code[3] = {vs, gs, fs};
    ShaderInfo *si[3] = {0};
    uint32_t *spv[3] = {0};
    size_t nw[3] = {0};
    int ms[3], n = 0;
    uint32_t out_mask = 0;
    for (int i = 0; i < 3; i++) {
        if (code[i] < 0) continue;
        ShaderKey k = {0};
        k.glsl = 1;
        k.vs_out_mask = out_mask;
        si[i] = translate(&codes[code[i]], stage[i], &k);
        if (!si[i]->msl) { fail(pi, stage[i], "translate", si[i]->error, NULL, NULL); goto out; }
        out_mask = si[i]->out_mask;
        if (g_dump && strstr(files[p->file], g_dump)) dump("dump", tag[i], pi, files[p->file], si[i]->msl);
    }
    for (int i = 0; i < 3; i++) {
        if (!si[i]) continue;
        if (!(spv[n] = compile(pi, stage[i], si[i]->msl, &nw[n]))) goto out;
        ms[n++] = stage[i];
    }
    if (g_vk_on) run_vk(pi, fs >= 0 ? STAGE_FRAGMENT : STAGE_VERTEX, spv, nw, ms, n, si[0], si[2] ? si[2] : si[1]);
    else n_ok++;
out:
    for (int i = 0; i < 3; i++) free(spv[i]), maxwell_free(si[i]);
}

static void run_program(int pi) {
    const Prog *p = &progs[pi];
    for (int s = 1; s < 3; s++)
        if (p->code[s] >= 0) { n_other++; fail(pi, SLOT_STAGE[s], "sweep", "tessellation stage (not translated)", NULL, NULL); return; }
    if (p->code[5] >= 0) {
        n_compute++;
        ShaderKey k = {0};
        k.glsl = 1;
        ShaderInfo *si = translate(&codes[p->code[5]], STAGE_COMPUTE, &k);
        if (!si->msl) { fail(pi, STAGE_COMPUTE, "translate", si->error, NULL, NULL); maxwell_free(si); return; }
        if (g_dump && strstr(files[p->file], g_dump)) dump("dump", "cs", pi, files[p->file], si->msl);
        size_t nw = 0;
        uint32_t *spv = compile(pi, STAGE_COMPUTE, si->msl, &nw);
        const int ms = STAGE_COMPUTE;
        if (spv && g_vk_on) run_vk(pi, STAGE_COMPUTE, &spv, &nw, &ms, 1, si, NULL);
        else if (spv) n_ok++;
        free(spv);
        maxwell_free(si);
        return;
    }
    if (p->code[0] < 0) { n_other++; fail(pi, STAGE_FRAGMENT, "sweep", "program without a vertex stage", NULL, NULL); return; }
    n_graphics++;
    run_graphics(pi, p->code[0], p->code[3], p->code[4]);
}

static atomic_int next_prog;
static void *worker(void *arg) {
    (void)arg;
    for (int i; (i = atomic_fetch_add(&next_prog, 1)) < nprogs;) {
        run_program(i);
        if ((i + 1) % 1000 == 0) fprintf(stderr, "%d / %d programs, %ld ok\n", i + 1, nprogs, (long)n_ok);
    }
    return NULL;
}

static int by_ms(const void *a, const void *b) {
    double x = progs[*(const int *)a].ms, y = progs[*(const int *)b].ms;
    return x < y ? 1 : x > y ? -1 : 0;
}

static uint64_t code_hash(int c) { return c >= 0 ? codes[c].hash : 0; }

static int by_programs(const void *a, const void *b) {
    const Class *x = a, *y = b;
    return x->programs < y->programs ? 1 : x->programs > y->programs ? -1 : strcmp(x->what, y->what);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <romfs dir> <out dir> [--no-vk] [--structured]\n", argv[0]); return 2; }
    g_out = argv[2];
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--no-vk")) g_vk_on = false;
        else if (!strcmp(argv[i], "--structured")) maxwell_glsl_structured = true;
    }
    char p[1100];
    mkdir(g_out, 0755);
    snprintf(p, sizeof p, "%s/fail", g_out);
    mkdir(p, 0755);
    if ((g_dump = getenv("SWEEP_DUMP"))) snprintf(p, sizeof p, "%s/dump", g_out), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/failures.tsv", g_out);
    g_tsv = fopen(p, "wb");
    if (!g_tsv) { perror(p); return 1; }
    fprintf(g_tsv, "program\tstage\tphase\tclass\tfile\tmessage\n");
    putenv("LARECOMP_VK_NO_SPIRV_CACHE=1");
    putenv("__GL_SHADER_DISK_CACHE=0");  // (NVIDIA: every pipeline built cold, as on a first visit)
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    walk(argv[1]);
    if (getenv("SWEEP_ONLY")) {  // SWEEP_ONLY=<code hash>,...: only the programs with one of these stage codes
        int n = 0;
        for (int i = 0; i < nprogs; i++) {
            bool keep = false;
            for (int k = 0; k < 6; k++) {
                char h[20];
                snprintf(h, sizeof h, "%016llx", (unsigned long long)code_hash(progs[i].code[k]));
                keep |= progs[i].code[k] >= 0 && strstr(getenv("SWEEP_ONLY"), h);
            }
            if (keep) progs[n++] = progs[i];
        }
        nprogs = n;
    }
    long ngfx = 0, ncs = 0;
    for (int i = 0; i < ncodes; i++) codes[i].slot == 5 ? ncs++ : ngfx++;
    printf("files read %ld (%ld .cmp, %ld not decompressed), files with shaders %d, distinct programs %d, distinct stage codes %d (%ld compute)\n",
           n_read, n_cmp, n_cmp_bad, nfiles, nprogs, ncodes, ncs);
    fflush(stdout);
    g_spv = spvContextCreate(SPV_ENV_VULKAN_1_3);
    if (g_vk_on) init_device();
    vk_shaders_init();
    int jobs = getenv("SWEEP_JOBS") ? atoi(getenv("SWEEP_JOBS")) : 8;
    pthread_t th[64];
    if (jobs < 1 || jobs > 64) jobs = 8;
    for (int i = 0; i < jobs; i++) pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < jobs; i++) pthread_join(th[i], NULL);
    fclose(g_tsv);
    qsort(classes, (size_t)nclasses, sizeof *classes, by_programs);
    snprintf(p, sizeof p, "%s/report.txt", g_out);
    FILE *r = fopen(p, "wb");
    FILE *outs[2] = {stdout, r};
    for (int o = 0; o < 2; o++) {
        FILE *f = outs[o];
        if (!f) continue;
        fprintf(f, "programs %d: %ld graphics, %ld compute, %ld other; ok %ld, failed %ld (%s)\n", nprogs, (long)n_graphics, (long)n_compute,
                (long)n_other, (long)n_ok, nprogs - (long)n_ok, g_vk_on ? "translate + glsl + spirv-val + driver pipeline" : "translate + glsl + spirv-val");
        fprintf(f, "%-8s %-5s %s\n", "programs", "stage", "failure (example: fail/<program>_*.txt)");
        for (int i = 0; i < nclasses; i++)
            fprintf(f, "%8ld %-5s %s  (e.g. %05d)\n", classes[i].programs, STAGE_NAME[classes[i].stage], classes[i].what, classes[i].example);
    }
    if (r) fclose(r);
    if (g_vk_on) {  // the slowest pipeline builds (the game's first-visit stalls), by stage code hash
        int *ord = malloc(sizeof *ord * (size_t)(nprogs + 1));
        for (int i = 0; i < nprogs; i++) ord[i] = i;
        qsort(ord, (size_t)nprogs, sizeof *ord, by_ms);
        printf("slowest pipeline builds (ms, vs / cs, fs, file):\n");
        for (int i = 0; i < nprogs && i < 15; i++) {
            const Prog *q = &progs[ord[i]];
            printf("%9.0f %016llx %016llx %s\n", q->ms, (unsigned long long)code_hash(q->code[q->code[5] >= 0 ? 5 : 0]),
                   (unsigned long long)code_hash(q->code[4]), files[q->file]);
        }
        free(ord);
    }
    return 0;
}
