/*
 * lightui/event.h — Platform input and window events
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIGHTUI_EVENT_H
#define LIGHTUI_EVENT_H

#include <lightvg/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * lui_event_type_t
 * ------------------------------------------------------------------------- */
typedef enum {
    LUI_EVENT_NONE            = 0,

    /* Window lifecycle */
    LUI_EVENT_QUIT,                  /* user or OS requested close        */
    LUI_EVENT_WINDOW_RESIZE,         /* content area resized              */
    LUI_EVENT_WINDOW_EXPOSE,         /* window content needs repainting   */
    LUI_EVENT_WINDOW_FOCUS_IN,       /* window gained keyboard focus      */
    LUI_EVENT_WINDOW_FOCUS_OUT,      /* window lost keyboard focus        */

    /* Keyboard */
    LUI_EVENT_KEY_DOWN,              /* key pressed (may repeat)          */
    LUI_EVENT_KEY_UP,                /* key released                      */
    LUI_EVENT_TEXT_INPUT,            /* printable text typed (UTF-8)      */

    /* Pointer */
    LUI_EVENT_MOUSE_MOVE,            /* cursor moved                      */
    LUI_EVENT_MOUSE_DOWN,            /* button pressed                    */
    LUI_EVENT_MOUSE_UP,              /* button released                   */
    LUI_EVENT_SCROLL,                /* mouse wheel / trackpad scroll     */

    /* Files dropped from the OS file manager (see lui_window_set_file_drop).
     * One event per file; fetch the path with lui_window_take_dropped_file(). */
    LUI_EVENT_FILE_DROP,
} lui_event_type_t;

/* -------------------------------------------------------------------------
 * Mouse buttons
 * ------------------------------------------------------------------------- */
typedef enum {
    LUI_MOUSE_LEFT   = 1,
    LUI_MOUSE_MIDDLE = 2,
    LUI_MOUSE_RIGHT  = 3,
} lui_mouse_button_t;

/* -------------------------------------------------------------------------
 * Keyboard modifier bitmask
 * ------------------------------------------------------------------------- */
typedef enum {
    LUI_MOD_SHIFT  = 1 << 0,
    LUI_MOD_CTRL   = 1 << 1,
    LUI_MOD_ALT    = 1 << 2,
    LUI_MOD_SUPER  = 1 << 3,  /* Command / Win key */
    LUI_MOD_CAPS   = 1 << 4,
    LUI_MOD_NUM    = 1 << 5,
} lui_mod_flags_t;

/* -------------------------------------------------------------------------
 * Portable key codes (X11 keysym-compatible; backends map to these)
 * ------------------------------------------------------------------------- */
#define LUI_KEY_BACKSPACE  0xFF08
#define LUI_KEY_TAB        0xFF09
#define LUI_KEY_RETURN     0xFF0D
#define LUI_KEY_ESCAPE     0xFF1B
#define LUI_KEY_HOME       0xFF50
#define LUI_KEY_LEFT       0xFF51
#define LUI_KEY_UP         0xFF52
#define LUI_KEY_RIGHT      0xFF53
#define LUI_KEY_DOWN       0xFF54
#define LUI_KEY_PAGE_UP    0xFF55
#define LUI_KEY_PAGE_DOWN  0xFF56
#define LUI_KEY_END        0xFF57
#define LUI_KEY_INSERT     0xFF63
#define LUI_KEY_DELETE     0xFFFF

/* -------------------------------------------------------------------------
 * lui_event_t
 * ------------------------------------------------------------------------- */
typedef struct {
    lui_event_type_t type;

    union {
        /** LUI_EVENT_KEY_DOWN / LUI_EVENT_KEY_UP */
        struct {
            int      key;       /* platform key-code                     */
            uint32_t scancode;  /* hardware scancode                     */
            uint32_t mods;      /* lui_mod_flags_t bitmask               */
            bool     repeat;    /* true when held-down auto-repeat       */
        } key;

        /** LUI_EVENT_TEXT_INPUT — NUL-terminated UTF-8 character(s) */
        struct {
            char text[8];
        } text;

        /** LUI_EVENT_MOUSE_MOVE */
        struct {
            int x, y;    /* position in logical pixels                   */
            int dx, dy;  /* delta from previous position                 */
        } mouse_move;

        /** LUI_EVENT_MOUSE_DOWN / LUI_EVENT_MOUSE_UP */
        struct {
            int                x, y;
            lui_mouse_button_t button;
            int                clicks;  /* 1 = single, 2 = double-click  */
        } mouse_button;

        /** LUI_EVENT_SCROLL */
        struct {
            int   x, y;       /* cursor position in logical pixels       */
            float delta_x;    /* positive = scroll right                 */
            float delta_y;    /* positive = scroll down                  */
        } scroll;

        /** LUI_EVENT_WINDOW_RESIZE */
        struct {
            int   width, height;  /* new logical size in pixels          */
            float dpi_scale;      /* new DPI scale factor                */
        } resize;
    } data;
} lui_event_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIGHTUI_EVENT_H */
