/*
 * platform_internal.h — Internal platform abstraction
 *
 * This header is NOT part of the public API.  It is included only by
 * platform backend files and by lightui.c.
 *
 * Each platform backend provides a statically-initialised
 * lui_platform_ops_t constant named `lui_platform_ops`, which
 * lightui.c references directly.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LUI_PLATFORM_INTERNAL_H
#define LUI_PLATFORM_INTERNAL_H

#include <lightui/lightui.h>

/* -------------------------------------------------------------------------
 * Concrete definition of the opaque lui_window_s type.
 *
 * Platform backends extend this by embedding it as the first member of a
 * larger struct, then casting to/from lui_window_t *.
 * ------------------------------------------------------------------------- */
struct lui_window_s {
    bool           external_render; /* caller owns the native surface */
    uintptr_t      native_window;   /* HWND on Win32, 0 if unavailable */
    int            native_kind;     /* lui_native_kind_t               */
    lvg_surface_t  surface;       /* backing pixel buffer               */
    int            logical_w;     /* logical (CSS-pixel) width          */
    int            logical_h;     /* logical height                     */
    float          dpi_scale;     /* physical / logical ratio           */
    bool           should_close;  /* set when OS requests close         */
};

/* -------------------------------------------------------------------------
 * Platform operations table
 * ------------------------------------------------------------------------- */
typedef struct {
    /** One-time initialisation (e.g. XOpenDisplay, NSApplicationMain). */
    bool (*init)(void);

    /** Release all OS-level resources acquired by init(). */
    void (*shutdown)(void);

    /* ---- Window --------------------------------------------------------- */
    lui_window_t *(*window_create)(const char *title,
                                    int w, int h, uint32_t flags);
    void          (*window_destroy)(lui_window_t *win);
    void          (*window_show)(lui_window_t *win);
    void          (*window_hide)(lui_window_t *win);
    void          (*window_set_title)(lui_window_t *win, const char *title);
    void          (*window_get_size)(const lui_window_t *win,
                                      int *w, int *h);
    void          (*window_get_physical_size)(const lui_window_t *win,
                                               int *w, int *h);
    lvg_surface_t *(*window_get_surface)(lui_window_t *win);
    void           (*window_present)(lui_window_t *win);
    /** Optional: present only @dirty (physical coords). NULL => use
     *  window_present (full present) as a fallback. */
    void           (*window_present_rect)(lui_window_t *win,
                                          const lvg_rect_t *dirty);
    bool           (*window_poll_event)(lui_window_t *win, lui_event_t *ev);
    bool           (*window_wait_event)(lui_window_t *win, lui_event_t *ev);
    /** Optional: file drag-and-drop from the OS (NULL => unsupported). */
    void           (*window_set_file_drop)(lui_window_t *win, bool enabled);
    char *         (*window_take_dropped_file)(lui_window_t *win);
} lui_platform_ops_t;

/*
 * Each backend defines this symbol.  lightui.c links against it.
 */
extern const lui_platform_ops_t lui_platform_ops;

#endif /* LUI_PLATFORM_INTERNAL_H */
