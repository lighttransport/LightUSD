/*
 * lightui.c — Library lifecycle; thin dispatch to the platform backend
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <lightui/lightui.h>
#include "platform/platform_internal.h"

#include <stddef.h>

/* ---- Lifecycle ---------------------------------------------------------- */

bool lui_init(void)
{
    return lui_platform_ops.init();
}

void lui_shutdown(void)
{
    lui_platform_ops.shutdown();
}

const char *lui_version(void)
{
    return LIGHTUI_VERSION_STRING;
}

/* lui_log_set_handler / lui_log__emit live in src/internal/lui_log.c so
 * they can be linked into headless test binaries without dragging in the
 * platform layer. */

/* ---- Window ------------------------------------------------------------- */

lui_window_t *lui_window_create(const char *title,
                                 int width, int height,
                                 uint32_t flags)
{
    return lui_platform_ops.window_create(title, width, height, flags);
}

void lui_window_destroy(lui_window_t *window)
{
    if (window)
        lui_platform_ops.window_destroy(window);
}

void lui_window_show(lui_window_t *window)
{
    if (window)
        lui_platform_ops.window_show(window);
}

void lui_window_hide(lui_window_t *window)
{
    if (window)
        lui_platform_ops.window_hide(window);
}

void lui_window_set_title(lui_window_t *window, const char *title)
{
    if (window)
        lui_platform_ops.window_set_title(window, title);
}

void lui_window_get_size(const lui_window_t *window, int *width, int *height)
{
    if (window)
        lui_platform_ops.window_get_size(window, width, height);
}

void lui_window_get_physical_size(const lui_window_t *window,
                                   int *width, int *height)
{
    if (window)
        lui_platform_ops.window_get_physical_size(window, width, height);
}

lvg_surface_t *lui_window_get_surface(lui_window_t *window)
{
    if (!window) return NULL;
    return lui_platform_ops.window_get_surface(window);
}

uintptr_t lui_window_native_handle(const lui_window_t *window, lui_native_kind_t *kind)
{
    if (kind) *kind = LUI_NATIVE_NONE;
    if (!window) return 0;
    if (kind) *kind = (lui_native_kind_t)window->native_kind;
    return window->native_window;
}

void lui_window_set_external_render(lui_window_t *window, bool enabled)
{
    if (window) window->external_render = enabled;
}

void lui_window_present(lui_window_t *window)
{
    if (window && !window->external_render)
        lui_platform_ops.window_present(window);
}

void lui_window_present_rect(lui_window_t *window, const lvg_rect_t *dirty)
{
    if (!window || window->external_render) return;
    /* Use the partial-present path when the backend provides one and the
     * caller gave a non-empty rect; otherwise fall back to a full present. */
    if (dirty && !lvg_rect_is_empty(dirty) &&
        lui_platform_ops.window_present_rect) {
        lui_platform_ops.window_present_rect(window, dirty);
    } else {
        lui_platform_ops.window_present(window);
    }
}

bool lui_window_poll_event(lui_window_t *window, lui_event_t *event)
{
    if (!window || !event) return false;
    return lui_platform_ops.window_poll_event(window, event);
}

void lui_window_set_file_drop(lui_window_t *window, bool enabled)
{
    if (window && lui_platform_ops.window_set_file_drop)
        lui_platform_ops.window_set_file_drop(window, enabled);
}

char *lui_window_take_dropped_file(lui_window_t *window)
{
    if (!window || !lui_platform_ops.window_take_dropped_file)
        return NULL;
    return lui_platform_ops.window_take_dropped_file(window);
}

bool lui_window_wait_event(lui_window_t *window, lui_event_t *event)
{
    if (!window || !event) return false;
    return lui_platform_ops.window_wait_event(window, event);
}
