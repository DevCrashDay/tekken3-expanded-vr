/* vr_openxr.c — OpenXR presentation for PSX_VR builds. See vr_openxr.h.
 *
 * Threads: the renderer (game) thread creates everything through the
 * bring-up hooks and fills the screen swapchain once per game frame; a
 * headset thread runs xrWaitFrame / xrBeginFrame / xrEndFrame at the
 * headset's refresh rate, reads the controllers, drives the VR menu and
 * draws it into its own swapchain. s_lock serializes the shared VkQueue (the
 * runtime submits on it inside xrBeginFrame / xrEndFrame and the swapchain
 * acquire / release calls) and guards the state both threads read. */

#include "vr_openxr.h"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "psx_sdl.h"

#if defined(__ANDROID__)
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#endif
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define VR_SETTINGS_PATH "vr_settings.ini"
#define MENU_W 1024
#define MENU_H 768

static void xr_log(const char *what, XrResult r) {
    fprintf(stdout, "psxrecomp: openxr: %s (XrResult %d)\n", what, (int)r);
}

/* ---- loader + function pointers ------------------------------------------ */
static void *s_lib;
static PFN_xrGetInstanceProcAddr p_xrGetInstanceProcAddr;

#define XR_FUNCS(X) \
    X(xrDestroyInstance) X(xrGetSystem) X(xrPollEvent) \
    X(xrCreateSession) X(xrDestroySession) X(xrBeginSession) X(xrEndSession) \
    X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) \
    X(xrCreateReferenceSpace) X(xrDestroySpace) X(xrLocateSpace) \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) \
    X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) \
    X(xrReleaseSwapchainImage) X(xrStringToPath) \
    X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction) \
    X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrSyncActions) \
    X(xrGetActionStateBoolean) X(xrGetActionStateFloat) X(xrGetActionStateVector2f) \
    X(xrGetVulkanGraphicsRequirements2KHR) X(xrCreateVulkanInstanceKHR) \
    X(xrGetVulkanGraphicsDevice2KHR) X(xrCreateVulkanDeviceKHR)
#define XR_OPT_FUNCS(X) \
    X(xrEnumerateDisplayRefreshRatesFB) X(xrRequestDisplayRefreshRateFB) \
    X(xrGetDisplayRefreshRateFB)

#define DECL(n) static PFN_##n p_##n;
XR_FUNCS(DECL)
XR_OPT_FUNCS(DECL)
#undef DECL

/* Vulkan for the menu upload (the headset thread's own command buffer). */
#define VKM_FUNCS(X) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkQueueSubmit) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory)
#define DECL(n) static PFN_##n q_##n;
VKM_FUNCS(DECL)
#undef DECL

/* ---- state ---------------------------------------------------------------- */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static XrInstance s_inst = XR_NULL_HANDLE;
static XrSystemId s_sys = XR_NULL_SYSTEM_ID;
static XrSession  s_sess = XR_NULL_HANDLE;
static XrSpace    s_local = XR_NULL_HANDLE, s_view = XR_NULL_HANDLE;
static int        s_has_refresh, s_has_bd;
#if defined(__ANDROID__)
static jobject    s_activity;
#endif

static pthread_t  s_thread;
static int        s_thread_up;
static volatile int s_thread_run;
static volatile int s_running;     /* session begun (frames go to the headset) */
static volatile int s_focused;     /* input focus */

static VkDevice   s_vkdev;
static VkQueue    s_vkq;
static VkFormat   s_fmt = VK_FORMAT_UNDEFINED;
static int64_t    s_xrfmt;

typedef struct {
    XrSwapchain sc;
    int w, h;
    uint32_t n;
    VkImage img[8];
    int ready;          /* an image has been released: the layer may show it */
} Swap;
static Swap  s_screen, s_menu;
static float s_aspect = 4.0f / 3.0f;

static PsxVrSettings s_set;
static PsxVrRates    s_rates;
static float         s_default_hz;
static int           s_applied_hz = -1;
static int           s_menu_open, s_menu_sel, s_menu_dirty = 1;
static PsxVrNavState s_nav;
static uint16_t      s_pad = 0xFFFF;
static XrPosef       s_base;          /* recentered screen origin in LOCAL */
static int           s_base_valid, s_recenter;

/* Menu upload resources (headset thread). */
static VkCommandPool   s_mpool;
static VkCommandBuffer s_mcb;
static VkFence         s_mfence;
static VkBuffer        s_mbuf;
static VkDeviceMemory  s_mmem;
static uint8_t        *s_mmap;
static uint8_t         s_menu_px[MENU_W * MENU_H * 4];

/* Actions. */
enum { A_A, A_B, A_X, A_Y, A_MENU, A_LCLICK, A_RCLICK, A_NBOOL };
enum { F_LTRIG, F_RTRIG, F_LGRIP, F_RGRIP, A_NFLOAT };
enum { V_LSTICK, V_RSTICK, A_NVEC };
static XrActionSet s_aset = XR_NULL_HANDLE;
static XrAction    s_abool[A_NBOOL], s_afloat[A_NFLOAT], s_avec[A_NVEC];

/* ---- small helpers -------------------------------------------------------- */
int psx_vr_requested(void) {
    const char *e = getenv("PSX_VR");
    return !(e && e[0] == '0');
}

void psx_vr_queue_lock(void)   { pthread_mutex_lock(&s_lock); }
void psx_vr_queue_unlock(void) { pthread_mutex_unlock(&s_lock); }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int has_ext(const XrExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static XrQuaternionf quat_mul(XrQuaternionf a, XrQuaternionf b) {
    XrQuaternionf r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}
static XrVector3f quat_rot(XrQuaternionf q, XrVector3f v) {
    XrQuaternionf p = { v.x, v.y, v.z, 0.0f }, c = { -q.x, -q.y, -q.z, q.w };
    XrQuaternionf r = quat_mul(quat_mul(q, p), c);
    XrVector3f o = { r.x, r.y, r.z };
    return o;
}
static XrPosef pose_ident(void) {
    XrPosef p;
    memset(&p, 0, sizeof p);
    p.orientation.w = 1.0f;
    return p;
}

/* ---- bring-up --------------------------------------------------------------- */
static int load_loader(void) {
    static const char *names[] = { "libopenxr_loader.so", "libopenxr_loader.so.1" };
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !s_lib; i++)
        s_lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!s_lib) { fprintf(stdout, "psxrecomp: openxr: no loader (%s)\n", dlerror()); return 0; }
    p_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)dlsym(s_lib, "xrGetInstanceProcAddr");
    return p_xrGetInstanceProcAddr != NULL;
}

int psx_vr_xr_init(void) {
    if (s_inst) return 1;
    if (!load_loader()) return 0;
    XrResult r;
#if defined(__ANDROID__)
    {
        PFN_xrInitializeLoaderKHR init = NULL;
        p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&init);
        JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
        jobject act = (jobject)SDL_GetAndroidActivity();
        if (!env || !act) return 0;
        JavaVM *vm = NULL;
        (*env)->GetJavaVM(env, &vm);
        s_activity = (*env)->NewGlobalRef(env, act);
        (*env)->DeleteLocalRef(env, act);
        if (init) {
            XrLoaderInitInfoAndroidKHR li = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
            li.applicationVM = vm;
            li.applicationContext = s_activity;
            r = init((const XrLoaderInitInfoBaseHeaderKHR *)&li);
            if (XR_FAILED(r)) { xr_log("xrInitializeLoaderKHR failed", r); return 0; }
        }
    }
#endif
    PFN_xrEnumerateInstanceExtensionProperties enum_ext = NULL;
    PFN_xrCreateInstance create = NULL;
    p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                            (PFN_xrVoidFunction *)&enum_ext);
    p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&create);
    if (!enum_ext || !create) return 0;
    uint32_t n = 0;
    if (XR_FAILED(enum_ext(NULL, 0, &n, NULL)) || n == 0) return 0;
    XrExtensionProperties *props = calloc(n, sizeof *props);
    if (!props) return 0;
    for (uint32_t i = 0; i < n; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    enum_ext(NULL, n, &n, props);
    const char *exts[4];
    uint32_t ne = 0;
    if (!has_ext(props, n, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) {
        free(props);
        fprintf(stdout, "psxrecomp: openxr: runtime has no %s\n", XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        return 0;
    }
    exts[ne++] = XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
#if defined(__ANDROID__)
    if (has_ext(props, n, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME))
        exts[ne++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
#endif
    s_has_refresh = has_ext(props, n, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (s_has_refresh) exts[ne++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
    s_has_bd = has_ext(props, n, XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
    if (s_has_bd) exts[ne++] = XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME;
    free(props);

    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
#if defined(__ANDROID__)
    XrInstanceCreateInfoAndroidKHR ai = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    {
        JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
        JavaVM *vm = NULL;
        (*env)->GetJavaVM(env, &vm);
        ai.applicationVM = vm;
        ai.applicationActivity = s_activity;
        ci.next = &ai;
    }
#endif
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "Tekken 3 Expanded");
    snprintf(ci.applicationInfo.engineName, sizeof ci.applicationInfo.engineName, "psxrecomp");
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = ne;
    ci.enabledExtensionNames = exts;
    r = create(&ci, &s_inst);
    if (XR_FAILED(r)) { xr_log("xrCreateInstance failed", r); s_inst = XR_NULL_HANDLE; return 0; }

#define LOAD(n) \
    if (XR_FAILED(p_xrGetInstanceProcAddr(s_inst, #n, (PFN_xrVoidFunction *)&p_##n)) || !p_##n) { \
        xr_log("missing " #n, XR_ERROR_FUNCTION_UNSUPPORTED); psx_vr_shutdown(); return 0; }
    XR_FUNCS(LOAD)
#undef LOAD
#define LOADOPT(n) p_xrGetInstanceProcAddr(s_inst, #n, (PFN_xrVoidFunction *)&p_##n);
    if (s_has_refresh) { XR_OPT_FUNCS(LOADOPT) }
#undef LOADOPT

    XrSystemGetInfo gi = { XR_TYPE_SYSTEM_GET_INFO };
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = p_xrGetSystem(s_inst, &gi, &s_sys);
    if (XR_FAILED(r)) { xr_log("no headset (xrGetSystem)", r); psx_vr_shutdown(); return 0; }
    XrGraphicsRequirementsVulkan2KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    r = p_xrGetVulkanGraphicsRequirements2KHR(s_inst, s_sys, &req);
    if (XR_FAILED(r)) { xr_log("xrGetVulkanGraphicsRequirements2KHR failed", r); psx_vr_shutdown(); return 0; }

    psx_vr_settings_defaults(&s_set);
    psx_vr_settings_load(&s_set, VR_SETTINGS_PATH);
    fprintf(stdout, "psxrecomp: openxr: headset found\n");
    return 1;
}

VkResult psx_vr_create_vk_instance(PFN_vkGetInstanceProcAddr gipa,
                                   const VkInstanceCreateInfo *vci, VkInstance *out) {
    if (!s_inst) return VK_ERROR_INITIALIZATION_FAILED;
    XrVulkanInstanceCreateInfoKHR ci = { XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
    ci.systemId = s_sys;
    ci.pfnGetInstanceProcAddr = gipa;
    ci.vulkanCreateInfo = vci;
    VkResult vr = VK_SUCCESS;
    XrResult r = p_xrCreateVulkanInstanceKHR(s_inst, &ci, out, &vr);
    if (XR_FAILED(r)) { xr_log("xrCreateVulkanInstanceKHR failed", r); return VK_ERROR_INITIALIZATION_FAILED; }
    return vr;
}

VkResult psx_vr_vk_physical_device(VkInstance inst, VkPhysicalDevice *out) {
    if (!s_inst) return VK_ERROR_INITIALIZATION_FAILED;
    XrVulkanGraphicsDeviceGetInfoKHR gi = { XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
    gi.systemId = s_sys;
    gi.vulkanInstance = inst;
    XrResult r = p_xrGetVulkanGraphicsDevice2KHR(s_inst, &gi, out);
    if (XR_FAILED(r)) { xr_log("xrGetVulkanGraphicsDevice2KHR failed", r); return VK_ERROR_INITIALIZATION_FAILED; }
    return VK_SUCCESS;
}

VkResult psx_vr_create_vk_device(PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice phys,
                                 const VkDeviceCreateInfo *vci, VkDevice *out) {
    if (!s_inst) return VK_ERROR_INITIALIZATION_FAILED;
    XrVulkanDeviceCreateInfoKHR ci = { XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
    ci.systemId = s_sys;
    ci.pfnGetInstanceProcAddr = gipa;
    ci.vulkanPhysicalDevice = phys;
    ci.vulkanCreateInfo = vci;
    VkResult vr = VK_SUCCESS;
    XrResult r = p_xrCreateVulkanDeviceKHR(s_inst, &ci, out, &vr);
    if (XR_FAILED(r)) { xr_log("xrCreateVulkanDeviceKHR failed", r); return VK_ERROR_INITIALIZATION_FAILED; }
    return vr;
}

/* ---- swapchains ----------------------------------------------------------- */
static int pick_format(void) {
    uint32_t n = 0;
    if (XR_FAILED(p_xrEnumerateSwapchainFormats(s_sess, 0, &n, NULL)) || n == 0) return 0;
    int64_t *f = calloc(n, sizeof *f);
    if (!f) return 0;
    p_xrEnumerateSwapchainFormats(s_sess, n, &n, f);
    static const VkFormat want[] = { VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB };
    for (size_t k = 0; k < 2 && !s_xrfmt; k++)
        for (uint32_t i = 0; i < n; i++)
            if (f[i] == (int64_t)want[k]) { s_xrfmt = f[i]; s_fmt = want[k]; break; }
    free(f);
    if (!s_xrfmt) fprintf(stdout, "psxrecomp: openxr: no 8-bit sRGB swapchain format\n");
    return s_xrfmt != 0;
}

static void swap_destroy(Swap *s) {
    if (s->sc) p_xrDestroySwapchain(s->sc);
    memset(s, 0, sizeof *s);
}

static int swap_create(Swap *s, int w, int h) {
    swap_destroy(s);
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = s_xrfmt;
    ci.sampleCount = 1;
    ci.width = (uint32_t)w;
    ci.height = (uint32_t)h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XrResult r = p_xrCreateSwapchain(s_sess, &ci, &s->sc);
    if (XR_FAILED(r)) { xr_log("xrCreateSwapchain failed", r); s->sc = XR_NULL_HANDLE; return 0; }
    XrSwapchainImageVulkan2KHR imgs[8];
    for (int i = 0; i < 8; i++) {
        memset(&imgs[i], 0, sizeof imgs[i]);
        imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    }
    uint32_t n = 0;
    p_xrEnumerateSwapchainImages(s->sc, 0, &n, NULL);
    if (n > 8) n = 8;
    p_xrEnumerateSwapchainImages(s->sc, n, &n, (XrSwapchainImageBaseHeader *)imgs);
    for (uint32_t i = 0; i < n; i++) s->img[i] = imgs[i].image;
    s->n = n;
    s->w = w;
    s->h = h;
    return n > 0;
}

/* Acquire + wait; s_lock is NOT held on entry. Returns the index or -1. */
static int swap_acquire(Swap *s) {
    uint32_t idx = 0;
    pthread_mutex_lock(&s_lock);
    XrResult r = s->sc ? p_xrAcquireSwapchainImage(s->sc, NULL, &idx) : XR_ERROR_HANDLE_INVALID;
    pthread_mutex_unlock(&s_lock);
    if (XR_FAILED(r)) return -1;
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = 500000000;   /* 0.5 s */
    for (int tries = 0; tries < 8; tries++) {
        r = p_xrWaitSwapchainImage(s->sc, &wi);
        if (r != XR_TIMEOUT_EXPIRED) break;
    }
    return XR_SUCCEEDED(r) && r != XR_TIMEOUT_EXPIRED ? (int)idx : -1;
}

static void swap_release(Swap *s) {
    pthread_mutex_lock(&s_lock);
    if (s->sc && XR_SUCCEEDED(p_xrReleaseSwapchainImage(s->sc, NULL))) s->ready = 1;
    pthread_mutex_unlock(&s_lock);
}

VkFormat psx_vr_screen_format(void) { return s_fmt; }

void psx_vr_settings_get(PsxVrSettings *out) {
    pthread_mutex_lock(&s_lock);
    *out = s_set;
    pthread_mutex_unlock(&s_lock);
}

static int   s_acq = -1;      /* the game thread's acquired screen image */

int psx_vr_frame_acquire(int w, int h, VkImage *out_image) {
    if (!s_running || !s_sess || w <= 0 || h <= 0) return 0;
    if (s_screen.w != w || s_screen.h != h || !s_screen.sc) {
        pthread_mutex_lock(&s_lock);
        int ok = swap_create(&s_screen, w, h);
        pthread_mutex_unlock(&s_lock);
        if (!ok) return 0;
    }
    s_acq = swap_acquire(&s_screen);
    if (s_acq < 0) return 0;
    *out_image = s_screen.img[s_acq];
    return 1;
}

void psx_vr_frame_release(float aspect) {
    if (s_acq < 0) return;
    swap_release(&s_screen);
    pthread_mutex_lock(&s_lock);
    if (aspect > 0.1f) s_aspect = aspect;
    pthread_mutex_unlock(&s_lock);
    s_acq = -1;
}

/* ---- input ------------------------------------------------------------------ */
int psx_vr_controllers_active(void) { return s_running && s_focused; }
int psx_vr_menu_open(void) { return s_menu_open; }
uint16_t psx_vr_pad_word(void) {
    pthread_mutex_lock(&s_lock);
    uint16_t w = s_pad;
    pthread_mutex_unlock(&s_lock);
    return w;
}

static XrPath path(const char *s) {
    XrPath p = XR_NULL_PATH;
    p_xrStringToPath(s_inst, s, &p);
    return p;
}

static XrAction make_action(const char *name, const char *loc, XrActionType type) {
    XrActionCreateInfo ci = { XR_TYPE_ACTION_CREATE_INFO };
    ci.actionType = type;
    snprintf(ci.actionName, sizeof ci.actionName, "%s", name);
    snprintf(ci.localizedActionName, sizeof ci.localizedActionName, "%s", loc);
    XrAction a = XR_NULL_HANDLE;
    p_xrCreateAction(s_aset, &ci, &a);
    return a;
}

typedef struct { XrAction *a; const char *path; } Bind;

static void suggest(const char *profile, const Bind *b, int n) {
    XrActionSuggestedBinding sb[24];
    int k = 0;
    for (int i = 0; i < n && k < 24; i++) {
        if (!*b[i].a) continue;
        sb[k].action = *b[i].a;
        sb[k].binding = path(b[i].path);
        k++;
    }
    XrInteractionProfileSuggestedBinding s = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    s.interactionProfile = path(profile);
    s.countSuggestedBindings = (uint32_t)k;
    s.suggestedBindings = sb;
    XrResult r = p_xrSuggestInteractionProfileBindings(s_inst, &s);
    if (XR_FAILED(r)) fprintf(stdout, "psxrecomp: openxr: bindings for %s refused (%d)\n", profile, (int)r);
}

static int create_actions(void) {
    XrActionSetCreateInfo ci = { XR_TYPE_ACTION_SET_CREATE_INFO };
    snprintf(ci.actionSetName, sizeof ci.actionSetName, "game");
    snprintf(ci.localizedActionSetName, sizeof ci.localizedActionSetName, "Game");
    if (XR_FAILED(p_xrCreateActionSet(s_inst, &ci, &s_aset))) return 0;
    s_abool[A_A] = make_action("a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT);
    s_abool[A_B] = make_action("b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT);
    s_abool[A_X] = make_action("x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT);
    s_abool[A_Y] = make_action("y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT);
    s_abool[A_MENU] = make_action("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    s_abool[A_LCLICK] = make_action("left_stick_click", "Left stick click (VR menu)", XR_ACTION_TYPE_BOOLEAN_INPUT);
    s_abool[A_RCLICK] = make_action("right_stick_click", "Right stick click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    s_afloat[F_LTRIG] = make_action("left_trigger", "Left trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    s_afloat[F_RTRIG] = make_action("right_trigger", "Right trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    s_afloat[F_LGRIP] = make_action("left_grip", "Left grip", XR_ACTION_TYPE_FLOAT_INPUT);
    s_afloat[F_RGRIP] = make_action("right_grip", "Right grip", XR_ACTION_TYPE_FLOAT_INPUT);
    s_avec[V_LSTICK] = make_action("left_stick", "Left stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    s_avec[V_RSTICK] = make_action("right_stick", "Right stick", XR_ACTION_TYPE_VECTOR2F_INPUT);

    const Bind touch[] = {
        { &s_abool[A_A], "/user/hand/right/input/a/click" },
        { &s_abool[A_B], "/user/hand/right/input/b/click" },
        { &s_abool[A_X], "/user/hand/left/input/x/click" },
        { &s_abool[A_Y], "/user/hand/left/input/y/click" },
        { &s_abool[A_MENU], "/user/hand/left/input/menu/click" },
        { &s_abool[A_LCLICK], "/user/hand/left/input/thumbstick/click" },
        { &s_abool[A_RCLICK], "/user/hand/right/input/thumbstick/click" },
        { &s_afloat[F_LTRIG], "/user/hand/left/input/trigger/value" },
        { &s_afloat[F_RTRIG], "/user/hand/right/input/trigger/value" },
        { &s_afloat[F_LGRIP], "/user/hand/left/input/squeeze/value" },
        { &s_afloat[F_RGRIP], "/user/hand/right/input/squeeze/value" },
        { &s_avec[V_LSTICK], "/user/hand/left/input/thumbstick" },
        { &s_avec[V_RSTICK], "/user/hand/right/input/thumbstick" },
    };
    const int nt = (int)(sizeof touch / sizeof touch[0]);
    /* The Pico 4 controllers use the same input names as Touch. */
    if (s_has_bd) suggest("/interaction_profiles/bytedance/pico4_controller", touch, nt);
    suggest("/interaction_profiles/oculus/touch_controller", touch, nt);
    const Bind simple[] = {
        { &s_abool[A_A], "/user/hand/right/input/select/click" },
        { &s_abool[A_LCLICK], "/user/hand/left/input/select/click" },
        { &s_abool[A_MENU], "/user/hand/left/input/menu/click" },
    };
    suggest("/interaction_profiles/khr/simple_controller", simple, 3);

    XrSessionActionSetsAttachInfo ai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    ai.countActionSets = 1;
    ai.actionSets = &s_aset;
    return XR_SUCCEEDED(p_xrAttachSessionActionSets(s_sess, &ai));
}

static int get_bool(XrAction a) {
    if (!a) return 0;
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
    return XR_SUCCEEDED(p_xrGetActionStateBoolean(s_sess, &gi, &st)) && st.isActive && st.currentState;
}
static float get_float(XrAction a) {
    if (!a) return 0.0f;
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateFloat st = { XR_TYPE_ACTION_STATE_FLOAT };
    return XR_SUCCEEDED(p_xrGetActionStateFloat(s_sess, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}
static XrVector2f get_vec(XrAction a) {
    XrVector2f z = { 0.0f, 0.0f };
    if (!a) return z;
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
    return XR_SUCCEEDED(p_xrGetActionStateVector2f(s_sess, &gi, &st)) && st.isActive ? st.currentState : z;
}

static void read_controls(PsxVrControls *c) {
    memset(c, 0, sizeof *c);
    if (!s_focused) return;
    XrActiveActionSet as = { s_aset, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &as;
    if (XR_FAILED(p_xrSyncActions(s_sess, &si))) return;
    c->a = get_bool(s_abool[A_A]);
    c->b = get_bool(s_abool[A_B]);
    c->x = get_bool(s_abool[A_X]);
    c->y = get_bool(s_abool[A_Y]);
    c->menu = get_bool(s_abool[A_MENU]);
    c->stick_click_l = get_bool(s_abool[A_LCLICK]);
    c->stick_click_r = get_bool(s_abool[A_RCLICK]);
    c->trigger_l = get_float(s_afloat[F_LTRIG]);
    c->trigger_r = get_float(s_afloat[F_RTRIG]);
    c->grip_l = get_float(s_afloat[F_LGRIP]);
    c->grip_r = get_float(s_afloat[F_RGRIP]);
    XrVector2f l = get_vec(s_avec[V_LSTICK]), r = get_vec(s_avec[V_RSTICK]);
    c->stick_lx = l.x; c->stick_ly = l.y;
    c->stick_rx = r.x; c->stick_ry = r.y;
}

/* ---- refresh rate ------------------------------------------------------------- */
static void query_rates(void) {
    memset(&s_rates, 0, sizeof s_rates);
    if (!s_has_refresh || !p_xrEnumerateDisplayRefreshRatesFB) return;
    float hz[16];
    uint32_t n = 0;
    if (XR_FAILED(p_xrEnumerateDisplayRefreshRatesFB(s_sess, 0, &n, NULL)) || n == 0) return;
    if (n > 16) n = 16;
    p_xrEnumerateDisplayRefreshRatesFB(s_sess, n, &n, hz);
    for (uint32_t i = 0; i < n && s_rates.count < 8; i++) {
        int v = (int)lrintf(hz[i]);
        int dup = 0;
        for (int k = 0; k < s_rates.count; k++) dup |= s_rates.hz[k] == v;
        if (!dup && v > 0) s_rates.hz[s_rates.count++] = v;
    }
    if (p_xrGetDisplayRefreshRateFB) p_xrGetDisplayRefreshRateFB(s_sess, &s_default_hz);
}

static void apply_refresh(int hz) {
    if (!s_has_refresh || !p_xrRequestDisplayRefreshRateFB || hz == s_applied_hz) return;
    float want = 0.0f;
    if (hz > 0) {
        /* The offered rate the setting was rounded from. */
        float hzs[16];
        uint32_t n = 0;
        p_xrEnumerateDisplayRefreshRatesFB(s_sess, 0, &n, NULL);
        if (n > 16) n = 16;
        p_xrEnumerateDisplayRefreshRatesFB(s_sess, n, &n, hzs);
        for (uint32_t i = 0; i < n; i++) if ((int)lrintf(hzs[i]) == hz) want = hzs[i];
        if (want <= 0.0f) return;
    } else {
        want = s_default_hz;
        if (want <= 0.0f) { s_applied_hz = hz; return; }
    }
    XrResult r = p_xrRequestDisplayRefreshRateFB(s_sess, want);
    if (XR_FAILED(r)) xr_log("refresh rate request refused", r);
    s_applied_hz = hz;
}

/* ---- the menu panel -------------------------------------------------------------- */
static uint32_t find_mem(VkPhysicalDevice phys, PFN_vkGetInstanceProcAddr gipa, VkInstance inst,
                         uint32_t bits, VkMemoryPropertyFlags want) {
    PFN_vkGetPhysicalDeviceMemoryProperties gmp = (PFN_vkGetPhysicalDeviceMemoryProperties)
        gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    if (!gmp) return UINT32_MAX;
    VkPhysicalDeviceMemoryProperties mp;
    gmp(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int menu_gpu_init(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, VkPhysicalDevice phys,
                         uint32_t qfam) {
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    if (!gdpa) return 0;
#define LOAD(n) q_##n = (PFN_##n)gdpa(s_vkdev, #n); if (!q_##n) return 0;
    VKM_FUNCS(LOAD)
#undef LOAD
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = qfam;
    if (q_vkCreateCommandPool(s_vkdev, &pi, NULL, &s_mpool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = s_mpool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (q_vkAllocateCommandBuffers(s_vkdev, &ai, &s_mcb) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (q_vkCreateFence(s_vkdev, &fi, NULL, &s_mfence) != VK_SUCCESS) return 0;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = sizeof s_menu_px;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (q_vkCreateBuffer(s_vkdev, &bi, NULL, &s_mbuf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    q_vkGetBufferMemoryRequirements(s_vkdev, s_mbuf, &req);
    VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = find_mem(phys, gipa, inst, req.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mi.memoryTypeIndex == UINT32_MAX ||
        q_vkAllocateMemory(s_vkdev, &mi, NULL, &s_mmem) != VK_SUCCESS) return 0;
    q_vkBindBufferMemory(s_vkdev, s_mbuf, s_mmem, 0);
    void *map = NULL;
    if (q_vkMapMemory(s_vkdev, s_mmem, 0, sizeof s_menu_px, 0, &map) != VK_SUCCESS) return 0;
    s_mmap = (uint8_t *)map;
    return 1;
}

static void menu_gpu_free(void) {
    if (!s_vkdev) return;
    if (s_mbuf) q_vkDestroyBuffer(s_vkdev, s_mbuf, NULL);
    if (s_mmem) q_vkFreeMemory(s_vkdev, s_mmem, NULL);
    if (s_mfence) q_vkDestroyFence(s_vkdev, s_mfence, NULL);
    if (s_mpool) q_vkDestroyCommandPool(s_vkdev, s_mpool, NULL);
    s_mbuf = VK_NULL_HANDLE; s_mmem = VK_NULL_HANDLE; s_mfence = VK_NULL_HANDLE;
    s_mpool = VK_NULL_HANDLE; s_mcb = VK_NULL_HANDLE; s_mmap = NULL;
}

/* Draw the menu and copy it into its swapchain (headset thread). */
static void menu_upload(void) {
    if (!s_mmap) return;
    if (!s_menu.sc) {
        pthread_mutex_lock(&s_lock);
        int ok = swap_create(&s_menu, MENU_W, MENU_H);
        pthread_mutex_unlock(&s_lock);
        if (!ok) return;
    }
    PsxVrSettings set;
    pthread_mutex_lock(&s_lock);
    set = s_set;
    pthread_mutex_unlock(&s_lock);
    psx_vr_menu_draw(&set, s_menu_sel, s_menu_px, MENU_W, MENU_H);
    if (s_fmt == VK_FORMAT_B8G8R8A8_SRGB) {
        for (size_t i = 0; i < sizeof s_menu_px; i += 4) {
            uint8_t t = s_menu_px[i]; s_menu_px[i] = s_menu_px[i + 2]; s_menu_px[i + 2] = t;
        }
    }
    int idx = swap_acquire(&s_menu);
    if (idx < 0) return;
    memcpy(s_mmap, s_menu_px, sizeof s_menu_px);
    VkImage img = s_menu.img[idx];
    q_vkResetCommandBuffer(s_mcb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    q_vkBeginCommandBuffer(s_mcb, &bi);
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    q_vkCmdPipelineBarrier(s_mcb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy bc;
    memset(&bc, 0, sizeof bc);
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = MENU_W;
    bc.imageExtent.height = MENU_H;
    bc.imageExtent.depth = 1;
    q_vkCmdCopyBufferToImage(s_mcb, s_mbuf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    q_vkCmdPipelineBarrier(s_mcb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    q_vkEndCommandBuffer(s_mcb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_mcb;
    pthread_mutex_lock(&s_lock);
    q_vkQueueSubmit(s_vkq, 1, &si, s_mfence);
    pthread_mutex_unlock(&s_lock);
    q_vkWaitForFences(s_vkdev, 1, &s_mfence, VK_TRUE, UINT64_MAX);
    q_vkResetFences(s_vkdev, 1, &s_mfence);
    swap_release(&s_menu);
    s_menu_dirty = 0;
}

static void save_settings(void) {
    PsxVrSettings set;
    pthread_mutex_lock(&s_lock);
    set = s_set;
    pthread_mutex_unlock(&s_lock);
    if (!psx_vr_settings_save(&set, VR_SETTINGS_PATH))
        fprintf(stdout, "psxrecomp: openxr: could not write %s\n", VR_SETTINGS_PATH);
}

static void menu_close(void) {
    s_menu_open = 0;
    save_settings();
}

static void menu_action(PsxVrMenuAction a) {
    switch (a) {
    case PSX_VR_MENU_CHANGED: s_menu_dirty = 1; break;
    case PSX_VR_MENU_CLOSE:   menu_close(); break;
    case PSX_VR_MENU_RECENTER: s_recenter = 1; break;
    default: break;
    }
}

static void menu_input(const PsxVrControls *c) {
    PsxVrNav nav = psx_vr_nav(&s_nav, c, now_s());
    if (!s_menu_open) {
        if (nav == PSX_VR_NAV_TOGGLE) { s_menu_open = 1; s_menu_sel = 0; s_menu_dirty = 1; }
        return;
    }
    int step = 0;
    switch (nav) {
    case PSX_VR_NAV_TOGGLE: case PSX_VR_NAV_BACK: menu_close(); return;
    case PSX_VR_NAV_UP:    s_menu_sel = (s_menu_sel + PSX_VR_ITEM_COUNT - 1) % PSX_VR_ITEM_COUNT; s_menu_dirty = 1; return;
    case PSX_VR_NAV_DOWN:  s_menu_sel = (s_menu_sel + 1) % PSX_VR_ITEM_COUNT; s_menu_dirty = 1; return;
    case PSX_VR_NAV_LEFT:  step = -1; break;
    case PSX_VR_NAV_RIGHT: step = 1; break;
    case PSX_VR_NAV_ACCEPT: step = 0; break;
    default: return;
    }
    pthread_mutex_lock(&s_lock);
    PsxVrMenuAction a = psx_vr_menu_step(&s_set, s_menu_sel, step, &s_rates);
    pthread_mutex_unlock(&s_lock);
    menu_action(a);
}

/* ---- the frame loop ---------------------------------------------------------------- */
static void recenter(XrTime t) {
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(p_xrLocateSpace(s_view, s_local, t, &loc))) return;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return;
    /* Keep the head's heading only, so the screen stands upright. */
    XrVector3f fwd = { 0.0f, 0.0f, -1.0f };
    fwd = quat_rot(loc.pose.orientation, fwd);
    float yaw = atan2f(-fwd.x, -fwd.z);
    s_base.orientation.x = 0.0f;
    s_base.orientation.y = sinf(yaw * 0.5f);
    s_base.orientation.z = 0.0f;
    s_base.orientation.w = cosf(yaw * 0.5f);
    s_base.position = loc.pose.position;
    s_base_valid = 1;
    s_recenter = 0;
}

static void request_quit(void) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
}

static void poll_events(void) {
    XrEventDataBuffer ev;
    for (;;) {
        memset(&ev, 0, sizeof ev);
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        if (p_xrPollEvent(s_inst, &ev) != XR_SUCCESS) break;
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const XrEventDataSessionStateChanged *sc = (const XrEventDataSessionStateChanged *)&ev;
            switch (sc->state) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = p_xrBeginSession(s_sess, &bi);
                if (XR_SUCCEEDED(r)) {
                    s_running = 1;
                    query_rates();
                    s_applied_hz = -1;
                } else {
                    xr_log("xrBeginSession failed", r);
                }
                break;
            }
            case XR_SESSION_STATE_FOCUSED: s_focused = 1; break;
            case XR_SESSION_STATE_VISIBLE: s_focused = 0; break;
            case XR_SESSION_STATE_STOPPING:
                s_focused = 0;
                s_running = 0;
                p_xrEndSession(s_sess);
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                s_focused = 0;
                s_running = 0;
                request_quit();
                break;
            default: break;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            s_running = 0;
            request_quit();
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            s_recenter = 1;
            break;
        default: break;
        }
    }
}

static void *xr_thread(void *arg) {
    (void)arg;
    while (s_thread_run) {
        poll_events();
        if (!s_running) { usleep(10000); continue; }

        XrFrameState fs = { XR_TYPE_FRAME_STATE };
        XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
        if (XR_FAILED(p_xrWaitFrame(s_sess, &wi, &fs))) { usleep(5000); continue; }
        pthread_mutex_lock(&s_lock);
        XrResult br = p_xrBeginFrame(s_sess, NULL);
        pthread_mutex_unlock(&s_lock);
        if (XR_FAILED(br)) continue;

        PsxVrControls c;
        read_controls(&c);
        menu_input(&c);
        uint16_t pad = s_menu_open ? 0xFFFF : psx_vr_pad_word_from(&c);
        if (s_menu_open && s_menu_dirty) menu_upload();
        if (s_recenter || !s_base_valid) recenter(fs.predictedDisplayTime);

        PsxVrSettings set;
        pthread_mutex_lock(&s_lock);
        s_pad = pad;
        set = s_set;
        pthread_mutex_unlock(&s_lock);
        apply_refresh(set.refresh_hz);

        /* Screen + menu as quad layers. */
        XrCompositionLayerQuad q[3];
        const XrCompositionLayerBaseHeader *layers[3];
        uint32_t nl = 0;
        memset(q, 0, sizeof q);
        pthread_mutex_lock(&s_lock);
        if (fs.shouldRender && s_screen.sc && s_screen.ready) {
            const float d = (float)set.distance_cm / 100.0f;
            const float wm = 2.0f * d * tanf((float)set.fov * 0.5f * 3.14159265f / 180.0f);
            XrVector3f off = { 0.0f, (float)set.height_cm / 100.0f, -d };
            XrPosef pose = pose_ident();
            XrSpace space = s_view;
            if (!set.follow_head) {
                space = s_local;
                if (s_base_valid) {
                    XrVector3f r = quat_rot(s_base.orientation, off);
                    pose.orientation = s_base.orientation;
                    pose.position.x = s_base.position.x + r.x;
                    pose.position.y = s_base.position.y + r.y;
                    pose.position.z = s_base.position.z + r.z;
                } else {
                    pose.position = off;
                }
            } else {
                pose.position = off;
            }
            const int ew = s_screen.w / 2;
            for (int e = 0; e < 2; e++) {
                XrCompositionLayerQuad *l = &q[nl];
                l->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                l->space = space;
                l->eyeVisibility = e ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
                l->subImage.swapchain = s_screen.sc;
                l->subImage.imageRect.offset.x = e * ew;
                l->subImage.imageRect.offset.y = 0;
                l->subImage.imageRect.extent.width = ew;
                l->subImage.imageRect.extent.height = s_screen.h;
                l->pose = pose;
                l->size.width = wm;
                l->size.height = wm / s_aspect;
                layers[nl] = (const XrCompositionLayerBaseHeader *)l;
                nl++;
            }
        }
        if (fs.shouldRender && s_menu_open && s_menu.sc && s_menu.ready) {
            XrCompositionLayerQuad *l = &q[nl];
            l->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            l->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            l->space = s_view;
            l->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            l->subImage.swapchain = s_menu.sc;
            l->subImage.imageRect.extent.width = MENU_W;
            l->subImage.imageRect.extent.height = MENU_H;
            l->pose = pose_ident();
            l->pose.position.z = -1.0f;
            l->size.width = 0.9f;
            l->size.height = 0.9f * (float)MENU_H / (float)MENU_W;
            layers[nl] = (const XrCompositionLayerBaseHeader *)l;
            nl++;
        }
        XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = nl;
        ei.layers = nl ? layers : NULL;
        XrResult er = p_xrEndFrame(s_sess, &ei);
        pthread_mutex_unlock(&s_lock);
        if (XR_FAILED(er)) xr_log("xrEndFrame failed", er);
    }
    return NULL;
}

int psx_vr_start(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, VkPhysicalDevice phys,
                 VkDevice dev, uint32_t queue_family, VkQueue queue) {
    if (!s_inst) return 0;
    XrGraphicsBindingVulkan2KHR gb = { XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
    gb.instance = inst;
    gb.physicalDevice = phys;
    gb.device = dev;
    gb.queueFamilyIndex = queue_family;
    gb.queueIndex = 0;
    XrSessionCreateInfo ci = { XR_TYPE_SESSION_CREATE_INFO };
    ci.next = &gb;
    ci.systemId = s_sys;
    XrResult r = p_xrCreateSession(s_inst, &ci, &s_sess);
    if (XR_FAILED(r)) { xr_log("xrCreateSession failed", r); s_sess = XR_NULL_HANDLE; return 0; }
    s_vkdev = dev;
    s_vkq = queue;

    XrReferenceSpaceCreateInfo si = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    si.poseInReferenceSpace = pose_ident();
    si.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (XR_FAILED(p_xrCreateReferenceSpace(s_sess, &si, &s_local))) return 0;
    si.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (XR_FAILED(p_xrCreateReferenceSpace(s_sess, &si, &s_view))) return 0;
    if (!pick_format()) return 0;
    if (!create_actions()) fprintf(stdout, "psxrecomp: openxr: controller actions unavailable\n");
    if (!menu_gpu_init(gipa, inst, phys, queue_family))
        fprintf(stdout, "psxrecomp: openxr: menu panel unavailable\n");

    s_thread_run = 1;
    if (pthread_create(&s_thread, NULL, xr_thread, NULL) != 0) { s_thread_run = 0; return 0; }
    s_thread_up = 1;
    fprintf(stdout, "psxrecomp: openxr: session created; VR settings: left stick click\n");
    return 1;
}

void psx_vr_shutdown(void) {
    if (s_thread_up) {
        s_thread_run = 0;
        pthread_join(s_thread, NULL);
        s_thread_up = 0;
    }
    s_running = 0;   /* a session is destroyable in any state */
    s_focused = 0;
    menu_gpu_free();
    if (s_sess) {
        swap_destroy(&s_screen);
        swap_destroy(&s_menu);
        if (s_local) p_xrDestroySpace(s_local);
        if (s_view) p_xrDestroySpace(s_view);
        if (s_aset) p_xrDestroyActionSet(s_aset);
        p_xrDestroySession(s_sess);
    }
    s_local = s_view = XR_NULL_HANDLE;
    s_aset = XR_NULL_HANDLE;
    s_sess = XR_NULL_HANDLE;
    if (s_inst && p_xrDestroyInstance) p_xrDestroyInstance(s_inst);
    s_inst = XR_NULL_HANDLE;
    s_vkdev = VK_NULL_HANDLE;
#if defined(__ANDROID__)
    if (s_activity) {
        JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
        if (env) (*env)->DeleteGlobalRef(env, s_activity);
        s_activity = NULL;
    }
#endif
}
