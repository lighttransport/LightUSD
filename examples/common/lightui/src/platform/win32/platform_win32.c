/*
 * platform_win32.c — Windows platform backend
 *
 * Uses Win32 CreateWindowEx for window management and StretchDIBits for
 * presenting the software-rendered pixel buffer to the screen.
 * A Direct2D / DXGI upgrade path is noted in comments.
 *
 * Compile with: /DLUI_PLATFORM_WIN32 on MSVC or MinGW.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef LUI_PLATFORM_WIN32

/* Require at least Windows Vista for modern API surface */
#ifndef WINVER
#  define WINVER 0x0600
#endif
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0600
#endif

/* Use wide-char Win32 API consistently */
#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif

#include "../platform_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Platform-specific window struct
 * ------------------------------------------------------------------------- */
typedef struct {
    lui_window_t  base;        /* MUST be first */
    HWND          hwnd;
    HDC           hdc;

#define WIN32_EVT_QUEUE_SIZE 128
    lui_event_t   event_queue[WIN32_EVT_QUEUE_SIZE];
    int           evt_head;
    int           evt_tail;

    int           last_mouse_x;
    int           last_mouse_y;

    /* Dropped file paths (UTF-8) waiting for lui_window_take_dropped_file(). */
    char        **drops;
    int           drop_count;
    int           drop_cap;
} lui_window_win32_t;

static void win32_push_event(lui_window_win32_t *ww, const lui_event_t *ev)
{
    int next = (ww->evt_tail + 1) % WIN32_EVT_QUEUE_SIZE;
    if (next == ww->evt_head) return;
    ww->event_queue[ww->evt_tail] = *ev;
    ww->evt_tail = next;
}

/* -------------------------------------------------------------------------
 * Window class name
 * ------------------------------------------------------------------------- */
static const WCHAR *k_class_name = L"LightUIWindow";

/* -------------------------------------------------------------------------
 * Win32 → lui modifier mapping
 * ------------------------------------------------------------------------- */
static uint32_t get_mods(void)
{
    uint32_t mods = 0;
    if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= LUI_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= LUI_MOD_CTRL;
    if (GetKeyState(VK_MENU)    & 0x8000) mods |= LUI_MOD_ALT;
    if (GetKeyState(VK_LWIN)    & 0x8000) mods |= LUI_MOD_SUPER;
    if (GetKeyState(VK_RWIN)    & 0x8000) mods |= LUI_MOD_SUPER;
    return mods;
}

/* -------------------------------------------------------------------------
 * Win32 VK_* -> lui portable key-code mapping
 *
 * lui_event_t.data.key.key is documented (event.h) as X11 keysym-compatible:
 * lowercase ASCII for letters, raw ASCII for digits/punctuation, and
 * 0xFF5x-range codes for navigation keys. Win32 delivers uppercase VK_A..
 * VK_Z and its own VK_UP/VK_HOME/etc. codes, which never match either
 * convention, so every non-digit shortcut and all arrow/Home/End/PageUp/
 * PageDown/Backspace/Tab/Return/Escape handling was silently dead on
 * Windows. Route everything through this table instead of forwarding wp.
 * ------------------------------------------------------------------------- */
static int win32_translate_key(WPARAM vk)
{
    switch (vk) {
    case VK_BACK:   return LUI_KEY_BACKSPACE;
    case VK_TAB:    return LUI_KEY_TAB;
    case VK_RETURN: return LUI_KEY_RETURN;
    case VK_ESCAPE: return LUI_KEY_ESCAPE;
    case VK_HOME:   return LUI_KEY_HOME;
    case VK_LEFT:   return LUI_KEY_LEFT;
    case VK_UP:     return LUI_KEY_UP;
    case VK_RIGHT:  return LUI_KEY_RIGHT;
    case VK_DOWN:   return LUI_KEY_DOWN;
    case VK_PRIOR:  return LUI_KEY_PAGE_UP;
    case VK_NEXT:   return LUI_KEY_PAGE_DOWN;
    case VK_END:    return LUI_KEY_END;
    case VK_INSERT: return LUI_KEY_INSERT;
    case VK_DELETE: return LUI_KEY_DELETE;
    default: break;
    }

    /* Letters, digits, and OEM punctuation: resolve to the unshifted
     * character for the active keyboard layout (so this stays correct on
     * non-US layouts too) and lowercase letters, since callers read shift
     * state from event mods rather than baking it into the key code. */
    UINT ch = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR);
    if (ch >= 32 && ch < 127) {
        if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
        return (int)ch;
    }

    return (int)vk;
}

/* -------------------------------------------------------------------------
 * WndProc
 * ------------------------------------------------------------------------- */
static LRESULT CALLBACK lui_wndproc(HWND hwnd, UINT msg,
                                     WPARAM wp, LPARAM lp)
{
    lui_window_win32_t *ww =
        (lui_window_win32_t *)(LONG_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (!ww) return DefWindowProcW(hwnd, msg, wp, lp);

    lui_event_t ev;

    switch (msg) {
    case WM_CLOSE:
        memset(&ev, 0, sizeof(ev));
        ev.type = LUI_EVENT_QUIT;
        ww->base.should_close = true;
        win32_push_event(ww, &ev);
        return 0;

    case WM_SIZE: {
        int new_w = LOWORD(lp);
        int new_h = HIWORD(lp);
        if (new_w > 0 && new_h > 0 &&
            (new_w != ww->base.logical_w || new_h != ww->base.logical_h)) {
            ww->base.logical_w = new_w;
            ww->base.logical_h = new_h;
            lvg_surface_resize(&ww->base.surface, new_w, new_h);
            ww->base.surface.dpi_scale = ww->base.dpi_scale;
            memset(&ev, 0, sizeof(ev));
            ev.type               = LUI_EVENT_WINDOW_RESIZE;
            ev.data.resize.width  = new_w;
            ev.data.resize.height = new_h;
            ev.data.resize.dpi_scale = ww->base.dpi_scale;
            win32_push_event(ww, &ev);
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (ww->base.external_render) {
            EndPaint(hwnd, &ps);
            return 0;
        }
        /* Present pixel buffer via DIB section */
        int w = ww->base.surface.width;
        int h = ww->base.surface.height;
        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = w;
        bmi.bmiHeader.biHeight      = -h;  /* top-down */
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(hdc,
                      0, 0, ww->base.logical_w, ww->base.logical_h,
                      0, 0, w, h,
                      ww->base.surface.pixels,
                      &bmi, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        memset(&ev, 0, sizeof(ev));
        ev.type           = LUI_EVENT_KEY_DOWN;
        ev.data.key.key      = win32_translate_key(wp);
        ev.data.key.scancode = (uint32_t)((lp >> 16) & 0xFF);
        ev.data.key.mods     = get_mods();
        ev.data.key.repeat   = (lp & (1 << 30)) != 0;
        win32_push_event(ww, &ev);
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        memset(&ev, 0, sizeof(ev));
        ev.type           = LUI_EVENT_KEY_UP;
        ev.data.key.key      = win32_translate_key(wp);
        ev.data.key.scancode = (uint32_t)((lp >> 16) & 0xFF);
        ev.data.key.mods     = get_mods();
        win32_push_event(ww, &ev);
        return 0;

    case WM_CHAR:
        if (wp >= 32 && wp != 127) {
            memset(&ev, 0, sizeof(ev));
            ev.type = LUI_EVENT_TEXT_INPUT;
            /* Encode codepoint as UTF-8 */
            if (wp < 0x80) {
                ev.data.text.text[0] = (char)wp;
            } else if (wp < 0x800) {
                ev.data.text.text[0] = (char)(0xC0 | (wp >> 6));
                ev.data.text.text[1] = (char)(0x80 | (wp & 0x3F));
            } else {
                ev.data.text.text[0] = (char)(0xE0 | (wp >> 12));
                ev.data.text.text[1] = (char)(0x80 | ((wp >> 6) & 0x3F));
                ev.data.text.text[2] = (char)(0x80 | (wp & 0x3F));
            }
            win32_push_event(ww, &ev);
        }
        return 0;

    case WM_MOUSEMOVE: {
        int x = (int)(short)LOWORD(lp);
        int y = (int)(short)HIWORD(lp);
        memset(&ev, 0, sizeof(ev));
        ev.type              = LUI_EVENT_MOUSE_MOVE;
        ev.data.mouse_move.x  = x;
        ev.data.mouse_move.y  = y;
        ev.data.mouse_move.dx = x - ww->last_mouse_x;
        ev.data.mouse_move.dy = y - ww->last_mouse_y;
        ww->last_mouse_x = x;
        ww->last_mouse_y = y;
        win32_push_event(ww, &ev);
        return 0;
    }

    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: {
        bool pressed = (msg == WM_LBUTTONDOWN ||
                        msg == WM_MBUTTONDOWN ||
                        msg == WM_RBUTTONDOWN);
        lui_mouse_button_t btn =
            (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? LUI_MOUSE_LEFT   :
            (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? LUI_MOUSE_RIGHT  :
                                                              LUI_MOUSE_MIDDLE;
        memset(&ev, 0, sizeof(ev));
        ev.type                    = pressed ? LUI_EVENT_MOUSE_DOWN : LUI_EVENT_MOUSE_UP;
        ev.data.mouse_button.x      = (int)(short)LOWORD(lp);
        ev.data.mouse_button.y      = (int)(short)HIWORD(lp);
        ev.data.mouse_button.button = btn;
        ev.data.mouse_button.clicks = 1;
        win32_push_event(ww, &ev);
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        memset(&ev, 0, sizeof(ev));
        ev.type                    = LUI_EVENT_MOUSE_DOWN;
        ev.data.mouse_button.x      = (int)(short)LOWORD(lp);
        ev.data.mouse_button.y      = (int)(short)HIWORD(lp);
        ev.data.mouse_button.button = LUI_MOUSE_LEFT;
        ev.data.mouse_button.clicks = 2;
        win32_push_event(ww, &ev);
        return 0;

    case WM_MOUSEWHEEL: {
        float delta = (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
        memset(&ev, 0, sizeof(ev));
        ev.type              = LUI_EVENT_SCROLL;
        ev.data.scroll.x      = ww->last_mouse_x;
        ev.data.scroll.y      = ww->last_mouse_y;
        ev.data.scroll.delta_y = -delta;  /* positive = scroll down */
        win32_push_event(ww, &ev);
        return 0;
    }

    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFFu, NULL, 0);
        for (UINT i = 0; i < n; ++i) {
            UINT len = DragQueryFileW(drop, i, NULL, 0);
            WCHAR *wide = (WCHAR *)malloc(((size_t)len + 1) * sizeof(WCHAR));
            if (!wide)
                break;
            DragQueryFileW(drop, i, wide, len + 1);
            int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
            char *utf8 = bytes > 0 ? (char *)malloc((size_t)bytes) : NULL;
            if (utf8)
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, bytes, NULL, NULL);
            free(wide);
            if (!utf8)
                continue;
            if (ww->drop_count == ww->drop_cap) {
                int cap = ww->drop_cap ? ww->drop_cap * 2 : 4;
                char **grown = (char **)realloc(ww->drops, (size_t)cap * sizeof(char *));
                if (!grown) {
                    free(utf8);
                    continue;
                }
                ww->drops = grown;
                ww->drop_cap = cap;
            }
            ww->drops[ww->drop_count++] = utf8;
            lui_event_t drop_ev;
            memset(&drop_ev, 0, sizeof(drop_ev));
            drop_ev.type = LUI_EVENT_FILE_DROP;
            win32_push_event(ww, &drop_ev);
        }
        DragFinish(drop);
        return 0;
    }

    case WM_SETFOCUS:
        memset(&ev, 0, sizeof(ev));
        ev.type = LUI_EVENT_WINDOW_FOCUS_IN;
        win32_push_event(ww, &ev);
        return 0;

    case WM_KILLFOCUS:
        memset(&ev, 0, sizeof(ev));
        ev.type = LUI_EVENT_WINDOW_FOCUS_OUT;
        win32_push_event(ww, &ev);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* -------------------------------------------------------------------------
 * Platform ops implementation
 * ------------------------------------------------------------------------- */

static bool win32_init(void)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS | CS_OWNDC;
    wc.lpfnWndProc   = lui_wndproc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = k_class_name;
    if (!RegisterClassExW(&wc)) {
        /* Already registered is OK */
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
    }
    return true;
}

static void win32_shutdown(void)
{
    UnregisterClassW(k_class_name, GetModuleHandleW(NULL));
}

static lui_window_t *win32_window_create(const char *title,
                                          int w, int h,
                                          uint32_t flags)
{
    lui_window_win32_t *ww =
        (lui_window_win32_t *)calloc(1, sizeof(lui_window_win32_t));
    if (!ww) return NULL;

    ww->base.logical_w   = w;
    ww->base.logical_h   = h;
    ww->base.dpi_scale   = 1.0f;  /* TODO: GetDpiForWindow() on Win10+ */
    ww->base.should_close = false;

    if (!lvg_surface_resize(&ww->base.surface, w, h)) {
        free(ww);
        return NULL;
    }
    ww->base.surface.dpi_scale = ww->base.dpi_scale;

    /* Adjust window rect to achieve the desired client area size */
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!(flags & LUI_WINDOW_RESIZABLE))
        style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    if (flags & LUI_WINDOW_BORDERLESS)
        style = WS_POPUP;

    RECT rect = { 0, 0, w, h };
    AdjustWindowRect(&rect, style, FALSE);

    /* Convert title to wide string */
    wchar_t wtitle[256] = {0};
    if (title) MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 255);

    ww->hwnd = CreateWindowExW(
        0,
        k_class_name,
        wtitle,
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        NULL, NULL,
        GetModuleHandleW(NULL),
        NULL
    );
    if (!ww->hwnd) {
        free(ww->base.surface.pixels);
        free(ww);
        return NULL;
    }

    ww->base.native_kind = 3; /* LUI_NATIVE_WIN32 */
    ww->base.native_window = (uintptr_t)ww->hwnd;
    SetWindowLongPtrW(ww->hwnd, GWLP_USERDATA, (LONG_PTR)ww);
    ww->hdc = GetDC(ww->hwnd);

    if (!(flags & LUI_WINDOW_HIDDEN))
        ShowWindow(ww->hwnd, SW_SHOWNORMAL);

    return (lui_window_t *)ww;
}

static void win32_window_destroy(lui_window_t *win)
{
    lui_window_win32_t *ww = (lui_window_win32_t *)win;
    if (!ww) return;
    if (ww->hdc)  ReleaseDC(ww->hwnd, ww->hdc);
    if (ww->hwnd) DestroyWindow(ww->hwnd);
    for (int i = 0; i < ww->drop_count; ++i) free(ww->drops[i]);
    free(ww->drops);
    free(ww->base.surface.pixels);
    ww->base.surface.pixels = NULL;
    free(ww);
}

static void win32_window_show(lui_window_t *win)
{
    lui_window_win32_t *ww = (lui_window_win32_t *)win;
    ShowWindow(ww->hwnd, SW_SHOWNORMAL);
}

static void win32_window_hide(lui_window_t *win)
{
    lui_window_win32_t *ww = (lui_window_win32_t *)win;
    ShowWindow(ww->hwnd, SW_HIDE);
}

static void win32_window_set_title(lui_window_t *win, const char *title)
{
    lui_window_win32_t *ww = (lui_window_win32_t *)win;
    if (!title) return;
    wchar_t wtitle[256] = {0};
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 255);
    SetWindowTextW(ww->hwnd, wtitle);
}

static void win32_window_get_size(const lui_window_t *win, int *w, int *h)
{
    if (w) *w = win->logical_w;
    if (h) *h = win->logical_h;
}

static void win32_window_get_physical_size(const lui_window_t *win,
                                            int *w, int *h)
{
    if (w) *w = win->surface.width;
    if (h) *h = win->surface.height;
}

static lvg_surface_t *win32_window_get_surface(lui_window_t *win)
{
    return &win->surface;
}

static void win32_window_present(lui_window_t *win)
{
    lui_window_win32_t *ww = (lui_window_win32_t *)win;
    /* Trigger WM_PAINT to blit the pixel buffer */
    InvalidateRect(ww->hwnd, NULL, FALSE);
    UpdateWindow(ww->hwnd);
}

static void win32_window_present_rect(lui_window_t *win, const lvg_rect_t *dirty)
{
    lui_window_win32_t *ww = (lui_window_win32_t *)win;
    if (!dirty || dirty->width <= 0 || dirty->height <= 0) {
        win32_window_present(win);
        return;
    }
    /* @dirty is in physical surface pixels; the client area is logical.
     * Map physical -> logical, rounding the box outward so the stretch blit
     * never leaves a 1px seam.  WM_PAINT's BeginPaint clips the HDC to this
     * update region, so the existing full-surface StretchDIBits transfers
     * only these pixels. */
    float scale = ww->base.dpi_scale > 0.0f ? ww->base.dpi_scale : 1.0f;
    RECT rc;
    rc.left   = (LONG)((float)dirty->x / scale);
    rc.top    = (LONG)((float)dirty->y / scale);
    rc.right  = (LONG)(((float)(dirty->x + dirty->width)  / scale) + 0.999f);
    rc.bottom = (LONG)(((float)(dirty->y + dirty->height) / scale) + 0.999f);
    InvalidateRect(ww->hwnd, &rc, FALSE);
    UpdateWindow(ww->hwnd);
}

static bool win32_window_poll_event(lui_window_t *win, lui_event_t *ev)
{
    lui_window_win32_t *ww = (lui_window_win32_t *)win;
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (ww->evt_head == ww->evt_tail) return false;
    *ev = ww->event_queue[ww->evt_head];
    ww->evt_head = (ww->evt_head + 1) % WIN32_EVT_QUEUE_SIZE;
    return true;
}

static bool win32_window_wait_event(lui_window_t *win, lui_event_t *ev)
{
    lui_window_win32_t *ww = (lui_window_win32_t *)win;
    for (;;) {
        MSG msg;
        if (GetMessageW(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (ww->evt_head != ww->evt_tail) {
            *ev = ww->event_queue[ww->evt_head];
            ww->evt_head = (ww->evt_head + 1) % WIN32_EVT_QUEUE_SIZE;
            return true;
        }
    }
}

/* -------------------------------------------------------------------------
 * Platform ops table
 * ------------------------------------------------------------------------- */
static void win32_window_set_file_drop(lui_window_t *win, bool enabled)
{
    lui_window_win32_t *ww = (lui_window_win32_t *)win;
    if (ww && ww->hwnd)
        DragAcceptFiles(ww->hwnd, enabled ? TRUE : FALSE);
}

static char *win32_window_take_dropped_file(lui_window_t *win)
{
    lui_window_win32_t *ww = (lui_window_win32_t *)win;
    if (!ww || ww->drop_count == 0)
        return NULL;
    char *first = ww->drops[0];
    memmove(ww->drops, ww->drops + 1, (size_t)(ww->drop_count - 1) * sizeof(char *));
    --ww->drop_count;
    return first;
}

const lui_platform_ops_t lui_platform_ops = {
    .init                    = win32_init,
    .shutdown                = win32_shutdown,
    .window_create           = win32_window_create,
    .window_destroy          = win32_window_destroy,
    .window_show             = win32_window_show,
    .window_hide             = win32_window_hide,
    .window_set_title        = win32_window_set_title,
    .window_get_size         = win32_window_get_size,
    .window_get_physical_size = win32_window_get_physical_size,
    .window_get_surface      = win32_window_get_surface,
    .window_present          = win32_window_present,
    .window_present_rect     = win32_window_present_rect,
    .window_poll_event       = win32_window_poll_event,
    .window_wait_event       = win32_window_wait_event,
    .window_set_file_drop    = win32_window_set_file_drop,
    .window_take_dropped_file = win32_window_take_dropped_file,
};

#endif /* LUI_PLATFORM_WIN32 */
