#ifndef PSX_VR_SETTINGS_H
#define PSX_VR_SETTINGS_H

/* VR settings, the in-headset settings menu and the controller -> PS1 pad
 * mapping (PSX_VR builds, e.g. the Pico 4 APK). Pure C with no OpenXR or
 * Vulkan in it, so the host tests cover it; vr_openxr.c drives it.
 *
 * The settings live in vr_settings.ini beside the game's data (one key=value
 * per line); the menu writes them back when it closes. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsxVrSettings {
    int stereo;          /* 1 = 3D (two eye views), 0 = the same flat picture in both eyes */
    int strength;        /* depth strength: the far plane's disparity, 0.1 % of the screen width */
    int convergence;     /* where the screen plane sits, % of the scene's reference depth */
    int flat_depth;      /* depth of 2D layers (HUD, sprites): -100 far .. 0 screen .. 100 near */
    int fov;             /* the virtual screen's width as an angle, degrees */
    int distance_cm;     /* the virtual screen's distance */
    int height_cm;       /* the virtual screen's height offset from eye level */
    int follow_head;     /* 1 = the screen stays in front of the head (head-locked) */
    int eye_width;       /* each eye's picture width in pixels (the height follows the aspect) */
    int refresh_hz;      /* headset refresh rate; 0 = the headset's default */
    int swap_eyes;       /* 1 = left and right views swapped */
    int quality;         /* depth search steps per pixel (more = cleaner edges, slower) */
} PsxVrSettings;

void psx_vr_settings_defaults(PsxVrSettings *s);
/* Clamp every field into its range (after loading or a step). */
void psx_vr_settings_clamp(PsxVrSettings *s);
/* 1 when the file was read (missing keys keep their defaults). */
int  psx_vr_settings_load(PsxVrSettings *s, const char *path);
int  psx_vr_settings_save(const PsxVrSettings *s, const char *path);

/* ---- the menu ------------------------------------------------------------- */
typedef enum PsxVrMenuItem {
    PSX_VR_ITEM_RESUME = 0,
    PSX_VR_ITEM_STEREO,
    PSX_VR_ITEM_STRENGTH,
    PSX_VR_ITEM_CONVERGENCE,
    PSX_VR_ITEM_FLAT_DEPTH,
    PSX_VR_ITEM_FOV,
    PSX_VR_ITEM_DISTANCE,
    PSX_VR_ITEM_HEIGHT,
    PSX_VR_ITEM_FOLLOW_HEAD,
    PSX_VR_ITEM_RESOLUTION,
    PSX_VR_ITEM_REFRESH,
    PSX_VR_ITEM_QUALITY,
    PSX_VR_ITEM_SWAP_EYES,
    PSX_VR_ITEM_RECENTER,
    PSX_VR_ITEM_DEFAULTS,
    PSX_VR_ITEM_COUNT
} PsxVrMenuItem;

/* What the headset offers for the refresh rate (XR_FB_display_refresh_rate);
 * count 0 hides the choice. */
typedef struct PsxVrRates {
    int count;
    int hz[8];
} PsxVrRates;

typedef enum PsxVrMenuAction {
    PSX_VR_MENU_NONE = 0,
    PSX_VR_MENU_CHANGED,     /* a setting changed */
    PSX_VR_MENU_CLOSE,       /* Resume */
    PSX_VR_MENU_RECENTER,    /* Recenter view */
} PsxVrMenuAction;

/* Step the item's value by dir (-1 / +1); accept (dir 0) toggles a switch or
 * runs an action. */
PsxVrMenuAction psx_vr_menu_step(PsxVrSettings *s, int item, int dir, const PsxVrRates *rates);
/* The item's label and its value as text (value is "" for actions). */
void psx_vr_menu_text(const PsxVrSettings *s, int item, char *label, int label_cap,
                      char *value, int value_cap);

/* Draw the menu panel into an RGBA8 image (byte order R,G,B,A; sRGB values).
 * selected = highlighted item. */
void psx_vr_menu_draw(const PsxVrSettings *s, int selected, uint8_t *rgba, int w, int h);

/* ---- controllers ----------------------------------------------------------- */
/* What the two controllers hold this frame (OpenXR action state). Sticks are
 * -1..1 with +y up; triggers and grips 0..1. */
typedef struct PsxVrControls {
    int   a, b, x, y;
    int   menu;                     /* left controller's menu button */
    int   stick_click_l, stick_click_r;
    float trigger_l, trigger_r, grip_l, grip_r;
    float stick_lx, stick_ly, stick_rx, stick_ry;
} PsxVrControls;

/* The PS1 pad word (active low, 0xFFFF = nothing held):
 *   left stick -> D-pad, right trigger -> Square (1, LP), B -> Triangle
 *   (2, RP), right grip -> Cross (3, LK), A -> Circle (4, RK), X -> L1,
 *   Y -> R1, left trigger -> L2, left grip -> R2, menu -> Start,
 *   right stick click -> Select. The left stick click opens the VR menu. */
uint16_t psx_vr_pad_word_from(const PsxVrControls *c);

/* Menu navigation from the controllers: edges with auto-repeat on the sticks.
 * Call once per headset frame with the frame's time in seconds. */
typedef enum PsxVrNav {
    PSX_VR_NAV_NONE = 0,
    PSX_VR_NAV_UP, PSX_VR_NAV_DOWN, PSX_VR_NAV_LEFT, PSX_VR_NAV_RIGHT,
    PSX_VR_NAV_ACCEPT, PSX_VR_NAV_BACK, PSX_VR_NAV_TOGGLE,
} PsxVrNav;
typedef struct PsxVrNavState {
    PsxVrControls prev;
    int    held_dir;      /* the stick direction held (PSX_VR_NAV_UP..RIGHT), 0 none */
    double next_repeat;   /* when that direction repeats */
} PsxVrNavState;
PsxVrNav psx_vr_nav(PsxVrNavState *st, const PsxVrControls *c, double now);

#ifdef __cplusplus
}
#endif

#endif /* PSX_VR_SETTINGS_H */
