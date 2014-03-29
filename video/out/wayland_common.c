/*
 * This file is part of mpv video player.
 * Copyright © 2008 Kristian Høgsberg
 * Copyright © 2012-2013 Collabora, Ltd.
 * Copyright © 2013 Alexander Preisinger <alexander.preisinger@gmail.com>
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>
#include <limits.h>
#include <assert.h>
#include <poll.h>
#include <unistd.h>

#include <sys/mman.h>
#include <linux/input.h>

#include "config.h"
#include "bstr/bstr.h"
#include "options/options.h"
#include "common/msg.h"
#include "libavutil/common.h"
#include "talloc.h"

#include "wayland_common.h"

#include "vo.h"
#include "aspect.h"
#include "osdep/timer.h"
#include "sub/osd.h"

#include "input/input.h"
#include "input/event.h"
#include "input/keycodes.h"

#define MOD_SHIFT_MASK      0x01
#define MOD_ALT_MASK        0x02
#define MOD_CONTROL_MASK    0x04

static int lookupkey(int key);

static void hide_cursor(struct vo_wayland_state * wl);
static void show_cursor(struct vo_wayland_state * wl);
static void shedule_resize(struct vo_wayland_state *wl,
                           int32_t width,
                           int32_t height);

static void vo_wayland_fullscreen (struct vo *vo);

static const struct mp_keymap keymap[] = {
    // special keys
    {XKB_KEY_Pause,     MP_KEY_PAUSE}, {XKB_KEY_Escape, MP_KEY_ESC},
    {XKB_KEY_BackSpace, MP_KEY_BS},    {XKB_KEY_Tab,    MP_KEY_TAB},
    {XKB_KEY_Return,    MP_KEY_ENTER}, {XKB_KEY_Menu,   MP_KEY_MENU},
    {XKB_KEY_Print,     MP_KEY_PRINT},

    // cursor keys
    {XKB_KEY_Left, MP_KEY_LEFT}, {XKB_KEY_Right, MP_KEY_RIGHT},
    {XKB_KEY_Up,   MP_KEY_UP},   {XKB_KEY_Down,  MP_KEY_DOWN},

    // navigation block
    {XKB_KEY_Insert,  MP_KEY_INSERT},  {XKB_KEY_Delete,    MP_KEY_DELETE},
    {XKB_KEY_Home,    MP_KEY_HOME},    {XKB_KEY_End,       MP_KEY_END},
    {XKB_KEY_Page_Up, MP_KEY_PAGE_UP}, {XKB_KEY_Page_Down, MP_KEY_PAGE_DOWN},

    // F-keys
    {XKB_KEY_F1,  MP_KEY_F+1},  {XKB_KEY_F2,  MP_KEY_F+2},
    {XKB_KEY_F3,  MP_KEY_F+3},  {XKB_KEY_F4,  MP_KEY_F+4},
    {XKB_KEY_F5,  MP_KEY_F+5},  {XKB_KEY_F6,  MP_KEY_F+6},
    {XKB_KEY_F7,  MP_KEY_F+7},  {XKB_KEY_F8,  MP_KEY_F+8},
    {XKB_KEY_F9,  MP_KEY_F+9},  {XKB_KEY_F10, MP_KEY_F+10},
    {XKB_KEY_F11, MP_KEY_F+11}, {XKB_KEY_F12, MP_KEY_F+12},

    // numpad independent of numlock
    {XKB_KEY_KP_Subtract, '-'}, {XKB_KEY_KP_Add, '+'},
    {XKB_KEY_KP_Multiply, '*'}, {XKB_KEY_KP_Divide, '/'},
    {XKB_KEY_KP_Enter, MP_KEY_KPENTER},

    // numpad with numlock
    {XKB_KEY_KP_0, MP_KEY_KP0}, {XKB_KEY_KP_1, MP_KEY_KP1},
    {XKB_KEY_KP_2, MP_KEY_KP2}, {XKB_KEY_KP_3, MP_KEY_KP3},
    {XKB_KEY_KP_4, MP_KEY_KP4}, {XKB_KEY_KP_5, MP_KEY_KP5},
    {XKB_KEY_KP_6, MP_KEY_KP6}, {XKB_KEY_KP_7, MP_KEY_KP7},
    {XKB_KEY_KP_8, MP_KEY_KP8}, {XKB_KEY_KP_9, MP_KEY_KP9},
    {XKB_KEY_KP_Decimal, MP_KEY_KPDEC}, {XKB_KEY_KP_Separator, MP_KEY_KPDEC},

    // numpad without numlock
    {XKB_KEY_KP_Insert, MP_KEY_KPINS}, {XKB_KEY_KP_End,       MP_KEY_KP1},
    {XKB_KEY_KP_Down,   MP_KEY_KP2},   {XKB_KEY_KP_Page_Down, MP_KEY_KP3},
    {XKB_KEY_KP_Left,   MP_KEY_KP4},   {XKB_KEY_KP_Begin,     MP_KEY_KP5},
    {XKB_KEY_KP_Right,  MP_KEY_KP6},   {XKB_KEY_KP_Home,      MP_KEY_KP7},
    {XKB_KEY_KP_Up,     MP_KEY_KP8},   {XKB_KEY_KP_Page_Up,   MP_KEY_KP9},
    {XKB_KEY_KP_Delete, MP_KEY_KPDEL},

    {0, 0}
};


/** Wayland listeners **/

static void display_handle_error(void *data,
                                 struct wl_display *display,
                                 void *object_id,
                                 uint32_t code,
                                 const char *message)
{
    struct vo_wayland_state *wl = data;
    const char * error_type_msg = "";

    switch (code) {
    case WL_DISPLAY_ERROR_INVALID_OBJECT:
        error_type_msg = "Invalid object";
        break;
    case WL_DISPLAY_ERROR_INVALID_METHOD:
        error_type_msg = "Invalid method";
        break;
    case WL_DISPLAY_ERROR_NO_MEMORY:
        error_type_msg = "No memory";
        break;
    }

    MP_ERR(wl, "%s: %s\n", error_type_msg, message);
}

static void display_handle_delete_id(void *data,
                                     struct wl_display *display,
                                     uint32_t id)
{
    struct vo_wayland_state *wl = data;
    MP_DBG(wl, "Object %u deleted\n", id);
}

static const struct wl_display_listener display_listener = {
    display_handle_error,
    display_handle_delete_id
};

static void xdg_handle_ping(void *data,
                            struct xdg_shell *shell,
                            uint32_t serial)
{
    xdg_shell_pong(shell, serial);
}

static const struct xdg_shell_listener shell_listener = {
    xdg_handle_ping,
};

static void xdg_handle_configure(void *data,
                                 struct xdg_surface *surface,
                                 int32_t width,
                                 int32_t height)
{
    struct vo_wayland_state *wl = data;
    shedule_resize(wl, width, height);
}

static void xdg_handle_change_state(void *data,
                                    struct xdg_surface *surface,
                                    uint32_t state,
                                    uint32_t value,
                                    uint32_t serial)
{
    struct vo_wayland_state *wl = data;
    switch (state) {
        case XDG_SURFACE_STATE_MAXIMIZED:
            break;
        case XDG_SURFACE_STATE_FULLSCREEN:
            wl->window.is_fullscreen = !!value;
            break;
        default:
            break;
    }
    xdg_surface_ack_change_state(surface, state, value, serial);
    // compositor invoked fullscreen request
}


static void xdg_handle_activated(void *data, struct xdg_surface *surface)
{
    struct vo_wayland_state *wl = data;
    wl->window.has_focus = true;
}

static void xdg_handle_deactivated(void *data, struct xdg_surface *surface)
{
    struct vo_wayland_state *wl = data;
    wl->window.has_focus = false;
}

static void xdg_handle_delete(void *data, struct xdg_surface *surface)
{
    struct vo_wayland_state *wl = data;
    mp_input_put_key(wl->vo->input_ctx, MP_KEY_CLOSE_WIN);
}

const struct xdg_surface_listener xdg_surface_listener = {
    xdg_handle_configure,
    xdg_handle_change_state,
    xdg_handle_activated,
    xdg_handle_deactivated,
    xdg_handle_delete,
};

static void output_handle_geometry(void *data,
                                   struct wl_output *wl_output,
                                   int32_t x,
                                   int32_t y,
                                   int32_t physical_width,
                                   int32_t physical_height,
                                   int32_t subpixel,
                                   const char *make,
                                   const char *model,
                                   int32_t transform)
{
    /* Ignore transforms for now */
    switch (transform) {
        case WL_OUTPUT_TRANSFORM_NORMAL:
        case WL_OUTPUT_TRANSFORM_90:
        case WL_OUTPUT_TRANSFORM_180:
        case WL_OUTPUT_TRANSFORM_270:
        case WL_OUTPUT_TRANSFORM_FLIPPED:
        case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        default:
            break;
    }
}

static void output_handle_mode(void *data,
                               struct wl_output *wl_output,
                               uint32_t flags,
                               int32_t width,
                               int32_t height,
                               int32_t refresh)
{
    struct vo_wayland_output *output = data;

    if (!output)
        return;

    output->width = width;
    output->height = height;
    output->flags = flags;
}

static const struct wl_output_listener output_listener = {
    output_handle_geometry,
    output_handle_mode
};

/* KEYBOARD LISTENER */
static void keyboard_handle_keymap(void *data,
                                   struct wl_keyboard *wl_keyboard,
                                   uint32_t format,
                                   int32_t fd,
                                   uint32_t size)
{
    struct vo_wayland_state *wl = data;
    char *map_str;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    map_str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (map_str == MAP_FAILED) {
        close(fd);
        return;
    }

    wl->input.xkb.keymap = xkb_keymap_new_from_string(wl->input.xkb.context,
                                                      map_str,
                                                      XKB_KEYMAP_FORMAT_TEXT_V1,
                                                      0);

    munmap(map_str, size);
    close(fd);

    if (!wl->input.xkb.keymap) {
        MP_ERR(wl, "failed to compile keymap\n");
        return;
    }

    wl->input.xkb.state = xkb_state_new(wl->input.xkb.keymap);
    if (!wl->input.xkb.state) {
        MP_ERR(wl, "failed to create XKB state\n");
        xkb_map_unref(wl->input.xkb.keymap);
        wl->input.xkb.keymap = NULL;
        return;
    }
}

static void keyboard_handle_enter(void *data,
                                  struct wl_keyboard *wl_keyboard,
                                  uint32_t serial,
                                  struct wl_surface *surface,
                                  struct wl_array *keys)
{
}

static void keyboard_handle_leave(void *data,
                                  struct wl_keyboard *wl_keyboard,
                                  uint32_t serial,
                                  struct wl_surface *surface)
{
}

static void keyboard_handle_key(void *data,
                                struct wl_keyboard *wl_keyboard,
                                uint32_t serial,
                                uint32_t time,
                                uint32_t key,
                                uint32_t state)
{
    struct vo_wayland_state *wl = data;
    uint32_t code, num_syms;
    int mpkey;

    const xkb_keysym_t *syms;
    xkb_keysym_t sym;

    code = key + 8;
    num_syms = xkb_key_get_syms(wl->input.xkb.state, code, &syms);

    sym = XKB_KEY_NoSymbol;
    if (num_syms == 1)
        sym = syms[0];

    if (sym != XKB_KEY_NoSymbol && (mpkey = lookupkey(sym))) {
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
            mp_input_put_key(wl->vo->input_ctx, mpkey | MP_KEY_STATE_DOWN);
        else
            mp_input_put_key(wl->vo->input_ctx, mpkey | MP_KEY_STATE_UP);
    }
}

static void keyboard_handle_modifiers(void *data,
                                      struct wl_keyboard *wl_keyboard,
                                      uint32_t serial,
                                      uint32_t mods_depressed,
                                      uint32_t mods_latched,
                                      uint32_t mods_locked,
                                      uint32_t group)
{
    struct vo_wayland_state *wl = data;

    xkb_state_update_mask(wl->input.xkb.state,
                          mods_depressed,
                          mods_latched,
                          mods_locked,
                          0, 0, group);
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_handle_keymap,
    keyboard_handle_enter,
    keyboard_handle_leave,
    keyboard_handle_key,
    keyboard_handle_modifiers
};

/* POINTER LISTENER */
static void pointer_handle_enter(void *data,
                                 struct wl_pointer *pointer,
                                 uint32_t serial,
                                 struct wl_surface *surface,
                                 wl_fixed_t sx_w,
                                 wl_fixed_t sy_w)
{
    struct vo_wayland_state *wl = data;

    wl->cursor.serial = serial;
    wl->cursor.pointer = pointer;

    /* Release the left button on pointer enter again
     * because after moving the shell surface no release event is sent */
    mp_input_put_key(wl->vo->input_ctx, MP_MOUSE_BTN0 | MP_KEY_STATE_UP);
    show_cursor(wl);
}

static void pointer_handle_leave(void *data,
                                 struct wl_pointer *pointer,
                                 uint32_t serial,
                                 struct wl_surface *surface)
{
    struct vo_wayland_state *wl = data;
    mp_input_put_key(wl->vo->input_ctx, MP_KEY_MOUSE_LEAVE);
}

static void pointer_handle_motion(void *data,
                                  struct wl_pointer *pointer,
                                  uint32_t time,
                                  wl_fixed_t sx_w,
                                  wl_fixed_t sy_w)
{
    struct vo_wayland_state *wl = data;

    wl->cursor.pointer = pointer;
    wl->window.mouse_x = wl_fixed_to_int(sx_w);
    wl->window.mouse_y = wl_fixed_to_int(sy_w);

    vo_mouse_movement(wl->vo, wl->window.mouse_x,
                              wl->window.mouse_y);
}

static void pointer_handle_button(void *data,
                                  struct wl_pointer *pointer,
                                  uint32_t serial,
                                  uint32_t time,
                                  uint32_t button,
                                  uint32_t state)
{
    struct vo_wayland_state *wl = data;

    mp_input_put_key(wl->vo->input_ctx, MP_MOUSE_BTN0 + (button - BTN_LEFT) |
                    ((state == WL_POINTER_BUTTON_STATE_PRESSED)
                    ? MP_KEY_STATE_DOWN : MP_KEY_STATE_UP));

    if (!mp_input_test_dragging(wl->vo->input_ctx, wl->window.mouse_x, wl->window.mouse_y) &&
        (button == BTN_LEFT) && (state == WL_POINTER_BUTTON_STATE_PRESSED))
        xdg_surface_move(wl->window.xdg_surface, wl->input.seat, serial);
}

static void pointer_handle_axis(void *data,
                                struct wl_pointer *pointer,
                                uint32_t time,
                                uint32_t axis,
                                wl_fixed_t value)
{
    struct vo_wayland_state *wl = data;

    // value is 10.00 on a normal mouse wheel
    // scale it down to 1.00 for multipliying it with the commands
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        if (value > 0)
            mp_input_put_axis(wl->vo->input_ctx, MP_AXIS_DOWN,
                    wl_fixed_to_double(value)*0.1);
        if (value < 0)
            mp_input_put_axis(wl->vo->input_ctx, MP_AXIS_UP,
                    wl_fixed_to_double(value)*-0.1);
    }
    else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        if (value > 0)
            mp_input_put_axis(wl->vo->input_ctx, MP_AXIS_RIGHT,
                    wl_fixed_to_double(value)*0.1);
        if (value < 0)
            mp_input_put_axis(wl->vo->input_ctx, MP_AXIS_LEFT,
                    wl_fixed_to_double(value)*-0.1);
    }
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
};

static void seat_handle_capabilities(void *data,
                                     struct wl_seat *seat,
                                     enum wl_seat_capability caps)
{
    struct vo_wayland_state *wl = data;

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl->input.keyboard) {
        wl->input.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(wl->input.keyboard, &keyboard_listener, wl);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && wl->input.keyboard) {
        wl_keyboard_destroy(wl->input.keyboard);
        wl->input.keyboard = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl->input.pointer) {
        wl->input.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(wl->input.pointer, &pointer_listener, wl);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && wl->input.pointer) {
        wl_pointer_destroy(wl->input.pointer);
        wl->input.pointer = NULL;
    }
}

static const struct wl_seat_listener seat_listener = {
    seat_handle_capabilities,
};

static void data_offer_handle_offer(void *data,
                                    struct wl_data_offer *offer,
                                    const char *mime_type)
{
    struct vo_wayland_state *wl = data;
    if (strcmp(mime_type, "text/uri-list") != 0)
        MP_VERBOSE(wl, "unsupported mime type for drag and drop: %s\n", mime_type);
}

static const struct wl_data_offer_listener data_offer_listener = {
    data_offer_handle_offer,
};

static void data_device_handle_data_offer(void *data,
                                          struct wl_data_device *wl_data_device,
                                          struct wl_data_offer *id)
{
    struct vo_wayland_state *wl = data;
    if (wl->input.offer) {
        MP_ERR(wl, "There is already a dnd entry point.\n");
        wl_data_offer_destroy(wl->input.offer);
    }

    wl->input.offer = id;
    wl_data_offer_add_listener(id, &data_offer_listener, wl);
}

static void data_device_handle_enter(void *data,
                                     struct wl_data_device *wl_data_device,
                                     uint32_t serial,
                                     struct wl_surface *surface,
                                     wl_fixed_t x,
                                     wl_fixed_t y,
                                     struct wl_data_offer *id)
{
    struct vo_wayland_state *wl = data;
    if (wl->input.offer != id)
        MP_FATAL(wl, "Fatal dnd error (Please report this issue)\n");

    wl_data_offer_accept(id, serial, "text/uri-list");
}

static void data_device_handle_leave(void *data,
                                     struct wl_data_device *wl_data_device)
{
    struct vo_wayland_state *wl = data;
    if (wl->input.offer) {
        wl_data_offer_destroy(wl->input.offer);
        wl->input.offer = NULL;
    }
    // dnd fd is closed on POLLHUP
}

static void data_device_handle_motion(void *data,
                                      struct wl_data_device *wl_data_device,
                                      uint32_t time,
                                      wl_fixed_t x,
                                      wl_fixed_t y)
{
}

static void data_device_handle_drop(void *data,
                                    struct wl_data_device *wl_data_device)
{
    struct vo_wayland_state *wl = data;

    int pipefd[2];

    if (pipe(pipefd) == -1) {
        MP_FATAL(wl, "can't create pipe for dnd communication\n");
        return;
    }

    wl->input.dnd_fd = pipefd[0];
    wl_data_offer_receive(wl->input.offer, "text/uri-list", pipefd[1]);
    close(pipefd[1]);
}

static void data_device_handle_selection(void *data,
                                         struct wl_data_device *wl_data_device,
                                         struct wl_data_offer *id)
{
}

static const struct wl_data_device_listener data_device_listener = {
    data_device_handle_data_offer,
    data_device_handle_enter,
    data_device_handle_leave,
    data_device_handle_motion,
    data_device_handle_drop,
    data_device_handle_selection
};

static void registry_handle_global (void *data,
                                    struct wl_registry *reg,
                                    uint32_t id,
                                    const char *interface,
                                    uint32_t version)
{
    struct vo_wayland_state *wl = data;

    if (strcmp(interface, "wl_compositor") == 0) {

        wl->display.compositor = wl_registry_bind(reg, id,
                                                  &wl_compositor_interface, 1);
    }

    else if (strcmp(interface, "wl_shm") == 0) {

        wl->display.shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
    }

    else if (strcmp(interface, "wl_output") == 0) {

        struct vo_wayland_output *output =
            talloc_zero(wl, struct vo_wayland_output);

        output->id = id;
        output->output = wl_registry_bind(reg, id, &wl_output_interface, 1);

        wl_output_add_listener(output->output, &output_listener, output);
        wl_list_insert(&wl->display.output_list, &output->link);
    }

    else if (strcmp(interface, "wl_data_device_manager") == 0) {

        wl->input.devman = wl_registry_bind(reg,
                                            id,
                                            &wl_data_device_manager_interface,
                                            1);
    }

    else if (strcmp(interface, "wl_seat") == 0) {

        wl->input.seat = wl_registry_bind(reg, id, &wl_seat_interface, 1);
        wl_seat_add_listener(wl->input.seat, &seat_listener, wl);

        wl->input.datadev = wl_data_device_manager_get_data_device(
                wl->input.devman, wl->input.seat);
        wl_data_device_add_listener(wl->input.datadev, &data_device_listener, wl);
    }

    else if (strcmp(interface, "xdg_shell") == 0) {

        wl->display.shell = wl_registry_bind(reg, id, &xdg_shell_interface, 1);
        xdg_shell_add_listener(wl->display.shell, &shell_listener, wl);
        xdg_shell_use_unstable_version(wl->display.shell,
                                       XDG_SHELL_VERSION_CURRENT);
    }

}

static void registry_handle_global_remove (void *data,
                                           struct wl_registry *registry,
                                           uint32_t id)
{
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};


/*** internal functions ***/

static int lookupkey(int key)
{
    static const char *passthrough_keys
        = " -+*/<>`~!@#$%^&()_{}:;\"\',.?\\|=[]";

    int mpkey = 0;
    if ((key >= 'a' && key <= 'z') ||
        (key >= 'A' && key <= 'Z') ||
        (key >= '0' && key <= '9') ||
        (key >  0   && key <  256 && strchr(passthrough_keys, key)))
        mpkey = key;

    if (!mpkey)
        mpkey = lookup_keymap_table(keymap, key);

    return mpkey;
}

static void hide_cursor (struct vo_wayland_state *wl)
{
    if (!wl->cursor.pointer)
        return;

    wl_pointer_set_cursor(wl->cursor.pointer, wl->cursor.serial, NULL, 0, 0);
}

static void show_cursor (struct vo_wayland_state *wl)
{
    if (!wl->cursor.pointer)
        return;

    struct wl_cursor_image *image  = wl->cursor.default_cursor->images[0];
    struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);

    wl_pointer_set_cursor(wl->cursor.pointer,
                          wl->cursor.serial,
                          wl->cursor.surface,
                          image->hotspot_x,
                          image->hotspot_y);

    wl_surface_attach(wl->cursor.surface, buffer, 0, 0);
    wl_surface_damage(wl->cursor.surface, 0, 0, image->width, image->height);
    wl_surface_commit(wl->cursor.surface);
}

static void shedule_resize(struct vo_wayland_state *wl,
                           int32_t width,
                           int32_t height)
{
    MP_DBG(wl, "shedule resize: %dx%d\n", width, height);
    wl->vo->dwidth = width;
    wl->vo->dheight = height;
    struct mp_rect src, dst;
    struct mp_osd_res osd;
    vo_get_src_dst_rects(wl->vo, &src, &dst, &osd);


    wl->window.sh_width = dst.x1 - dst.x0;
    wl->window.sh_height = dst.y1 - dst.y0;
    wl->window.events |= VO_EVENT_RESIZE;
}

static bool create_display (struct vo_wayland_state *wl)
{
    if (wl->vo->probing && !getenv("XDG_RUNTIME_DIR"))
        return false;

    wl->display.display = wl_display_connect(NULL);

    if (!wl->display.display) {
        MP_MSG(wl, wl->vo->probing ? MSGL_V : MSGL_ERR,
               "failed to connect to a wayland server: "
               "check if a wayland compositor is running\n");

        return false;
    }

    wl_display_add_listener(wl->display.display, &display_listener, wl);

    wl->display.registry = wl_display_get_registry(wl->display.display);
    wl_registry_add_listener(wl->display.registry, &registry_listener, wl);

    wl_display_dispatch(wl->display.display);

    wl->display.display_fd = wl_display_get_fd(wl->display.display);

    printf("DISPLAY_FD: %d\n", wl->display.display_fd);

    return true;
}

static void destroy_display (struct vo_wayland_state *wl)
{
    struct vo_wayland_output *output = NULL;
    struct vo_wayland_output *tmp = NULL;

    wl_list_for_each_safe(output, tmp, &wl->display.output_list, link) {
        if (output && output->output) {
            wl_output_destroy(output->output);
            output->output = NULL;
            wl_list_remove(&output->link);
        }
    }

    if (wl->display.shm)
        wl_shm_destroy(wl->display.shm);

    if (wl->display.shell)
        xdg_shell_destroy(wl->display.shell);

    if (wl->display.compositor)
        wl_compositor_destroy(wl->display.compositor);

    if (wl->display.registry)
        wl_registry_destroy(wl->display.registry);

    if (wl->display.display) {
        wl_display_flush(wl->display.display);
        wl_display_disconnect(wl->display.display);
    }
}

static bool create_window (struct vo_wayland_state *wl)
{
    wl->window.surface = wl_compositor_create_surface(wl->display.compositor);
    wl->window.xdg_surface = xdg_shell_get_xdg_surface(wl->display.shell,
                                                       wl->window.surface);

    if (!wl->window.xdg_surface) {
        MP_ERR(wl, "creating xdg surface failed\n");
        return false;
    }

    xdg_surface_add_listener(wl->window.xdg_surface, &xdg_surface_listener, wl);
    xdg_surface_set_app_id(wl->window.xdg_surface, "mpv");

    return true;
}

static void destroy_window (struct vo_wayland_state *wl)
{
    if (wl->window.xdg_surface)
        xdg_surface_destroy(wl->window.xdg_surface);

    if (wl->window.surface)
        wl_surface_destroy(wl->window.surface);
}

static bool create_cursor (struct vo_wayland_state *wl)
{
    if (!wl->display.shm) {
        MP_ERR(wl->vo, "no shm interface available\n");
        return false;
    }

    wl->cursor.surface =
        wl_compositor_create_surface(wl->display.compositor);

    if (!wl->cursor.surface)
        return false;

    wl->cursor.theme = wl_cursor_theme_load(NULL, 32, wl->display.shm);
    wl->cursor.default_cursor = wl_cursor_theme_get_cursor(wl->cursor.theme,
                                                           "left_ptr");
    return true;
}

static void destroy_cursor (struct vo_wayland_state *wl)
{
    if (wl->cursor.theme)
        wl_cursor_theme_destroy(wl->cursor.theme);

    if (wl->cursor.surface)
        wl_surface_destroy(wl->cursor.surface);
}

static bool create_input (struct vo_wayland_state *wl)
{
    wl->input.xkb.context = xkb_context_new(0);

    if (!wl->input.xkb.context) {
        MP_ERR(wl, "failed to initialize input: check xkbcommon\n");
        return false;
    }

    wl->input.dnd_fd = -1;

    return true;
}

static void destroy_input (struct vo_wayland_state *wl)
{
    if (wl->input.keyboard) {
        wl_keyboard_destroy(wl->input.keyboard);
        xkb_map_unref(wl->input.xkb.keymap);
        xkb_state_unref(wl->input.xkb.state);
    }

    if (wl->input.xkb.context)
        xkb_context_unref(wl->input.xkb.context);

    if (wl->input.pointer)
        wl_pointer_destroy(wl->input.pointer);

    if (wl->input.datadev)
        wl_data_device_destroy(wl->input.datadev);

    if (wl->input.devman)
        wl_data_device_manager_destroy(wl->input.devman);

    if (wl->input.seat)
        wl_seat_destroy(wl->input.seat);
}

/*** mplayer2 interface ***/

int vo_wayland_init (struct vo *vo)
{
    vo->wayland = talloc_zero(NULL, struct vo_wayland_state);
    struct vo_wayland_state *wl = vo->wayland;
    wl->vo = vo;
    wl->log = mp_log_new(wl, vo->log, "wayland");

    wl_list_init(&wl->display.output_list);

    if (!create_input(wl)
        || !create_display(wl)
        || !create_window(wl)
        || !create_cursor(wl))
    {
        vo_wayland_uninit(vo);
        return false;
    }

    vo->event_fd = wl->display.display_fd;

    return true;
}

void vo_wayland_uninit (struct vo *vo)
{
    struct vo_wayland_state *wl = vo->wayland;
    destroy_cursor(wl);
    destroy_window(wl);
    destroy_display(wl);
    destroy_input(wl);
    talloc_free(wl);
    vo->wayland = NULL;
}

static void vo_wayland_ontop (struct vo *vo)
{
    struct vo_wayland_state *wl = vo->wayland;
    MP_DBG(wl, "going ontop\n");
    vo->opts->ontop = 1;
    xdg_surface_request_change_state(wl->window.xdg_surface,
                                     XDG_SURFACE_STATE_FULLSCREEN,
                                     false, 0);
    shedule_resize(wl, wl->window.width, wl->window.height);
}

static void vo_wayland_border (struct vo *vo)
{
    /* wayland clienst have to do the decorations themself
     * (client side decorations) but there is no such code implement nor
     * do I plan on implementing something like client side decorations
     *
     * The only exception would be resizing on when clicking and dragging
     * on the border region of the window but this should be discussed at first
     */
}

static void vo_wayland_fullscreen (struct vo *vo)
{
    struct vo_wayland_state *wl = vo->wayland;
    if (!wl->display.shell)
        return;


    if (vo->opts->fullscreen) {
        MP_DBG(wl, "going fullscreen\n");
        wl->window.is_fullscreen = true;
        wl->window.p_width = wl->window.width;
        wl->window.p_height = wl->window.height;
        xdg_surface_request_change_state(wl->window.xdg_surface,
                                         XDG_SURFACE_STATE_FULLSCREEN,
                                         true, 0);
    }

    else {
        MP_DBG(wl, "leaving fullscreen\n");
        wl->window.is_fullscreen = false;
        xdg_surface_request_change_state(wl->window.xdg_surface,
                                         XDG_SURFACE_STATE_FULLSCREEN,
                                         false, 0);
        shedule_resize(wl, wl->window.p_width, wl->window.p_height);
    }
}

static int vo_wayland_check_events (struct vo *vo)
{
    struct vo_wayland_state *wl = vo->wayland;
    struct wl_display *dp = wl->display.display;

    wl_display_dispatch_pending(dp);
    wl_display_flush(dp);

    struct pollfd fd = {
        wl->display.display_fd,
        POLLIN | POLLOUT | POLLERR | POLLHUP,
        0
    };

    /* wl_display_dispatch is blocking
     * wl_dipslay_dispatch_pending is non-blocking but does not read from the fd
     *
     * when pausing no input events get queued so we have to check if there
     * are events to read from the file descriptor through poll */
    if (poll(&fd, 1, 0) > 0) {
        if (fd.revents & POLLERR || fd.revents & POLLHUP) {
            MP_FATAL(wl, "error occurred on the display fd: "
                         "closing file descriptor\n");
            close(wl->display.display_fd);
            mp_input_put_key(vo->input_ctx, MP_KEY_CLOSE_WIN);
        }
        if (fd.revents & POLLIN)
            wl_display_dispatch(dp);
        if (fd.revents & POLLOUT)
            wl_display_flush(dp);
    }

    /* If drag & drop was ended poll the file descriptor from the offer if
     * there is data to read.
     * We only accept the mime type text/uri-list.
     */
    if (wl->input.dnd_fd != -1) {
        fd.fd = wl->input.dnd_fd;
        fd.events = POLLIN | POLLHUP | POLLERR;

        if (poll(&fd, 1, 0) > 0) {
            if (fd.revents & POLLERR) {
                MP_ERR(wl, "error occured on the drag&drop fd\n");
                close(wl->input.dnd_fd);
                wl->input.dnd_fd = -1;
            }

            if (fd.revents & POLLIN) {
                int const to_read = 2048;
                char *buffer = malloc(to_read);
                size_t buffer_len = to_read;
                size_t str_len = 0;
                int has_read = 0;

                while (0 < (has_read = read(fd.fd, buffer+str_len, to_read))) {
                    if (buffer_len + to_read < buffer_len) {
                        MP_ERR(wl, "Integer overflow while reading from fd\n");
                        free(buffer);
                        break;
                    }

                    str_len += has_read;
                    buffer_len += to_read;
                    buffer = realloc(buffer, buffer_len);

                    if (has_read < to_read) {
                        buffer[str_len] = 0;
                        struct bstr file_list = bstr0(buffer);
                        mp_event_drop_mime_data(vo->input_ctx, "text/uri-list",
                                                file_list);
                        free(buffer);
                        break;
                    }
                }
            }

            if (fd.revents & POLLHUP) {
                close(wl->input.dnd_fd);
                wl->input.dnd_fd = -1;
            }
        }
    }

    // window events are reset by the resizing code
    return wl->window.events;
}

static void vo_wayland_update_screeninfo (struct vo *vo)
{
    struct vo_wayland_state *wl = vo->wayland;
    struct mp_vo_opts *opts = vo->opts;
    bool mode_received = false;

    wl_display_roundtrip(wl->display.display);

    vo->xinerama_x = vo->xinerama_y = 0;

    int screen_id = 0;

    struct vo_wayland_output *output;
    struct vo_wayland_output *first_output = NULL;
    struct vo_wayland_output *fsscreen_output = NULL;

    wl_list_for_each_reverse(output, &wl->display.output_list, link) {
        if (!output || !output->width)
            continue;

        mode_received = true;

        if (opts->fsscreen_id == screen_id)
            fsscreen_output = output;

        if (!first_output)
            first_output = output;

        screen_id++;
    }

    if (!mode_received) {
        MP_ERR(wl, "no output mode detected\n");
        return;
    }

    if (fsscreen_output) {
        wl->display.fs_output = fsscreen_output->output;
        opts->screenwidth = fsscreen_output->width;
        opts->screenheight = fsscreen_output->height;
    }
    else {
        wl->display.fs_output = NULL; /* current output is always 0 */

        if (first_output) {
            opts->screenwidth = first_output->width;
            opts->screenheight = first_output->height;
        }
    }

    wl->window.fs_width = opts->screenwidth;
    wl->window.fs_height = opts->screenheight;

    xdg_surface_set_output(wl->window.xdg_surface, wl->display.fs_output);
}

int vo_wayland_control (struct vo *vo, int *events, int request, void *arg)
{
    struct vo_wayland_state *wl = vo->wayland;
    wl_display_dispatch_pending(wl->display.display);

    switch (request) {
    case VOCTRL_CHECK_EVENTS:
        *events |= vo_wayland_check_events(vo);
        return VO_TRUE;
    case VOCTRL_FULLSCREEN:
        vo_wayland_fullscreen(vo);
        *events |= VO_EVENT_RESIZE;
        return VO_TRUE;
    case VOCTRL_ONTOP:
        vo_wayland_ontop(vo);
        return VO_TRUE;
    case VOCTRL_BORDER:
        vo_wayland_border(vo);
        *events |= VO_EVENT_RESIZE;
        return VO_TRUE;
    case VOCTRL_UPDATE_SCREENINFO:
        vo_wayland_update_screeninfo(vo);
        return VO_TRUE;
    case VOCTRL_GET_WINDOW_SIZE: {
        int *s = arg;
        s[0] = wl->window.width;
        s[1] = wl->window.height;
        return VO_TRUE;
    }
    case VOCTRL_SET_WINDOW_SIZE: {
        int *s = arg;
        if (!wl->window.is_fullscreen)
            shedule_resize(wl, s[0], s[1]);
        return VO_TRUE;
    }
    case VOCTRL_SET_CURSOR_VISIBILITY:
        if (*(bool *)arg) {
            if (!wl->cursor.visible)
                show_cursor(wl);
        }
        else {
            if (wl->cursor.visible)
                hide_cursor(wl);
        }
        wl->cursor.visible = *(bool *)arg;
        return VO_TRUE;
    case VOCTRL_UPDATE_WINDOW_TITLE:
        xdg_surface_set_title(wl->window.xdg_surface, (char *) arg);
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

bool vo_wayland_config (struct vo *vo, uint32_t d_width,
                        uint32_t d_height, uint32_t flags)
{
    struct vo_wayland_state *wl = vo->wayland;

    wl->window.p_width = d_width;
    wl->window.p_height = d_height;
    wl->window.aspect = d_width / (float) MPMAX(d_height, 1);

    if (!(flags & VOFLAG_HIDDEN)) {
        if (!wl->window.is_init) {
            wl->window.width = d_width;
            wl->window.height = d_height;
        }

        if (vo->opts->fullscreen) {
            if (wl->window.is_fullscreen)
                shedule_resize(wl, wl->window.fs_width, wl->window.fs_height);
            else
                vo_wayland_fullscreen(vo);
        }
        else
            vo_wayland_ontop(vo);
        wl->window.is_init = true;
    }

    return true;
}
