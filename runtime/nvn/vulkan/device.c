// Vulkan device, window, swapchain and presentation (counterpart of the window/present half of metal.m).
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../paths.h"
#include "../../platform.h"
#include "../../settings.h"
#include "../../ui/menu.h"
#include "vk.h"
#include "../output_scale.h"
#include "../renderer.h"
#include "../maxwell/shader.h"
#ifdef __ANDROID__
#include <adrenotools/driver.h>
#include <dlfcn.h>
#include "../../platform/android.h"
#endif
#include "../pace.h"
#include "../shader_prep.h"
#include <pthread.h>

VkG g_vk;
static SDL_Window *window;
static atomic_bool ui_ready;
static VkDebugUtilsMessengerEXT messenger;

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    static atomic_uint n;
    bool stop = sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT && getenv("LARECOMP_VK_VALIDATION_STOP");
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT &&
        (stop || atomic_fetch_add_explicit(&n, 1, memory_order_relaxed) < 500)) LOG("[vk] %s\n", d->pMessage);
    if (stop) {
        LOG("[vk] stopping at the first validation error\n");
        fflush(stderr);
        _Exit(EXIT_FAILURE);  // do not run GPU cleanup after the validation failure
    }
    return VK_FALSE;
}

#ifdef __ANDROID__
// ---- a Vulkan driver the player installed (Mesa Turnip on Adreno ...: Video settings, GpuDrivers.java) in place of the
// system's. libadrenotools loads a private copy of the system's Vulkan loader whose driver lookup is hooked to the
// installed driver; volk takes its vkGetInstanceProcAddr. SDL's own handle is on the system loader, which doesn't know
// that instance: the window surface is then made here (create_surface). Anything failing: the system driver, logged.
static bool custom_driver_on;
static char driver_probe[512];  // GpuDrivers' marker: removed once the driver has presented a few frames
static char driver_failed[256];
bool g_vk_shader_stats;  // (exec.c build_pipeline)  // the installed driver that didn't start (this time or last time), for the settings

// The settings' GPU driver rows (runtime/ui/menu.mm): whether the installed driver runs; failed: one that didn't start
bool vk_gpu_driver_custom(const char **failed) {
    if (failed) *failed = driver_failed[0] ? driver_failed : NULL;
    return custom_driver_on;
}

static bool custom_driver(void) {
    AndroidGpuDriver d;
    if (!android_gpu_driver_config(&d)) {
        const char *failed = android_gpu_driver_failed();
        if (failed) LOG("[vk] GPU driver %s failed at its last start: back to the system driver\n", failed);
        if (failed) snprintf(driver_failed, sizeof driver_failed, "%s", failed);
        return false;
    }
    void *lib = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, d.tmp_dir, d.hook_dir, d.dir, d.lib,
                                           NULL, NULL);
    PFN_vkGetInstanceProcAddr gipa = lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : NULL;
    if (!gipa) {  // (the probe stays: the next start goes back to the system driver and says so)
        LOG("[vk] GPU driver %s%s: %s (hooks in %s); using the system driver\n", d.dir, d.lib,
            lib ? "no vkGetInstanceProcAddr" : "libadrenotools could not load it", d.hook_dir);
        snprintf(driver_failed, sizeof driver_failed, "%s", d.lib);
        return false;
    }
    volkInitializeCustom(gipa);
    snprintf(driver_probe, sizeof driver_probe, "%s", d.probe);
    LOG("[vk] GPU driver: %s%s\n", d.dir, d.lib);
    return custom_driver_on = true;
}
#endif

// LARECOMP_VK_VALIDATION=1: Khronos validation layer (MSYS2 package vulkan-validation-layers)
// LARECOMP_VK_VALIDATION_STOP=1: require that layer and its callback, and exit nonzero on the first error.
static void create_instance(void) {
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    const char *ext[16];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    bool strict = getenv("LARECOMP_VK_VALIDATION_STOP") != NULL;
    bool validate = strict || getenv("LARECOMP_VK_VALIDATION") != NULL;
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
#ifdef VK_NO_PROTOTYPES
#ifdef __ANDROID__
    if (!custom_driver())
#endif
    if (volkInitialize() != VK_SUCCESS) FATAL("no Vulkan driver\n");
#endif
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "LARecomp",
                             .pEngineName = "LARecomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        if (strict) {
            LOG("[vk] cannot create an instance with required validation (%d)\n", r);
            fflush(stderr);
            _Exit(EXIT_FAILURE);
        }
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
#ifdef __ANDROID__
    if (r != VK_SUCCESS && custom_driver_on) {
        LOG("[vk] vkCreateInstance with the installed GPU driver: %d; using the system driver\n", r);
        snprintf(driver_failed, sizeof driver_failed, "The installed driver");
        custom_driver_on = false, driver_probe[0] = 0;  // (the probe stays: the next start is back on the system driver)
        if (volkInitialize() != VK_SUCCESS) FATAL("no Vulkan driver\n");
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
#endif
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.3 driver installed?)\n", r);
#ifdef VK_NO_PROTOTYPES
    volkLoadInstance(g_vk.inst);
#endif
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        VkResult result = f ? f(g_vk.inst, &mi, NULL, &messenger) : VK_ERROR_EXTENSION_NOT_PRESENT;
        if (result != VK_SUCCESS && strict) {
            LOG("[vk] required validation callback unavailable (%d)\n", result);
            fflush(stderr);
            _Exit(EXIT_FAILURE);
        }
        if (result == VK_SUCCESS) LOG("[vk] validation enabled%s\n", strict ? " (stop on error)" : "");
    }
}

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

#ifdef VK_NO_PROTOTYPES
// Drivers below Vulkan 1.3 (most phones up to Android 13): what of 1.2 / 1.3 the backend uses, as extensions. The
// backend then runs exactly its 1.3 code: the extensions' functions take the core names' place (volk's pointers,
// compat_aliases) and their structures are the same. `below`: the version from which it is core.
static const struct { const char *name; uint32_t below; } COMPAT_EXT[] = {
    {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_API_VERSION_1_3},
    {VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME, VK_API_VERSION_1_2},  // (dynamic rendering needs these two)
    {VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_API_VERSION_1_2},
    {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_API_VERSION_1_3},
    {VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, VK_API_VERSION_1_3},
    {VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, VK_API_VERSION_1_3},
    {VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME, VK_API_VERSION_1_3},
    {VK_KHR_SHADER_TERMINATE_INVOCATION_EXTENSION_NAME, VK_API_VERSION_1_3},
    {VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, VK_API_VERSION_1_2},
    {VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME, VK_API_VERSION_1_2},
    {VK_KHR_8BIT_STORAGE_EXTENSION_NAME, VK_API_VERSION_1_2},
    {VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME, VK_API_VERSION_1_2},
    {VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME, VK_API_VERSION_1_2},
};

// Whether a device below 1.3 has every extension it needs (missing ones logged)
static bool compat_ok(VkPhysicalDevice pd, uint32_t api) {
    if (api < VK_API_VERSION_1_1) return false;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    bool ok = true;
    for (size_t i = 0; i < sizeof COMPAT_EXT / sizeof *COMPAT_EXT; i++)
        if (api < COMPAT_EXT[i].below && !has_ext(ep, ne, COMPAT_EXT[i].name)) {
            LOG("[vk]   missing %s\n", COMPAT_EXT[i].name);
            ok = false;
        }
    free(ep);
    return ok;
}

static void compat_aliases(void) {
    if (g_vk.api < VK_API_VERSION_1_3) {
        vkCmdBeginRendering = vkCmdBeginRenderingKHR;
        vkCmdEndRendering = vkCmdEndRenderingKHR;
        vkCmdPipelineBarrier2 = vkCmdPipelineBarrier2KHR;
        vkQueueSubmit2 = vkQueueSubmit2KHR;
        vkCmdWriteTimestamp2 = vkCmdWriteTimestamp2KHR;
        vkCmdSetCullMode = vkCmdSetCullModeEXT;
        vkCmdSetFrontFace = vkCmdSetFrontFaceEXT;
        vkCmdSetPrimitiveTopology = vkCmdSetPrimitiveTopologyEXT;
        vkCmdSetDepthTestEnable = vkCmdSetDepthTestEnableEXT;
        vkCmdSetDepthWriteEnable = vkCmdSetDepthWriteEnableEXT;
        vkCmdSetDepthCompareOp = vkCmdSetDepthCompareOpEXT;
        vkCmdSetStencilTestEnable = vkCmdSetStencilTestEnableEXT;
        vkCmdSetStencilOp = vkCmdSetStencilOpEXT;
        vkCmdSetPrimitiveRestartEnable = vkCmdSetPrimitiveRestartEnableEXT;
        vkCmdSetDepthBiasEnable = vkCmdSetDepthBiasEnableEXT;
    }
    if (g_vk.api < VK_API_VERSION_1_2) {
        vkWaitSemaphores = vkWaitSemaphoresKHR;
        vkGetSemaphoreCounterValue = vkGetSemaphoreCounterValueKHR;
        vkSignalSemaphore = vkSignalSemaphoreKHR;
    }
}
#endif

// Discrete GPU first (LARECOMP_GPU=<index> picks one), needs Vulkan 1.3 and a queue that can present.
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = getenv("LARECOMP_GPU") ? atoi(getenv("LARECOMP_GPU")) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
#ifdef VK_NO_PROTOTYPES
        if (p.apiVersion < VK_API_VERSION_1_3 && !compat_ok(pd[i], p.apiVersion)) continue;
#else
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
#endif
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if ((int)i == want) score = 100;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) FATAL("no Vulkan 1.3 device\n");
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    g_vk.api = g_vk.props.apiVersion < VK_API_VERSION_1_3 ? VK_MAKE_API_VERSION(0, 1, VK_API_VERSION_MINOR(g_vk.props.apiVersion), 0)
                                                          : VK_API_VERSION_1_3;
    {
        VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        maxwell_frag_subgroups = (sg.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) &&
                                 (sg.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT);
        maxwell_h2f_bits = g_vk.props.vendorID == 0x10DE;  // (NVIDIA)
        if (!maxwell_frag_subgroups) LOG("[vk] no subgroup shuffles in fragment shaders: quad shuffles emulated\n");
    }
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) { g_vk.qfam = i; break; }
    }
    free(qp);
    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\n");
    LOG("[vk] using %s\n", g_vk.props.deviceName);
}

static bool g_fault_ext, g_budget_ext;
// After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), into the log.
void vk_device_fault_report(void) {
    static int once;
    if (once++ || !g_fault_ext) return;
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) return;
    VkDeviceFaultCountsEXT c = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(g_vk.dev, &c, NULL) != VK_SUCCESS) { LOG("[vk] device fault: no info\n"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (c.addressInfoCount > 16) c.addressInfoCount = 16;
    if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
    c.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend};
    if (get(g_vk.dev, &c, &info) != VK_SUCCESS && 0) return;
    LOG("[vk] device fault: %s\n", info.description);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (uint32_t i = 0; i < c.addressInfoCount; i++)
        LOG("[vk]   address %s %#llx (precision %#llx)\n", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < c.vendorInfoCount; i++)
        LOG("[vk]   vendor: %s (code %#llx data %#llx)\n", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
}

static void core_features(VkPhysicalDeviceFeatures *b, const VkPhysicalDeviceFeatures *h) {
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
    b->shaderInt64 = h->shaderInt64;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    maxwell_robust_cbuf = b->robustBufferAccess;      // (then translate.c leaves out its own index clamp)
    b->shaderClipDistance = h->shaderClipDistance;
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
    if (!h->textureCompressionBC) LOG("[vk] no BC texture formats: decoded on upload\n");
}

static void device_created(void);
#ifdef VK_NO_PROTOTYPES
// A device below 1.3 (COMPAT_EXT): the same features through the extensions' own structures, each enabled as far as
// the driver has it (what it lacks is logged: some only matter to some shaders).
static void create_device_compat(const char **ext, uint32_t n_ext, void *ext_features) {
    for (size_t i = 0; i < sizeof COMPAT_EXT / sizeof *COMPAT_EXT; i++)
        if (g_vk.api < COMPAT_EXT[i].below) ext[n_ext++] = COMPAT_EXT[i].name;
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR,
                                                      .pNext = ext_features};
    VkPhysicalDeviceSynchronization2FeaturesKHR s2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR,
                                                      .pNext = &dr};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, .pNext = &s2};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, .pNext = &eds};
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT dem = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT, .pNext = &eds2};
    VkPhysicalDeviceShaderTerminateInvocationFeaturesKHR term = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES_KHR, .pNext = &dem};
    // 1.2: its feature structure; 1.1: the extensions'
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &term};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceTimelineSemaphoreFeatures tl = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
                                                    .pNext = &term};
    VkPhysicalDeviceShaderFloat16Int8Features i8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
                                                    .pNext = &tl};
    VkPhysicalDevice8BitStorageFeatures b8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, .pNext = &i8};
    VkPhysicalDeviceScalarBlockLayoutFeatures sbl = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,
                                                     .pNext = &b8};
    VkPhysicalDevice16BitStorageFeatures b16 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, .pNext = &sbl};
    bool v12 = g_vk.api >= VK_API_VERSION_1_2;
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = v12 ? (void *)&f11 : (void *)&b16};
    // what the driver has: everything wanted is enabled as far as it goes (the extensions' own features below
    // ext_features are asked for as they are, as on 1.3: hide them from the query)
    dr.pNext = NULL;
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &f);
    dr.pNext = ext_features;
    VkPhysicalDeviceFeatures have = f.features;
    memset(&f.features, 0, sizeof f.features);
    core_features(&f.features, &have);
#define WANT(cond, what) do { if (!(cond)) LOG("[vk] driver lacks %s\n", what); } while (0)
    WANT(dr.dynamicRendering, "dynamicRendering");
    WANT(s2.synchronization2, "synchronization2");
    WANT(eds.extendedDynamicState, "extendedDynamicState");
    WANT(eds2.extendedDynamicState2, "extendedDynamicState2");
    WANT(dem.shaderDemoteToHelperInvocation, "shaderDemoteToHelperInvocation");
    WANT(term.shaderTerminateInvocation, "shaderTerminateInvocation");
    eds2.extendedDynamicState2LogicOp = eds2.extendedDynamicState2PatchControlPoints = VK_FALSE;
    if (v12) {
        WANT(f12.timelineSemaphore, "timelineSemaphore");
        WANT(f12.shaderInt8, "shaderInt8");
        WANT(f12.storageBuffer8BitAccess, "storageBuffer8BitAccess");
        WANT(f12.scalarBlockLayout, "scalarBlockLayout");
        VkPhysicalDeviceVulkan12Features want12 = {.timelineSemaphore = f12.timelineSemaphore, .shaderInt8 = f12.shaderInt8,
            .samplerMirrorClampToEdge = f12.samplerMirrorClampToEdge,
            .uniformAndStorageBuffer8BitAccess = f12.uniformAndStorageBuffer8BitAccess,
            .storageBuffer8BitAccess = f12.storageBuffer8BitAccess, .scalarBlockLayout = f12.scalarBlockLayout};
        want12.sType = f12.sType, want12.pNext = f12.pNext;
        f12 = want12;
        VkPhysicalDeviceVulkan11Features want11 = {.sType = f11.sType, .pNext = f11.pNext,
                                                   .storageBuffer16BitAccess = f11.storageBuffer16BitAccess};
        f11 = want11;
    } else {
        WANT(tl.timelineSemaphore, "timelineSemaphore");
        WANT(i8.shaderInt8, "shaderInt8");
        WANT(b8.storageBuffer8BitAccess, "storageBuffer8BitAccess");
        WANT(sbl.scalarBlockLayout, "scalarBlockLayout");
        i8.shaderFloat16 = VK_FALSE;
        b8.storagePushConstant8 = VK_FALSE;
        b16.uniformAndStorageBuffer16BitAccess = b16.storagePushConstant16 = b16.storageInputOutput16 = VK_FALSE;
    }
#undef WANT
    if (!(tl.timelineSemaphore || (v12 && f12.timelineSemaphore)) || !dr.dynamicRendering || !s2.synchronization2 ||
        !eds.extendedDynamicState || !eds2.extendedDynamicState2)
        FATAL("this GPU driver lacks Vulkan features the game needs (see the lines above)\n");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    LOG("[vk] Vulkan %u.%u device with %u extensions for 1.3\n", VK_API_VERSION_MAJOR(g_vk.api), VK_API_VERSION_MINOR(g_vk.api), n_ext);
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
    device_created();
}
#endif

// Each memory heap: its size, the driver's budget for this process (smaller when other programs hold that memory),
// this process's use and the part LAS Recomp allocated through VMA. Logged at start and when an allocation fails
// (issue #45: out of device memory on a 6 GB GTX 1060 - our own use, or other programs'?).
void vk_log_memory(const char *why) {
    if (!g_vk.vma) return;
    const VkPhysicalDeviceMemoryProperties *mp;
    vmaGetMemoryProperties(g_vk.vma, &mp);
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(g_vk.vma, b);
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++)
        LOG("[vk] memory (%s) heap %u %s: %llu MB, budget %llu MB, in use %llu MB, ours %llu MB in %u allocations\n", why, i,
            mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "device-local" : "system",
            (unsigned long long)(mp->memoryHeaps[i].size >> 20), (unsigned long long)(b[i].budget >> 20),
            (unsigned long long)(b[i].usage >> 20), (unsigned long long)(b[i].statistics.allocationBytes >> 20),
            b[i].statistics.allocationCount);
}

// ---- pipeline cache file
#define PCACHE_FILE "cache/vk_pipelines.bin"
#define PCACHE_TAG "cache/vk_pipelines.bin.tag"
static size_t pcache_saved;  // bytes of the last save (or load)
void vk_pipeline_cache_save(void);

static void pipeline_cache_tag(char *out, size_t n) {
    char gpu[400], uuid[2 * VK_UUID_SIZE + 1];
    for (int i = 0; i < VK_UUID_SIZE; i++) snprintf(uuid + 2 * i, 3, "%02x", g_vk.props.pipelineCacheUUID[i]);
    snprintf(gpu, sizeof gpu, "%s, vendor %x device %x, driver %u, cache %s", g_vk.props.deviceName, g_vk.props.vendorID,
             g_vk.props.deviceID, g_vk.props.driverVersion, uuid);
    shader_cache_tag(out, n, gpu);
}

static void *pipeline_cache_saver(void *a) {  // every 3 minutes, when it grew
    (void)a;
    for (;;) {
        SDL_Delay(180000);
        vk_pipeline_cache_save();
    }
    return NULL;
}

static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[32];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    maxwell_gmem_mirror = !g_vk.host_import;  // (exec.c mirror_table)
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    g_fault_ext = fault;
    g_budget_ext = has_ext(ep, ne, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);  // (real per-heap budgets: vk_log_memory)
    if (g_budget_ext) ext[n_ext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    // LARECOMP_VK_SHADER_STATS=1: the driver's statistics of each pipeline's shaders (registers, instructions, spills)
    g_vk_shader_stats = getenv("LARECOMP_VK_SHADER_STATS") && has_ext(ep, ne, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    if (g_vk_shader_stats) ext[n_ext++] = VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME;
#ifdef __linux__
    // the guest heap as driver memory mapped at HEAP_BASE (exec.c backend_guest_heap_prepare)
    g_vk.dmabuf = has_ext(ep, ne, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
                  has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    if (g_vk.dmabuf) {
        ext[n_ext++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    }
#endif
    free(ep);

    // features the backend relies on (all present on current desktop drivers)
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
#ifdef VK_NO_PROTOTYPES
    if (g_vk.api < VK_API_VERSION_1_3) {
        VkPhysicalDeviceFaultFeaturesEXT ffc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,
                                                .deviceFault = VK_TRUE,
                                                .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
        create_device_compat(ext, n_ext, fault ? (void *)&ffc : ffc.pNext);
        return;
    }
#endif
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE, .shaderTerminateInvocation = VK_TRUE,
                                            .maintenance4 = VK_TRUE,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = VK_TRUE, .samplerMirrorClampToEdge = VK_TRUE,
                                            .uniformAndStorageBuffer8BitAccess = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE,
                                            .scalarBlockLayout = VK_TRUE, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = VK_TRUE, .pNext = &f12};
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                           .pNext = &f11};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fault ? (void *)&ff : (void *)&f11};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pef = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, .pipelineExecutableInfo = VK_TRUE,
        .pNext = f.pNext};
    if (g_vk_shader_stats) f.pNext = &pef;
    core_features(&f.features, &have.features);
#ifdef VK_NO_PROTOTYPES
    {   // phones: one feature the driver lacks fails the whole device - enable what it has, log what it lacks
        VkPhysicalDeviceVulkan13Features h13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features h12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &h13};
        VkPhysicalDeviceVulkan11Features h11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &h12};
        VkPhysicalDeviceFeatures2 hf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &h11};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &hf);
#define HAVE(w, h, x) if (w.x && !h.x) { LOG("[vk] driver lacks %s\n", #x); w.x = VK_FALSE; }
        if (!h13.dynamicRendering || !h13.synchronization2 || !h12.timelineSemaphore)
            FATAL("this GPU driver lacks Vulkan features the game needs (dynamic rendering, synchronization2, timeline "
                  "semaphores)\n");
        HAVE(f13, h13, shaderDemoteToHelperInvocation)
        HAVE(f13, h13, shaderTerminateInvocation) HAVE(f13, h13, maintenance4) HAVE(f12, h12, timelineSemaphore)
        HAVE(f12, h12, shaderInt8) HAVE(f12, h12, samplerMirrorClampToEdge) HAVE(f12, h12, uniformAndStorageBuffer8BitAccess)
        HAVE(f12, h12, storageBuffer8BitAccess) HAVE(f12, h12, scalarBlockLayout) HAVE(f11, h11, storageBuffer16BitAccess)
#undef HAVE
        // guest shaders fetch texels and images with coordinates nobody checked: out of the image, a phone GPU may
        // fault (device lost) where desktop ones just return zero
        f13.robustImageAccess = h13.robustImageAccess;
#ifdef __ANDROID__
        {   // native fp16 for the guest's half-precision instructions (translate.c maxwell_half_native)
            const char *e = getenv("LARECOMP_HALF_NATIVE");
            if (h12.shaderFloat16 && (!e || atoi(e))) f12.shaderFloat16 = VK_TRUE, maxwell_half_native = 1;
            const char *sc = getenv("LARECOMP_VK_SCALAR_CBUF");  // run-time indexed cbufs as words (translate.c)
            maxwell_scalar_cbuf = h12.scalarBlockLayout && (!sc || atoi(sc));
        }
#endif
        if (!h13.robustImageAccess) LOG("[vk] driver lacks robustImageAccess\n");
    }
#endif
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
    device_created();
}

// Do this GPU's min / max keep the non-NaN operand, and its clamp make NaN 0 - what Maxwell's FMNMX and .SAT do? Then
// the guest shaders' saturate / fmin / fmax go without the isnan selects around them (maxwell_ieee_minmax; shader.h,
// translate.c). A one-thread compute dispatch over a NaN read from a buffer (nothing the compiler can fold), its
// result words compared with the Switch's. LARECOMP_VK_IEEE_MINMAX=0/1 decides instead of the probe.
static void probe_ieee_minmax(void) {
    const char *e = getenv("LARECOMP_VK_IEEE_MINMAX");
    if (e) { maxwell_ieee_minmax = atoi(e) != 0; return; }
    char src[1200];
    snprintf(src, sizeof src,
             "#version 460\n%s"
             "layout(local_size_x = 1) in;\n"
             "layout(std430, set = 0, binding = 0) buffer B { uint w[12]; };\n"
             "void main() { float n = uintBitsToFloat(w[0]), one = uintBitsToFloat(w[1]);\n"
             "  w[2] = floatBitsToUint(max(n, one)); w[3] = floatBitsToUint(min(n, one));\n"
             "  w[4] = floatBitsToUint(max(one, n)); w[5] = floatBitsToUint(min(one, n));\n"
             "  w[6] = floatBitsToUint(clamp(n, 0.0f, 1.0f)); w[7] = floatBitsToUint(clamp(fma(n, one, one), 0.0f, 1.0f));\n"
             "  w[8] = floatBitsToUint(clamp(n * one, 0.0f, 1.0f));%s }\n",
             maxwell_half_native ? "#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require\n" : "",
             maxwell_half_native ? " w[9] = packFloat2x16(clamp(f16vec2(float16_t(n), float16_t(one) + float16_t(n)), f16vec2(0.0hf), f16vec2(1.0hf)));"
                                 : "");
    VkShaderModule sm = vk_compile_glsl(src, STAGE_COMPUTE, "ieee_minmax.comp");
    if (!sm) { LOG("[vk] NaN probe: no shader (isnan selects kept)\n"); return; }
    VkDescriptorSetLayout dl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline p = VK_NULL_HANDLE;
    VkDescriptorPool dp = VK_NULL_HANDLE;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation mem = NULL;
    VmaAllocationInfo info = {0};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
                                      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
    bool ok = vkCreateDescriptorSetLayout(g_vk.dev, &dli, NULL, &dl) == VK_SUCCESS;
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dl};
    ok = ok && vkCreatePipelineLayout(g_vk.dev, &pli, NULL, &pl) == VK_SUCCESS;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"}};
    ok = ok && vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &p) == VK_SUCCESS;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    ok = ok && vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info) == VK_SUCCESS && info.pMappedData;
    VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    ok = ok && vkCreateDescriptorPool(g_vk.dev, &dpi, NULL, &dp) == VK_SUCCESS;
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
                                       .descriptorSetCount = 1, .pSetLayouts = &dl};
    ok = ok && vkAllocateDescriptorSets(g_vk.dev, &dai, &ds) == VK_SUCCESS;
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    ok = ok && vkAllocateCommandBuffers(g_vk.dev, &cai, &cb) == VK_SUCCESS;
    uint32_t *w = info.pMappedData;
    if (ok) {
        memset(w, 0xEE, 64);
        w[0] = 0x7fc00000u, w[1] = 0x3f800000u;  // NaN, 1.0
        vmaFlushAllocation(g_vk.vma, mem, 0, 64);
        VkDescriptorBufferInfo dbi = {.buffer = buf, .range = 64};
        VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
        vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
        VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        ok = vkBeginCommandBuffer(cb, &cbi) == VK_SUCCESS;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
        vkCmdDispatch(cb, 1, 1, 1);
        ok = ok && vkEndCommandBuffer(cb) == VK_SUCCESS;
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        ok = ok && vkQueueSubmit(g_vk.queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(g_vk.queue) == VK_SUCCESS;
        vmaInvalidateAllocation(g_vk.vma, mem, 0, 64);
    }
    if (ok) {
        bool good = w[2] == 0x3f800000u && w[3] == 0x3f800000u && w[4] == 0x3f800000u && w[5] == 0x3f800000u && !w[6] && !w[7] &&
                    !w[8] && (!maxwell_half_native || !w[9]);
        maxwell_ieee_minmax = good;
        LOG("[vk] NaN through max min max min clamp clamp(fma) clamp(mul) clamp16: %08x %08x %08x %08x %08x %08x %08x %08x: %s\n",
            w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], good ? "as Maxwell: plain saturate / fmin / fmax" : "not Maxwell's: isnan selects kept");
    } else {
        LOG("[vk] NaN probe failed (isnan selects kept)\n");
    }
    if (cb) vkFreeCommandBuffers(g_vk.dev, g_vk.pool, 1, &cb);
    if (dp) vkDestroyDescriptorPool(g_vk.dev, dp, NULL);
    if (buf) vmaDestroyBuffer(g_vk.vma, buf, mem);
    if (p) vkDestroyPipeline(g_vk.dev, p, NULL);
    if (pl) vkDestroyPipelineLayout(g_vk.dev, pl, NULL);
    if (dl) vkDestroyDescriptorSetLayout(g_vk.dev, dl, NULL);
    vkDestroyShaderModule(g_vk.dev, sm, NULL);
}

// What follows the device's creation, whichever way it was made
static void device_created(void) {
#ifdef VK_NO_PROTOTYPES
    volkLoadDevice(g_vk.dev);
    compat_aliases();
#endif
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = g_vk.api,
                                 .flags = g_budget_ext ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0};
#ifdef VK_NO_PROTOTYPES
    VmaVulkanFunctions vf = {.vkGetInstanceProcAddr = vkGetInstanceProcAddr, .vkGetDeviceProcAddr = vkGetDeviceProcAddr};
    ai.pVulkanFunctions = &vf;
#endif
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));
    vk_log_memory("start");

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));
    probe_ieee_minmax();  // (before any guest shader is translated: vk_headstart_begin below)

    // pipeline cache: cache/vk_pipelines.bin (written by vk_pipeline_cache_save), used only with its tag
    // (vk_pipelines.bin.tag: this build, the translator, the GPU and its driver - another one starts it empty)
    char tag[512];
    pipeline_cache_tag(tag, sizeof tag);
    size_t sz = 0;
    void *data = NULL;
    FILE *fp = shader_cache_tag_ok(PCACHE_TAG, tag) ? fopen(PCACHE_FILE, "rb") : NULL;
    if (!fp && !access(PCACHE_FILE, F_OK)) LOG("[vk] pipeline cache from another build / GPU / driver: starting a new one\n");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        sz = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = malloc(sz);
        if (fread(data, 1, sz, fp) != sz) sz = 0;
        fclose(fp);
    }
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz,
                                    .pInitialData = data};
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        pc.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    free(data);
    extern void vk_headstart_begin(void);
    vk_headstart_begin();  // (the shaders and pipelines seen before, built in the background from now: exec.c)
    if (pc.initialDataSize) LOG("[vk] pipeline cache: loaded %zu bytes\n", pc.initialDataSize);
    pcache_saved = pc.initialDataSize;
    pthread_t t;  // (kept every few minutes while it grows, not only at a clean exit)
    pthread_create(&t, NULL, pipeline_cache_saver, NULL);
    pthread_detach(t);
}

// Saved atomically (temp file + rename: a crash mid-write leaves the previous one), with its tag; one save at a time
void vk_pipeline_cache_save(void) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    size_t sz = 0;
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) == VK_SUCCESS && sz && sz != pcache_saved) {
        void *data = malloc(sz);
        if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data) == VK_SUCCESS && shader_cache_write(PCACHE_FILE, data, sz)) {
            char tag[512];
            pipeline_cache_tag(tag, sizeof tag);
            if (!shader_cache_tag_ok(PCACHE_TAG, tag)) shader_cache_tag_write(PCACHE_TAG, tag);
            pcache_saved = sz;
            LOG("[vk] pipeline cache: saved %zu bytes\n", sz);
        }
        free(data);
    }
    pthread_mutex_unlock(&lock);
}

// ================================================================== swapchain
static VkPresentModeKHR present_mode(void) {
    // MAILBOX by default: the game paces itself (vi.c's emulated 60 Hz vsync), and with FIFO every submission
    // queued behind the one waiting for a window image, so the game's fence waits followed the presentation engine:
    // a throttled / captured / busy compositor slowed the whole game (4-21 fps with the GPU mostly idle; mailbox:
    // 60). No tearing. LARECOMP_VK_PRESENT=fifo|immediate|mailbox to choose.
    // Frame generation (framegen.c): FIFO, which shows the in-between frame, presented right after the real one, a
    // refresh later.
    extern bool fg_enabled(void);
    const char *e = getenv("LARECOMP_VK_PRESENT");
    if (!e && fg_enabled()) e = "fifo";
    VkPresentModeKHR want = !e || !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static bool g_sc_turned, g_fifo_present;  // (FIFO: a second picture per game frame shows a refresh later)
static atomic_bool g_background, g_surface_stale;  // (the app in the background: below)
static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VkResult capr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps);
    if (capr == VK_ERROR_SURFACE_LOST_KHR) { atomic_store(&g_surface_stale, true); return; }  // (Android: window gone)
    VK_CHECK(capr);
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    VkSurfaceTransformFlagBitsKHR xf = caps.currentTransform;
#ifdef __ANDROID__
    // Phones are portrait underneath: in landscape the surface reports a 90 / 270 degree transform. The images are
    // made in the window's own (landscape) shape, the extent it reports, and the system turns them (identity)
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) xf = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
#endif
    g_sc_turned = xf != caps.currentTransform;  // (then presents say "suboptimal": that's this, not a new size)
    if (!ext.width || !ext.height) return;  // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote (see the raw views in the present pass): a UNORM swapchain like Metal's
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainKHR old = g_vk.swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = xf,
        .compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                          : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        .presentMode = present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old};
    VkResult cr = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain);
    g_fifo_present = ci.presentMode == VK_PRESENT_MODE_FIFO_KHR;
    if (cr == VK_ERROR_SURFACE_LOST_KHR || cr == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {  // (Android: the app went away)
        LOG("[vk] swapchain: surface lost (%d)\n", cr);
        g_vk.swapchain = VK_NULL_HANDLE;
        if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
        for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
        g_vk.sc_count = 0;
        atomic_store(&g_surface_stale, true);  // (a new surface is made on the next frame)
        return;
    }
    VK_CHECK(cr);
    static const char *const modes[] = {"immediate", "mailbox", "fifo", "fifo-relaxed"};
    LOG("[vk] swapchain %ux%u, %u images, %s\n", ext.width, ext.height, count,
        ci.presentMode < 4 ? modes[ci.presentMode] : "other present mode");
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    {
        int pw, ph;
        SDL_GetWindowSize(window, &pw, &ph);
        LOG("[vk] swapchain %ux%u (surface %ux%u, transform %#x; window %dx%d px, %dx%d points)\n", ext.width, ext.height,
            caps.currentExtent.width, caps.currentExtent.height, caps.currentTransform, w, h, pw, ph);
    }
    g_vk.sc_count = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
}

static void recreate_swapchain(void) {
    vkDeviceWaitIdle(g_vk.dev);
    create_swapchain();
}

// ---- the app in the background (Android): the window's surface goes away. Nothing is drawn meanwhile, and the game
// waits in its next present (it pauses where it is: no CPU, no battery); back in front, a new surface and swapchain.
static bool SDLCALL lifecycle_watch(void *user, SDL_Event *e) {
    (void)user;
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND || e->type == SDL_EVENT_DID_ENTER_BACKGROUND) {
        if (!atomic_exchange(&g_background, true)) {
            LOG("[vk] in the background: game paused\n");
            vk_pipeline_cache_save();  // (phones end apps in the background without exit(): the exit handler never runs)
        }
    } else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND && atomic_load(&g_background)) {
        LOG("[vk] in front again\n");
        atomic_store(&g_surface_stale, true);
        atomic_store(&g_background, false);
    }
    return true;
}

// The window's Vulkan surface (false: SDL_GetError says why)
static bool create_surface(void) {
#ifdef __ANDROID__
    if (custom_driver_on) {  // (as SDL's Android_Vulkan_CreateSurface, through the installed driver's loader)
        struct ANativeWindow *nw = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
        if (!nw) return SDL_SetError("no native window (the app is in the background)");
        VkAndroidSurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = nw};
        VkResult r = vkCreateAndroidSurfaceKHR(g_vk.inst, &si, NULL, &g_vk.surface);
        if (r != VK_SUCCESS) return SDL_SetError("vkCreateAndroidSurfaceKHR: %d", r);
        return true;
    }
#endif
    return SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface);
}

static void new_surface(void) {
    vkDeviceWaitIdle(g_vk.dev);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    g_vk.sc_count = 0;
    if (g_vk.swapchain) vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
    g_vk.swapchain = VK_NULL_HANDLE;
    if (g_vk.surface) vkDestroySurfaceKHR(g_vk.inst, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    if (!create_surface()) {
        LOG("[vk] new surface: %s\n", SDL_GetError());
        atomic_store(&g_surface_stale, true);  // (the window isn't back yet: next frame)
        return;
    }
    create_swapchain();
}

// Called by the game's presenting thread: blocks while the app is in the background.
static void wait_foreground(void) {
    while (atomic_load(&g_background)) SDL_Delay(50);
}

// Time this thread spends waiting here - for a frame slot's GPU work, a window image, the presentation engine -
// counts as idle for the frame pacer (pace.c), like the game's own waits: in FIFO (exclusive fullscreen
// on Windows offers no mailbox) acquire / present block until the display's refresh, and counted as busy that made
// every frame look ~16.6 ms: the game's 30 fps mode outdoors on a fast GPU (issue #44). GPU-bound frames still show
// in the game's GPU counters.
extern __thread uint64_t g_blocked_ns;  // kernel.c
#define WAITING(x) do { uint64_t w0_ = plat_now_ns(); x; g_blocked_ns += plat_now_ns() - w0_; } while (0)

// ================================================================== frames (start screen and game)
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[8];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

typedef struct { uint32_t image; VkSemaphore acquired; bool ok; } Frame;
static Frame cur_frame;

// Acquire the next window image and begin rendering into it (cleared to `clear`). Returns false when there is
// nothing to draw into (minimised window).
bool vk_frame_begin(const float clear[4]) {
    cur_frame.ok = false;
    if (atomic_load(&g_background)) return false;
    if (atomic_exchange(&g_surface_stale, false)) new_surface();
    if (!g_vk.surface || atomic_load(&g_surface_stale)) return false;
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (!w || !h) return false;  // minimised
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height) recreate_swapchain();
    if (!g_vk.swapchain) return false;
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] frame begin %dx%d (swapchain %ux%u)\n", w, h, g_vk.sc_extent.width, g_vk.sc_extent.height);
    slot = (slot + 1) % FRAMES;
    // Phones: the window image is waited for on the CPU (a fence), not by the GPU (a semaphore). Turnip waits for the
    // compositor's release inside the submission that clears the image, and now and then that wait didn't end: the
    // kernel's hang detection, device lost (breadcrumbs: the GPU done with the game's frame, stuck before the window
    // image's first pass). With MAILBOX the image is ready at once. LARECOMP_VK_ACQUIRE_FENCE=0/1.
    static int by_fence = -1;
    if (by_fence < 0) {
        const char *e = getenv("LARECOMP_VK_ACQUIRE_FENCE");
#ifdef __ANDROID__
        by_fence = !e || atoi(e) != 0;
#else
        by_fence = e && atoi(e) != 0;
#endif
    }
    static VkFence acquire_fence;
    if (by_fence && !acquire_fence) {
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(g_vk.dev, &fi, NULL, &acquire_fence));
    }
    if (!by_fence && !acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    vk_wait_seq(slot_seq[slot]);  // (its time counts as blocked by itself)
    VkSemaphore asem = by_fence ? VK_NULL_HANDLE : acquire_sem[slot];
    VkFence afence = by_fence ? acquire_fence : VK_NULL_HANDLE;
    VkResult r;
    WAITING(r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, asem, afence, &cur_frame.image));
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        WAITING(r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, asem, afence, &cur_frame.image));
    }
    if (r == VK_ERROR_SURFACE_LOST_KHR) atomic_store(&g_surface_stale, true);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("[vk] acquire failed: %d\n", r); return false; }
    if (by_fence) {
        WAITING(vkWaitForFences(g_vk.dev, 1, &acquire_fence, VK_TRUE, UINT64_MAX));
        vkResetFences(g_vk.dev, 1, &acquire_fence);
    }
    cur_frame.acquired = asem;
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    extern void vk_bc_mark(VkCommandBuffer cb, const char *label);
    vk_bc_mark(cb, "window image: begin");
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color.float32 = {clear[0], clear[1], clear[2], clear[3]}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    cur_frame.ok = true;
    return true;
}

// Passes of their own between vk_frame_begin and vk_frame_end (exec.c post passes): end the window image's
// rendering, and begin it again keeping what was drawn.
void vk_frame_suspend(void) {
    if (!cur_frame.ok) return;
    vkCmdEndRendering(vk_cmd());
    extern void vk_bc_mark(VkCommandBuffer cb, const char *label);
    vk_bc_mark(vk_cmd(), "window image: cleared");
}
void vk_frame_resume(void) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

VkExtent2D vk_frame_extent(void) { return g_vk.sc_extent; }
void *vk_frame_cmd(void) { return (void *)vk_cmd(); }

static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    *out = vk_readback((uint64_t)w * h * 4);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->buf, 1, &rg);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

static bool g_fg_copy;  // vk_frame_end: copy the window picture for frame generation (backend_present_gpu)
static double g_shown_fps;
double vk_shown_fps(void) { return g_shown_fps; }
// End rendering into the window image and present it. shot_path: also save the window image as PNG.
void vk_frame_end(const char *shot_path) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    vkCmdEndRendering(cb);
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);
    if (g_fg_copy) {  // (frame generation: the real window picture, for the in-between frame)
        extern void fg_copy_window(VkCommandBuffer cb, VkImage sc, VkFormat fmt, uint32_t w, uint32_t h);
        fg_copy_window(cb, g_vk.sc_images[cur_frame.image], g_vk.sc_format, g_vk.sc_extent.width, g_vk.sc_extent.height);
        g_fg_copy = false;
    }
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    uint32_t img = cur_frame.image;
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done_sem[img],
                           .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
    VkResult r;
    WAITING(r = vkQueuePresentKHR(g_vk.queue, &pi));
    if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && !g_sc_turned)) recreate_swapchain();
    else if (r == VK_ERROR_SURFACE_LOST_KHR) atomic_store(&g_surface_stale, true);
#ifdef __ANDROID__
    static int presented;
    if (driver_probe[0] && r >= 0 && ++presented == 60) unlink(driver_probe), driver_probe[0] = 0;  // (the driver works)
#endif
    cur_frame.ok = false;
    {   // pictures shown a second (generated ones too): the menu's FPS counter
        static uint64_t t0, n;
        uint64_t now = SDL_GetTicksNS();
        n++;
        if (!t0) t0 = now;
        if (now - t0 >= 500000000ull) g_shown_fps = n * 1e9 / (double)(now - t0), n = 0, t0 = now;
    }
    if (shot_path) {
        vk_wait_seq(seq);
        uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
        uint8_t *px = shot.ptr;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint64_t i = 0; bgr && i < (uint64_t)w * h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
        vk_save_png(shot_path, px, w, h);
    }
}

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
#ifdef __ANDROID__
    // (a relative path would name an APK asset: under the app's files folder instead, its folders made)
    char full[1024];
    const char *dir = SDL_GetAndroidExternalStoragePath();
    if (path[0] != '/' && dir) {
        snprintf(full, sizeof full, "%s/%s", dir, path);
        for (char *q = full + strlen(dir) + 1; (q = strchr(q, '/')); q++) { *q = 0; mkdir(full, 0755); *q = '/'; }
        path = full;
    }
#endif
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
    if (!s || !SDL_SavePNG(s, path)) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    SDL_DestroySurface(s);
}

// ================================================================== game presentation (metal.m backend_present)
// Draws the game's image into the window with the widescreen crop, then the overlay menu.
extern void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]);
void renderer_submit(void);
void renderer_set_scale(float sx, float sy);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);

static uint64_t frames, total_frames, g_frame_counter;
uint64_t renderer_present_count(void) { return total_frames; }
uint64_t vk_frame_counter(void) { return g_frame_counter; }
static float g_shot_crop[4] = {0, 0, 1, 1};
static double last_fps_time;
static NvnTexture *g_last_presented;

const float *vk_shot_crop(void) { return g_shot_crop; }

void backend_present(NvnTexture *t);
void backend_represent(void) {
    if (g_last_presented) backend_present(g_last_presented);
    else SDL_Delay(16);
}

// The game's side of a frame: its pacing and the per-frame hooks that act on the game (input, warps, tests), on the
// game's thread; the frame number counts the game's presents (the render thread's may be one behind).
static void present_game(void) {
    static uint64_t n;
    n++;
    if (n % 120 == 1) plat_thread_core(PLAT_CORE_MAIN);  // (again now and then: Android resets it when the app's
                                                          // cpuset changes, e.g. on reaching the foreground)
    pace_frame();  // (pace.c: the game's 30 fps mode when frames don't fit 60; exec.c vk_pace_adjust)
    extern void input_frame(uint64_t frame);
    input_frame(n);
    static long slow_ms = -1;
    if (slow_ms < 0) slow_ms = getenv("LARECOMP_SLOW_FRAME_MS") ? atol(getenv("LARECOMP_SLOW_FRAME_MS")) : 0;
    static long slow_until = -1;  // LARECOMP_SLOW_FRAME_UNTIL=<frame>: the stall ends there (frame pacing recovery tests)
    if (slow_until < 0) slow_until = getenv("LARECOMP_SLOW_FRAME_UNTIL") ? atol(getenv("LARECOMP_SLOW_FRAME_UNTIL")) : 0;
    if (slow_ms > 0 && n > 2600 && (!slow_until || (long)n < slow_until)) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(n);
    extern void warp_frame(uint64_t frame);
    warp_frame(n);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (LARECOMP_REBOOT_AT)
    reboot_test_frame(n);
    extern void save_editor_frame(uint64_t frame);
    save_editor_frame(n);
    static long pgo_every = -1;  // LARECOMP_PGO_DUMP_EVERY=<frames>: an instrumented game library's profile (build/pgo/)
    if (pgo_every < 0) pgo_every = getenv("LARECOMP_PGO_DUMP_EVERY") ? atol(getenv("LARECOMP_PGO_DUMP_EVERY")) : 0;
    if (pgo_every > 0 && n % (uint64_t)pgo_every == 0) {
        extern bool gamelib_pgo_dump(const char *path);
        mkdir("build", 0755);
        mkdir("build/pgo", 0755);
        if (gamelib_pgo_dump("build/pgo/lasgame.profraw")) LOG("[pgo] profile written (frame %llu)\n", (unsigned long long)n);
    }
}

static bool menu_open(void) { extern bool menu_is_open(void); return menu_is_open(); }
static uint32_t game_interval(void) { extern uint32_t g_game_interval; return g_game_interval; }

void backend_present_gpu(NvnTexture *t);
void backend_present(NvnTexture *t) {
    g_last_presented = t;
    if (gt_on()) gt_present(t);  // (runtime/nvn/gpu_thread.c: backend_present_gpu there)
    else backend_present_gpu(t);
    present_game();
}

void backend_present_gpu(NvnTexture *t) {
    extern void vk_pass_gpu_report(void);
    vk_pass_gpu_report();  // (LARECOMP_VK_PASS_GPU: exec.c)
    TexBackend *b = vk_tex(t, true);
    b->presented = true;
    vk_end_pass();
    static const float black[4] = {0, 0, 0, 1};
    wait_foreground();
    // the frame's own work goes out first: the presentation's submission waits for a window image (FIFO: up to a
    // refresh), and work in it would wait with it
    renderer_submit();
    uint64_t t0 = SDL_GetTicksNS();
    bool have = vk_frame_begin(black);
    uint64_t nd = SDL_GetTicksNS() - t0;
    if (nd > 30000000) {
        static int warned;
        if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
    }
    if (have) {
        double dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
        extern void widescreen_frame_end(uint64_t frame);
        widescreen_frame_end(total_frames);
        extern void widescreen_set_factor(float f);
        extern bool widescreen_allowed(void);
        // render scale + placement in the window: runtime/nvn/output_scale.c (Video tab "Internal resolution")
        OutScale os;
        // the GPU's time for the frame (the fence watcher's sum: exec.c), for dynamic resolution
        extern _Atomic uint64_t g_gpu_busy_ns;
        static uint64_t last_gb;
        uint64_t gb = atomic_load(&g_gpu_busy_ns);
        double gpu_ms = gb && last_gb ? (gb - last_gb) / 1e6 : -1;
        last_gb = gb;
        output_scale(dw, dh, widescreen_allowed(), SDL_GetTicksNS() / 1e9, gpu_ms, &os);
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh, shape = os.shape;
        // interiors / dungeons: their 16:9 centre, or the part inside a room wider than one screen
        extern void widescreen_present_crop(uint64_t, double, float *, double *, double *, double *, double *);
        float crop[4];  // uv origin and size
        widescreen_present_crop(total_frames, shape, crop, &vx, &vy, &vw, &vh);
        widescreen_set_factor((float)shape);
        renderer_set_scale((float)sx, (float)sy);
        extern void renderer_set_display_height(float h);
        renderer_set_display_height((float)vh);  // (the game picture's height in the window)
        VkCommandBuffer cb = vk_cmd();
        float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
        memcpy(g_shot_crop, crop, sizeof crop);
        vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
        static long menu_at = -1;
        if (menu_at < 0) menu_at = getenv("LARECOMP_MENU_AT") ? atol(getenv("LARECOMP_MENU_AT")) : 0;
        if (menu_at && (long)total_frames + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        static long win_shot = -1;
        if (win_shot < 0) win_shot = getenv("LARECOMP_WINDOW_SHOT") ? atol(getenv("LARECOMP_WINDOW_SHOT")) : 0;
        bool shot = win_shot && (long)total_frames + 1 == win_shot;
        extern bool fg_frame_wanted(uint64_t frame, TexBackend *fin);
        extern bool fg_draw(VkCommandBuffer cb, VkFormat fmt, uint32_t ww, uint32_t wh, TexBackend *fin, const float rect[4],
                            const float crop[4], bool flip_present);
        bool gen = fg_frame_wanted(total_frames, b);
        g_fg_copy = gen;
        // LARECOMP_FG_SHOT=<frame>: that frame's window picture, its in-between one and the next real one (build/fg_*.png)
        static long fg_shot = -1;
        if (fg_shot < 0) fg_shot = getenv("LARECOMP_FG_SHOT") ? atol(getenv("LARECOMP_FG_SHOT")) : 0;
        bool fs0 = fg_shot && (long)total_frames == fg_shot, fs1 = fg_shot && (long)total_frames == fg_shot + 1;
        vk_frame_end(shot ? "build/window.png" : fs0 ? "build/fg_real0.png" : fs1 ? "build/fg_real1.png" : NULL);
        if (gen && vk_frame_begin(black)) {  // the in-between frame: the next swapchain image, presented a refresh later
            float rect[4] = {(float)vx, (float)vy, (float)vw, (float)vh};
            fg_draw(vk_cmd(), g_vk.sc_format, g_vk.sc_extent.width, g_vk.sc_extent.height, b, rect, crop, pv[0] > 0.5f);
            vk_frame_end(fs0 ? "build/fg_gen0.png" : NULL);
        } else if (!gen && g_fifo_present && menu_open() && game_interval() == 2 && vk_frame_begin(black)) {
            // the menu over the game's 30: a second picture with the menu drawn again from the latest input, a refresh
            // later - scrolling with a finger moves at 60 instead of with the game's frames
            cb = vk_cmd();
            vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
            menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
            vk_frame_end(NULL);
        }
        if (shot && getenv("LARECOMP_EXIT_AFTER_SHOT")) { fflush(stderr); _exit(0); }
    } else {
        renderer_submit();
    }
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    vk_prof_frame(total_frames, SDL_GetTicksNS() - t0);
    frames++;
    total_frames++;
    g_frame_counter++;
    if (getenv("LARECOMP_LOG_SYNCWAIT")) {
        extern uint64_t vk_pending_seq(void);
        LOG("[sync] present f%llu after seq %llu\n", (unsigned long long)total_frames, (unsigned long long)vk_pending_seq() - 1);
    }
    extern void vk_level_change_check(void);
    vk_level_change_check();
    extern void vk_pipeline_cache_autosave(void);
    vk_pipeline_cache_autosave();
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    extern bool g_debug_frame;
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // LARECOMP_DEBUG_FRAMES=<n> (with LARECOMP_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
    {   // LARECOMP_GPUPROF_AT=<frame>[,<frame>...]: GPU time per pass of those frames ("[gpuprof]" in the log)
        extern bool g_gpuprof_frame;
        extern void vk_gpuprof_report(void);
        if (g_gpuprof_frame) { g_gpuprof_frame = false; vk_gpuprof_report(); }
        const char *e = getenv("LARECOMP_GPUPROF_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)total_frames) { g_gpuprof_frame = true; break; }
            if (*p == ',') p++; else break;
        }
    }
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", (unsigned long long)total_frames);
    {   // LARECOMP_SHOT_AT=<frame>,<frame>,... (ascending): the game image at those frames -> build/frames/frame_N.png
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("LARECOMP_SHOT_AT"); if (next) mkdir("build/frames", 0755); }
        while (next && *next) {
            long at = strtol(next, (char **)&next, 10);
            if ((long)total_frames < at) { while (next > getenv("LARECOMP_SHOT_AT") && next[-1] != ',') next--; break; }
            if ((long)total_frames == at) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/frame_%05llu.png", (unsigned long long)total_frames);
                renderer_save_texture_tex(b, path);
            }
            if (*next == ',') next++;
        }
    }
    static long shot_from = -1;
    if (shot_from < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && total_frames % shot_every == 0 && (long)total_frames >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png",
                 (unsigned long long)total_frames);
        renderer_save_texture_tex(b, path);
        // a series (SCREENSHOT_FROM) runs on until LARECOMP_WINDOW_SHOT exits
        if (getenv("LARECOMP_EXIT_AFTER_SHOT") && !shot_from) { fflush(stderr); _exit(0); }
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;
    if (fps_iv < 0) fps_iv = getenv("LARECOMP_FPS_INTERVAL") ? atof(getenv("LARECOMP_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    prev_present = now;
    if (now - last_fps_time >= fps_iv) {
        extern const char *game_level_name(void);
        if (last_fps_time > 0)
        {
            extern double g_game_speed;  // (runtime/hle/framepace.c: the game's speed, its 30 / 60 fps mode)
            extern uint32_t g_game_interval;
            extern _Atomic uint64_t g_gpu_busy_ns;
            static uint64_t gb_at;
            uint64_t gbn = atomic_load(&g_gpu_busy_ns);
            double gpu_avg = frames && gb_at ? (gbn - gb_at) / 1e6 / frames : 0;  // (GPU ms per presented frame)
            gb_at = gbn;
            LOG("[vk] t=%.1f frame %llu %.1f fps max %.0fms gpu %.1fms speed %.0f%% (mode %d) level %s (%s)\n", now,
                (unsigned long long)total_frames, frames / (now - last_fps_time), max_gap * 1000.0, gpu_avg, g_game_speed * 100,
                g_game_interval ? 60 / (int)g_game_interval : 60, game_level_name(), renderer_stats());
        }
        max_gap = 0;
        frames = 0;
        last_fps_time = now;
    }
    static double mem_at = 60.0;  // (memory in steady state: a minute in, then every 10 minutes)
    if (now >= mem_at) { vk_log_memory("playing"); mem_at = now + 600.0; }
}

// ================================================================== UI thread
void vk_exec_init(void);
void vk_shaders_init(void);

// exit() from any thread: let the GPU finish, keep the pipeline cache, then end the process without the DLL
// teardown (driver / validation-layer threads can stall it for minutes while the game's threads still run).
static void quick_exit_handler(void) {
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] exit: waiting for the GPU\n");
    if (g_vk.dev) {
        vkDeviceWaitIdle(g_vk.dev);
        if (trace) LOG("[vk] exit: saving the pipeline cache\n");
        vk_pipeline_cache_save();
    }
    if (trace) LOG("[vk] exit: terminating\n");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
}

void ui_window_init(void) {
    if (window) return;
    atexit(quick_exit_handler);
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  // (Back is a key for the menus, not "close the app")
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");        // (touches are handled as such: menus, launcher, controls)
#endif
    settings_load();
    input_apply_background();  // controllers in the background: "Allow background inputs" (Controls tab)
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    input_load_user_mappings();
    int win_w = 1280, win_h = 720;
    if (getenv("LARECOMP_WINDOW")) sscanf(getenv("LARECOMP_WINDOW"), "%dx%d", &win_w, &win_h);
    SDL_WindowFlags wflags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (getenv("LARECOMP_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
#ifdef __ANDROID__
    wflags |= SDL_WINDOW_FULLSCREEN;  // (immersive: no status / navigation bars over the game)
#endif
    {   // scripted test runs (user input ignored: runtime/hle/input.c input_user_blocked) open without taking the
        // keyboard focus from whatever the player is doing
        const char *v = getenv("LARECOMP_NO_USER_INPUT");
        bool scripted = v ? atoi(v) != 0 : (getenv("LARECOMP_INPUT") || getenv("LARECOMP_DOORJUMP_TEST") || getenv("LARECOMP_PLACE") ||
                                            getenv("LARECOMP_HOST_INPUT") || getenv("LARECOMP_BIND_TEST"));
        if (scripted) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    window = SDL_CreateWindow("LAS Recomp", win_w, win_h, wflags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    rt_window_display(window);
    SDL_AddEventWatch(lifecycle_watch, NULL);
    rt_window_icon(window);
    // the Fullscreen setting applies from the start screen on (not only once the game runs); test windows stay windows
    if (g_settings.fullscreen && !getenv("LARECOMP_WINDOW") && !getenv("LARECOMP_HIDDEN")) SDL_SetWindowFullscreen(window, true);

    create_instance();
    if (!create_surface()) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
    create_swapchain();
    vk_exec_init();
    vk_shaders_init();
}

SDL_Window *ui_window(void) { return window; }

void backend_init(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
    LOG("[vk] %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
}

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
void backend_reset(void) {
    if (!g_vk.dev) return;
    gt_drain();
    vkDeviceWaitIdle(g_vk.dev);
    extern void vk_textures_reset(void);
    extern void vk_exec_reset(void);
    vk_textures_reset();
    vk_exec_reset();
    g_last_presented = NULL;
}
void backend_guest_memory_reset(void) {}  // (exec.c re-imports the heap on first use)
static void clear_window(void *arg) {
    (void)arg;
    static const float black[4] = {0, 0, 0, 1};
    if (g_vk.dev && vk_frame_begin(black)) vk_frame_end(NULL);
}
void backend_clear_window(void) { gt_call(clear_window, NULL); }

int ui_run(void) {
    ui_window_init();
    atomic_store(&ui_ready, true);
    for (;;) {
        if (rt_reboot_requested()) return rt_reboot_requested();  // (runtime/main.c tears the game down)
        SDL_Event e;
        static int log_events = -1;  // LARECOMP_LOG_EVENTS=1: what the window loop gets, every 5 s
        static uint64_t ev_n[0x10000 >> 8], loops, ev_t0;
        if (log_events < 0) log_events = getenv("LARECOMP_LOG_EVENTS") != NULL;
        if (log_events) {
            loops++;
            uint64_t now = SDL_GetTicks();
            if (now - ev_t0 > 5000) {
                LOG("[events] %llu loops in 5 s:", (unsigned long long)loops);
                for (int k = 0; k < (0x10000 >> 8); k++) if (ev_n[k]) LOG(" %#x00:%llu", k, (unsigned long long)ev_n[k]);
                LOG("\n");
                memset(ev_n, 0, sizeof ev_n), loops = 0, ev_t0 = now;
            }
        }
#ifdef __ANDROID__
        // (SDL_WaitEventTimeout spins there: a big core busy all the time for nothing. Touch events are queued by the
        // Java side as they come; looking every 4 ms is quicker than any finger)
        bool any = SDL_PollEvent(&e);
        if (!any) SDL_Delay(4);
        for (bool got = any; got; got = SDL_PollEvent(&e)) {
#else
        while (SDL_WaitEventTimeout(&e, 16)) {
#endif
            if (log_events) ev_n[(e.type >> 8) & 0xff]++;
            input_rumble_poll();
            if (e.type == SDL_EVENT_QUIT) {
                input_rumble_stop();
                LOG("window closed\n");
                exit(0);
            }
            extern bool input_user_blocked(const SDL_Event *e);
            if (input_user_blocked(&e)) continue;  // scripted test runs: the real keyboard / mouse / pads do nothing (as metal.m)
            if (menu_handle_event(&e)) continue;
            input_handle_event(&e);
        }
        menu_apply_window_requests(window);
        input_rumble_poll();
    }
}
