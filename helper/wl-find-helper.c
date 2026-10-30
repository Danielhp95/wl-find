// wl-find's link to the compositor, through Wayland protocols most
// compositors share rather than any one compositor's IPC.
//
//   wl-find-helper output [NAME]
//     Prints an output as JSON: its name, its logical box in the layout (x,
//     y, width, height), its scale, and whether the compositor has a virtual
//     pointer ("pointer"), which wl-kbptr's hints need too. With no NAME, the
//     output a new layer surface goes on: the focused one.
//
//   wl-find-helper click NAME X Y
//     Clicks the left button at (X, Y), in logical pixels within output NAME:
//     with a virtual pointer (Hyprland, sway, niri), or else through the
//     RemoteDesktop portal and libei (COSMIC). The portal asks once; the
//     token that spares asking again is kept in $XDG_STATE_HOME/wl-find.
//
//   wl-find-helper send SOCKET LINE
//     Sends LINE to the find bar listening on SOCKET and prints the bar's
//     pid; fails quietly if none listens.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <libei.h>
#include <libportal/portal.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

struct output {
    struct wl_output *wl;
    struct zxdg_output_v1 *xdg;
    char *name;
    // Logical, from xdg-output.
    int32_t x, y, width, height;
    // The current mode, in pixels, and wl_output's whole-number scale.
    int32_t mode_width, mode_height, scale;
    // enum wl_output_transform.
    int32_t transform;
    struct output *next;
};

static struct {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zxdg_output_manager_v1 *xdg_output_manager;
    struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
    uint32_t pointer_manager_version;
    struct output *outputs;
    // The probe's progress.
    bool configured, closed, has_entered;
    uint32_t configure_serial;
    struct output *entered;
} wl;

static void die(const char *message) {
    fprintf(stderr, "wl-find-helper: %s\n", message);
    exit(1);
}

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y, int32_t pw,
                            int32_t ph, int32_t subpixel, const char *make, const char *model,
                            int32_t transform) {
    ((struct output *)data)->transform = transform;
}

static void output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t width,
                        int32_t height, int32_t refresh) {
    struct output *out = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        out->mode_width = width;
        out->mode_height = height;
    }
}

static void output_done(void *data, struct wl_output *o) {}

static void output_scale(void *data, struct wl_output *o, int32_t factor) {
    ((struct output *)data)->scale = factor;
}

static void output_name(void *data, struct wl_output *o, const char *name) {
    struct output *out = data;
    free(out->name);
    out->name = strdup(name);
}

static void output_description(void *data, struct wl_output *o, const char *description) {}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void xdg_output_position(void *data, struct zxdg_output_v1 *o, int32_t x, int32_t y) {
    struct output *out = data;
    out->x = x;
    out->y = y;
}

static void xdg_output_size(void *data, struct zxdg_output_v1 *o, int32_t width, int32_t height) {
    struct output *out = data;
    out->width = width;
    out->height = height;
}

static void xdg_output_done(void *data, struct zxdg_output_v1 *o) {}

// Older compositors name outputs only here (wl_output gained names in v4).
static void xdg_output_name(void *data, struct zxdg_output_v1 *o, const char *name) {
    struct output *out = data;
    if (!out->name) out->name = strdup(name);
}

static void xdg_output_description(void *data, struct zxdg_output_v1 *o, const char *d) {}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_output_position,
    .logical_size = xdg_output_size,
    .done = xdg_output_done,
    .name = xdg_output_name,
    .description = xdg_output_description,
};

static uint32_t min_version(uint32_t offered, uint32_t wanted) {
    return offered < wanted ? offered : wanted;
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t id,
                            const char *interface, uint32_t version) {
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        wl.compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 1);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        wl.shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !wl.seat) {
        wl.seat = wl_registry_bind(registry, id, &wl_seat_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        wl.layer_shell = wl_registry_bind(registry, id, &zwlr_layer_shell_v1_interface,
                                          min_version(version, 2));
    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        wl.xdg_output_manager = wl_registry_bind(registry, id, &zxdg_output_manager_v1_interface,
                                                 min_version(version, 3));
    } else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        wl.pointer_manager_version = min_version(version, 2);
        wl.pointer_manager = wl_registry_bind(
            registry, id, &zwlr_virtual_pointer_manager_v1_interface, wl.pointer_manager_version);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct output *out = calloc(1, sizeof(*out));
        out->scale = 1;
        out->wl = wl_registry_bind(registry, id, &wl_output_interface, min_version(version, 4));
        wl_output_add_listener(out->wl, &output_listener, out);
        out->next = wl.outputs;
        wl.outputs = out;
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t id) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void connect_wayland(void) {
    wl.display = wl_display_connect(NULL);
    if (!wl.display) die("cannot connect to the Wayland display");
    wl_registry_add_listener(wl_display_get_registry(wl.display), &registry_listener, NULL);
    wl_display_roundtrip(wl.display);
    if (!wl.compositor || !wl.xdg_output_manager) die("the compositor lacks xdg-output");
    for (struct output *out = wl.outputs; out; out = out->next) {
        out->xdg = zxdg_output_manager_v1_get_xdg_output(wl.xdg_output_manager, out->wl);
        zxdg_output_v1_add_listener(out->xdg, &xdg_output_listener, out);
    }
    wl_display_roundtrip(wl.display);
}

static struct output *output_named(const char *name) {
    for (struct output *out = wl.outputs; out; out = out->next)
        if (out->name && strcmp(out->name, name) == 0) return out;
    return NULL;
}

// Dispatches Wayland events until *flag is set; false if timeout_ms passes
// first or the connection fails.
static bool dispatch_until(const bool *flag, int timeout_ms) {
    int64_t deadline = now_ms() + timeout_ms;
    while (!*flag) {
        while (wl_display_prepare_read(wl.display) != 0) wl_display_dispatch_pending(wl.display);
        if (*flag) {
            wl_display_cancel_read(wl.display);
            break;
        }
        wl_display_flush(wl.display);
        int64_t left = deadline - now_ms();
        struct pollfd fd = {.fd = wl_display_get_fd(wl.display), .events = POLLIN};
        int ready = left > 0 ? poll(&fd, 1, (int)left) : 0;
        if (ready <= 0) {
            wl_display_cancel_read(wl.display);
            if (ready < 0 && errno == EINTR) continue;
            return false;
        }
        if (wl_display_read_events(wl.display) < 0) return false;
        wl_display_dispatch_pending(wl.display);
    }
    return true;
}

static void surface_enter(void *data, struct wl_surface *surface, struct wl_output *o) {
    for (struct output *out = wl.outputs; out; out = out->next)
        if (out->wl == o) {
            wl.entered = out;
            wl.has_entered = true;
        }
}

static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *o) {}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
                            uint32_t width, uint32_t height) {
    wl.configure_serial = serial;
    wl.configured = true;
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
    wl.closed = true;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

// A transparent pixel, for the probe to show.
static struct wl_buffer *clear_pixel(void) {
    int fd = memfd_create("wl-find-probe", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, 4) < 0) die("cannot make the probe's buffer");
    struct wl_shm_pool *pool = wl_shm_create_pool(wl.shm, fd, 4);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

// The focused output, found the one way every compositor shares: a layer
// surface that names no output goes on the focused one, so show a clear
// pixel and see which output it enters. It is on the overlay layer, where
// nothing can cover it: some compositors tell a covered surface of no
// output at all.
static struct output *focused_output(void) {
    if (!wl.layer_shell || !wl.shm) die("the compositor lacks layer-shell");
    struct wl_surface *surface = wl_compositor_create_surface(wl.compositor);
    wl_surface_add_listener(surface, &surface_listener, NULL);
    struct wl_region *nothing = wl_compositor_create_region(wl.compositor);
    wl_surface_set_input_region(surface, nothing);
    wl_region_destroy(nothing);
    struct zwlr_layer_surface_v1 *layer = zwlr_layer_shell_v1_get_layer_surface(
        wl.layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "wl-find-probe");
    zwlr_layer_surface_v1_add_listener(layer, &layer_listener, NULL);
    zwlr_layer_surface_v1_set_size(layer, 1, 1);
    zwlr_layer_surface_v1_set_anchor(
        layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    wl_surface_commit(surface);
    if (!dispatch_until(&wl.configured, 1000) || wl.closed)
        die("the compositor did not place the probe");
    zwlr_layer_surface_v1_ack_configure(layer, wl.configure_serial);
    struct wl_buffer *buffer = clear_pixel();
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, 1, 1);
    wl_surface_commit(surface);
    dispatch_until(&wl.has_entered, 1000);
    zwlr_layer_surface_v1_destroy(layer);
    wl_surface_destroy(surface);
    wl_buffer_destroy(buffer);
    wl_display_flush(wl.display);
    if (!wl.entered) die("the probe entered no output");
    return wl.entered;
}

static void print_output(const struct output *out) {
    // Pixels per logical pixel, from the mode, as wl_output's scale is a
    // whole number. max() on both sides, as a rotated output's mode is
    // still in the panel's own orientation.
    double scale = out->scale;
    int32_t logical = out->width > out->height ? out->width : out->height;
    int32_t pixels = out->mode_width > out->mode_height ? out->mode_width : out->mode_height;
    if (logical > 0 && pixels > 0) scale = (double)pixels / logical;
    printf("{\"name\":\"");
    for (const char *c = out->name; *c; c++) {
        if (*c == '"' || *c == '\\') putchar('\\');
        putchar(*c);
    }
    printf("\",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,\"scale\":%.6g,\"pointer\":%s}\n",
           out->x, out->y, out->width, out->height, scale, wl.pointer_manager ? "true" : "false");
}

// (x, y) in an output's w x h, turned to the output's own orientation: the
// inverse of its transform, as smithay's Transform::transform_point_in() has
// them. The size turns with it.
static void untransform(int32_t transform, double *x, double *y, double *w, double *h) {
    double tx = *x, ty = *y, tw = *w, th = *h;
    switch (transform) {
    case WL_OUTPUT_TRANSFORM_90: *x = th - ty; *y = tx; break;
    case WL_OUTPUT_TRANSFORM_180: *x = tw - tx; *y = th - ty; break;
    case WL_OUTPUT_TRANSFORM_270: *x = ty; *y = tw - tx; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED: *x = tw - tx; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_90: *x = ty; *y = tx; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_180: *y = th - ty; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_270: *x = th - ty; *y = tw - tx; break;
    }
    if (transform % 2) {
        *w = th;
        *h = tw;
    }
}

// A click with a virtual pointer. Bound to the output (v2), an absolute
// motion spans the output; unbound (v1), it spans the whole layout. The
// extent is in 1/256 pixels, so the pointer lands between pixels too.
static void click_virtual_pointer(const struct output *out, double x, double y) {
    if (!wl.seat) die("the compositor has no seat");
    struct zwlr_virtual_pointer_v1 *pointer;
    int32_t left = out->x, top = out->y, right = out->x + out->width, bottom = out->y + out->height;
    if (wl.pointer_manager_version >= 2) {
        pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
            wl.pointer_manager, wl.seat, out->wl);
        // niri reads the motion in the output's own orientation, as a
        // touchscreen's; Hyprland and wlroots read it as laid out.
        if (getenv("NIRI_SOCKET")) {
            double w = out->width, h = out->height;
            untransform(out->transform, &x, &y, &w, &h);
            right = left + (int32_t)w;
            bottom = top + (int32_t)h;
        }
    } else {
        pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(wl.pointer_manager, wl.seat);
        for (struct output *o = wl.outputs; o; o = o->next) {
            if (o->x < left) left = o->x;
            if (o->y < top) top = o->y;
            if (o->x + o->width > right) right = o->x + o->width;
            if (o->y + o->height > bottom) bottom = o->y + o->height;
        }
    }
    uint32_t time = (uint32_t)now_ms();
    zwlr_virtual_pointer_v1_motion_absolute(pointer, time, (uint32_t)lround((out->x - left + x) * 256),
                                            (uint32_t)lround((out->y - top + y) * 256),
                                            (uint32_t)(right - left) * 256,
                                            (uint32_t)(bottom - top) * 256);
    zwlr_virtual_pointer_v1_frame(pointer);
    zwlr_virtual_pointer_v1_button(pointer, time, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(pointer);
    zwlr_virtual_pointer_v1_button(pointer, time, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(pointer);
    zwlr_virtual_pointer_v1_destroy(pointer);
    wl_display_roundtrip(wl.display);
}

// A click through libei, at (x, y) in global logical pixels: COSMIC's
// absolute pointer has a region per output, at the output's place in the
// layout.
static void click_ei(int fd, double x, double y) {
    struct ei *ei = ei_new_sender(NULL);
    ei_configure_name(ei, "wl-find");
    if (ei_setup_backend_fd(ei, fd) != 0) die("cannot set up libei");
    bool sent = false;
    uint32_t sequence = 0;
    int64_t deadline = now_ms() + 3000;
    while (!sent) {
        int64_t left = deadline - now_ms();
        struct pollfd pfd = {.fd = ei_get_fd(ei), .events = POLLIN};
        if (left <= 0 || poll(&pfd, 1, (int)left) == 0) die("the compositor offered no pointer over libei");
        ei_dispatch(ei);
        struct ei_event *event;
        while ((event = ei_get_event(ei))) {
            switch (ei_event_get_type(event)) {
            case EI_EVENT_SEAT_ADDED:
                ei_seat_bind_capabilities(ei_event_get_seat(event), EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                          EI_DEVICE_CAP_BUTTON, NULL);
                break;
            case EI_EVENT_DEVICE_RESUMED: {
                struct ei_device *device = ei_event_get_device(event);
                if (sent || !ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE) ||
                    !ei_device_has_capability(device, EI_DEVICE_CAP_BUTTON))
                    break;
                ei_device_start_emulating(device, ++sequence);
                ei_device_pointer_motion_absolute(device, x, y);
                ei_device_frame(device, ei_now(ei));
                ei_device_button_button(device, BTN_LEFT, true);
                ei_device_frame(device, ei_now(ei));
                ei_device_button_button(device, BTN_LEFT, false);
                ei_device_frame(device, ei_now(ei));
                ei_device_stop_emulating(device);
                sent = true;
                break;
            }
            case EI_EVENT_DISCONNECT:
                die("the compositor closed libei");
            default:
                break;
            }
            ei_event_unref(event);
        }
    }
    ei_dispatch(ei);
    ei_unref(ei);
}

struct portal_click {
    GMainLoop *loop;
    XdpSession *session;
    char *token_path;
    int fd;
    GError *error;
};

static void portal_started(GObject *source, GAsyncResult *result, gpointer data) {
    struct portal_click *pc = data;
    if (xdp_session_start_finish(pc->session, result, &pc->error)) {
        // Each start spends the token and hands a new one.
        char *token = xdp_session_get_restore_token(pc->session);
        if (token) {
            char *dir = g_path_get_dirname(pc->token_path);
            g_mkdir_with_parents(dir, 0700);
            g_file_set_contents(pc->token_path, token, -1, NULL);
            g_free(dir);
            g_free(token);
        }
        pc->fd = xdp_session_connect_to_eis(pc->session, &pc->error);
    }
    g_main_loop_quit(pc->loop);
}

static void portal_created(GObject *source, GAsyncResult *result, gpointer data) {
    struct portal_click *pc = data;
    pc->session =
        xdp_portal_create_remote_desktop_session_finish(XDP_PORTAL(source), result, &pc->error);
    if (pc->session)
        xdp_session_start(pc->session, NULL, NULL, portal_started, pc);
    else
        g_main_loop_quit(pc->loop);
}

static void click_portal(const struct output *out, double x, double y) {
    struct portal_click pc = {
        .loop = g_main_loop_new(NULL, FALSE),
        .token_path = g_build_filename(g_get_user_state_dir(), "wl-find", "remote-desktop-token", NULL),
        .fd = -1,
    };
    char *token = NULL;
    g_file_get_contents(pc.token_path, &token, NULL, NULL);
    XdpPortal *portal = xdp_portal_new();
    xdp_portal_create_remote_desktop_session_full(
        portal, XDP_DEVICE_POINTER, XDP_OUTPUT_NONE, XDP_REMOTE_DESKTOP_FLAG_NONE,
        XDP_CURSOR_MODE_HIDDEN, XDP_PERSIST_MODE_PERSISTENT, token, NULL, portal_created, &pc);
    g_main_loop_run(pc.loop);
    if (pc.fd < 0) {
        fprintf(stderr, "wl-find-helper: the RemoteDesktop portal gave no input: %s\n",
                pc.error ? pc.error->message : "refused");
        exit(1);
    }
    click_ei(pc.fd, out->x + x, out->y + y);
    xdp_session_close(pc.session);
    g_object_unref(pc.session);
    g_object_unref(portal);
    g_free(token);
    g_free(pc.token_path);
    g_main_loop_unref(pc.loop);
}

static int send_line(const char *path, const char *line) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(address.sun_path)) die("the socket's path is too long");
    strcpy(address.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) die("cannot make a socket");
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) return 1;
    struct ucred peer;
    socklen_t size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) < 0) die("cannot tell the bar's pid");
    size_t length = strlen(line);
    char *message = malloc(length + 1);
    memcpy(message, line, length);
    message[length] = '\n';
    for (size_t sent = 0; sent <= length;) {
        ssize_t n = write(fd, message + sent, length + 1 - sent);
        if (n < 0 && errno != EINTR) die("cannot write to the bar");
        if (n > 0) sent += n;
    }
    close(fd);
    printf("%d\n", peer.pid);
    return 0;
}

int main(int argc, char **argv) {
    bool output = argc >= 2 && argc <= 3 && strcmp(argv[1], "output") == 0;
    bool click = argc == 5 && strcmp(argv[1], "click") == 0;
    bool send = argc == 4 && strcmp(argv[1], "send") == 0;
    if (!output && !click && !send) {
        fprintf(stderr, "usage: wl-find-helper output [NAME]\n"
                        "       wl-find-helper click NAME X Y\n"
                        "       wl-find-helper send SOCKET LINE\n");
        return 2;
    }
    if (send) return send_line(argv[2], argv[3]);
    connect_wayland();
    struct output *out = argc >= 3 ? output_named(argv[2]) : focused_output();
    if (!out) die("no such output");
    if (output) {
        print_output(out);
        return 0;
    }
    char *end_x, *end_y;
    double x = strtod(argv[3], &end_x), y = strtod(argv[4], &end_y);
    if (*end_x || *end_y) die("X and Y must be numbers");
    if (wl.pointer_manager)
        click_virtual_pointer(out, x, y);
    else
        click_portal(out, x, y);
    return 0;
}
