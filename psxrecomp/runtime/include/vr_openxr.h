#ifndef PSX_VR_OPENXR_H
#define PSX_VR_OPENXR_H

/* OpenXR presentation for PSX_VR builds (the Pico 4 APK): the game's picture
 * on a virtual screen in the headset, one view per eye, plus the in-headset
 * VR settings menu (left thumbstick click) and the controller -> pad mapping.
 *
 * gpu_vk_renderer.c owns Vulkan; OpenXR decides which instance and device it
 * runs on (XR_KHR_vulkan_enable2), so the renderer calls the bring-up hooks
 * below in order instead of its own vkCreateInstance / vkCreateDevice, then
 * psx_vr_start() once its device is up. A thread then runs the headset's
 * frame loop (xrWaitFrame paces it at the headset's refresh rate, not the
 * game's 60 Hz) and submits the screen as quad layers, so the compositor does
 * the head tracking. The game thread renders each new frame's two eye views
 * into the screen's swapchain between psx_vr_frame_acquire and
 * psx_vr_frame_release.
 *
 * The OpenXR loader is opened at runtime (libopenxr_loader.so), so nothing
 * links against it and a build without a headset runtime falls back to the
 * flat picture. */

#include <stdint.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include "vr_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when this build presents in VR (PSX_VR) and the environment does not
 * turn it off (PSX_VR=0). */
int  psx_vr_requested(void);

/* ---- Vulkan bring-up (renderer thread, in this order) ------------------- */
/* Load the loader, create the XrInstance and find the headset. 0 = no VR. */
int  psx_vr_xr_init(void);
/* vkCreateInstance / vkCreateDevice through the runtime, which adds what it
 * needs. VK_SUCCESS or the failure. */
VkResult psx_vr_create_vk_instance(PFN_vkGetInstanceProcAddr gipa,
                                   const VkInstanceCreateInfo *ci, VkInstance *out);
/* The GPU the headset is attached to. */
VkResult psx_vr_vk_physical_device(VkInstance inst, VkPhysicalDevice *out);
VkResult psx_vr_create_vk_device(PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice phys,
                                 const VkDeviceCreateInfo *ci, VkDevice *out);
/* Create the session on the renderer's device/queue and start the headset
 * frame loop. 0 = failed (the renderer stays flat). */
int  psx_vr_start(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, VkPhysicalDevice phys,
                  VkDevice dev, uint32_t queue_family, VkQueue queue);
/* Stop the frame loop and destroy the session and instance (before the
 * renderer destroys its device). */
void psx_vr_shutdown(void);

/* The renderer's queue is shared with the runtime (xrBeginFrame / xrEndFrame
 * / swapchain acquire + release submit on it), and Vulkan requires a queue to
 * be externally synchronized: every vkQueueSubmit / vkQueueWaitIdle the
 * renderer makes goes through this lock. */
void psx_vr_queue_lock(void);
void psx_vr_queue_unlock(void);

/* ---- the screen (game thread) ------------------------------------------- */
/* The screen swapchain's format (an _SRGB one; VK_FORMAT_UNDEFINED before
 * psx_vr_start). The renderer draws the eye views into the matching _UNORM
 * image and copies them over unchanged, since its pixels already hold sRGB
 * values. */
VkFormat psx_vr_screen_format(void);
/* The settings as they are now (the menu changes them on the headset thread). */
void psx_vr_settings_get(PsxVrSettings *out);
/* Get the screen swapchain's next image for a w x h picture (both eyes side
 * by side: the left eye in x 0..w/2). The swapchain is (re)made at that size.
 * The image is in COLOR_ATTACHMENT_OPTIMAL and must be returned in it.
 * 0 = no image this frame (session not running): skip the frame. */
int  psx_vr_frame_acquire(int w, int h, VkImage *out_image);
/* Hand the image back once the commands that write it are submitted. aspect
 * is the picture's width / height (one eye), which sizes the screen. */
void psx_vr_frame_release(float aspect);

/* ---- input (game thread) ------------------------------------------------ */
/* 1 while the headset session has input focus (the controllers then drive
 * pad 1). */
int  psx_vr_controllers_active(void);
/* Pad 1's button word from the controllers (active low; 0xFFFF while the VR
 * menu is open, so navigating it does not reach the game). */
uint16_t psx_vr_pad_word(void);
/* 1 while the VR menu is shown. */
int  psx_vr_menu_open(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_VR_OPENXR_H */
