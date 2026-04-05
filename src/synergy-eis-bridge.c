/*
 * synergy-eis-bridge.c  (hot-patch edition)
 *
 * LD_PRELOAD shim that fixes Synergy client on KDE Plasma 5 Wayland.
 *
 * ─── THE PROBLEM ────────────────────────────────────────────────────────────
 *
 * Synergy 3.x uses libportal to talk to the XDG Desktop Portal for remote
 * desktop access.  The relevant call is:
 *
 *     xdp_session_connect_to_eis(session, &error)   → int (fd)
 *
 * This function asks the portal to open an EIS (Emulated Input Server)
 * socket so that the compositor (KWin) can feed input events to libei inside
 * synergy-core.  The EIS mechanism was introduced in KDE Plasma 6 / KWin 6.
 * On KDE Plasma 5 (KWin 5.x) the portal call always returns -1 because KWin
 * has no EIS socket to offer.
 *
 * When the fd comes back as -1, the callback at binary offset 0xd3f91 does:
 *
 *     js  0xd4013   ; jump-if-signed → g_main_loop_quit()
 *
 * ...and synergy-core silently terminates after ~0.15 seconds, causing the
 * Synergy GUI to restart it in an infinite loop.
 *
 * ─── WHY LD_PRELOAD SYMBOL INTERCEPTION DOESN'T WORK ───────────────────────
 *
 * The normal fix attempt is to export our own `xdp_session_connect_to_eis`
 * from an LD_PRELOAD library so the dynamic linker picks ours over the real
 * one.  This works when the target binary resolves the symbol dynamically
 * (a 'U' type in nm output, resolved via PLT/GOT at runtime).
 *
 * In Synergy's case, libportal was statically compiled into synergy-core:
 *
 *     $ nm /opt/Synergy/synergy-core | grep xdp_session_connect_to_eis
 *     000000000013499e T xdp_session_connect_to_eis
 *
 * The 'T' symbol type means it is a *defined* function in the text segment —
 * not an external reference.  The call at 0xd3f88 is a direct relative CALL:
 *
 *     e8 11 0a 06 00    callq  0x13499e
 *
 * There is no PLT stub, no GOT entry, and therefore nothing for the dynamic
 * linker to redirect.  LD_PRELOAD can export the symbol all it wants; the
 * binary will never look it up.
 *
 * ─── THE SOLUTION: RUNTIME HOT-PATCHING ─────────────────────────────────────
 *
 * Since the binary is PIE (Position-Independent Executable), its load address
 * is randomized, but we can find where it landed by reading /proc/self/maps:
 * the very first segment mapped from synergy-core with file offset 0 gives us
 * the load bias.
 *
 *     load_bias = VA of first synergy-core mapping with file_offset == 0
 *     target    = load_bias + 0x13499e    ← xdp_session_connect_to_eis
 *
 * We then use mprotect() to make the page writable and overwrite the first
 * 14 bytes of the function with an absolute indirect JMP trampoline:
 *
 *     ff 25 00 00 00 00           jmpq  *[rip+0]
 *     <8 bytes: absolute address of bridge_connect_to_eis>
 *
 * After this patch, the next time synergy-core calls xdp_session_connect_to_eis
 * it lands in our function instead, which:
 *   1. Creates a real EIS server via libeis (eis_setup_backend_fd).
 *   2. Returns one end of a socket pair as the "EIS fd" to synergy-core.
 *   3. Spawns a background thread that relays libeis events → /dev/uinput.
 *
 * Mouse and keyboard events from the Windows host then flow:
 *   ryzord (Windows) ──TCP/TLS──► synergy-core ──libei──► our EIS server
 *   ──► /dev/uinput ──► KWin Wayland input stack ──► applications
 *
 * ─── NOTE ON SCREEN DIMENSIONS ──────────────────────────────────────────────
 *
 * Synergy's DINF protocol message encodes screen width/height as SInt16
 * (signed 16-bit).  If you pass 65535 the server receives -1 and rejects the
 * client with "invalid message: DINF".  Always pass your real screen size via
 * SYNERGY_SCREEN_W / SYNERGY_SCREEN_H (see wayland-fix.conf).
 *
 * ─── COMPILE ────────────────────────────────────────────────────────────────
 *
 *   cc -O2 -shared -fPIC -o synergy-eis-bridge.so synergy-eis-bridge.c \
 *       -I/usr/include/libei-1.0 -leis -ldl -lpthread
 *
 * ─── USAGE ──────────────────────────────────────────────────────────────────
 *
 *   See install.sh or README.md for full installation instructions.
 *   The XDP_CONNECT_EIS_OFFSET constant below must match your synergy-core
 *   binary.  Verify with:
 *
 *       nm /opt/Synergy/synergy-core | grep xdp_session_connect_to_eis
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <poll.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <libeis.h>

/* ── logging ── */
#define LOG(fmt, ...) fprintf(stderr, "[eis-bridge] " fmt "\n", ##__VA_ARGS__)

/* ── constants ── */

/*
 * File offset of xdp_session_connect_to_eis inside synergy-core.
 *
 * This was determined by:
 *     nm /opt/Synergy/synergy-core | grep xdp_session_connect_to_eis
 *     → 000000000013499e T xdp_session_connect_to_eis
 *
 * If you are using a different Synergy version, re-run the nm command
 * and update this value.  The install.sh script does this automatically.
 */
#define XDP_CONNECT_EIS_OFFSET 0x13499eUL

/* ── screen size ── */

/*
 * The combined virtual desktop size in pixels.  Synergy's DINF message
 * sends these as SInt16, so they must be <= 32767.
 *
 * Defaults — overridden at runtime via SYNERGY_SCREEN_W / SYNERGY_SCREEN_H
 * environment variables set in the systemd drop-in.
 *
 * To find your combined size:
 *     xrandr | grep "^Screen 0" | grep -oP "current \K[0-9]+ x [0-9]+"
 */
static int screen_w = 1920;
static int screen_h = 1080;

/*
 * detect_screen_size() — called from bridge_init() before the hot-patch.
 *
 * IMPORTANT: Do NOT call popen()/system()/xrandr here.  This constructor
 * runs before the Wayland/X11 connection is established; any attempt to
 * spawn a subprocess that needs a display will deadlock.  Use the env vars.
 */
static void detect_screen_size(void)
{
    const char *sw = getenv("SYNERGY_SCREEN_W");
    const char *sh = getenv("SYNERGY_SCREEN_H");
    if (sw && sh) {
        int w = atoi(sw), h = atoi(sh);
        if (w > 0 && w <= 32767 && h > 0 && h <= 32767) {
            screen_w = w;
            screen_h = h;
            LOG("screen size from env: %dx%d", screen_w, screen_h);
            return;
        }
        LOG("WARNING: SYNERGY_SCREEN_W/H out of range, using default");
    }
    LOG("screen size: using default %dx%d", screen_w, screen_h);
}

/* ── uinput helpers ── */

static int uinput_fd_ptr = -1;
static int uinput_fd_kbd = -1;

static int uinput_open_pointer(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        LOG("open /dev/uinput (ptr) failed: %s", strerror(errno));
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT,  EV_KEY);
    ioctl(fd, UI_SET_EVBIT,  EV_ABS);
    ioctl(fd, UI_SET_EVBIT,  EV_REL);
    ioctl(fd, UI_SET_EVBIT,  EV_SYN);

    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_KEYBIT, BTN_SIDE);
    ioctl(fd, UI_SET_KEYBIT, BTN_EXTRA);

    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);

    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);

    struct uinput_abs_setup abs_x = { .code = ABS_X,
        .absinfo = { .minimum = 0, .maximum = screen_w } };
    struct uinput_abs_setup abs_y = { .code = ABS_Y,
        .absinfo = { .minimum = 0, .maximum = screen_h } };
    ioctl(fd, UI_ABS_SETUP, &abs_x);
    ioctl(fd, UI_ABS_SETUP, &abs_y);

    struct uinput_setup usetup = {0};
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor  = 0x1234;
    usetup.id.product = 0x5678;
    strncpy(usetup.name, "Synergy EIS Pointer", UINPUT_MAX_NAME_SIZE);

    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0 ||
        ioctl(fd, UI_DEV_CREATE) < 0)
    {
        LOG("failed to create pointer uinput device: %s", strerror(errno));
        close(fd);
        return -1;
    }
    LOG("pointer uinput device created");
    return fd;
}

static int uinput_open_keyboard(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        LOG("open /dev/uinput (kbd) failed: %s", strerror(errno));
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    for (int i = 0; i < KEY_MAX; i++)
        ioctl(fd, UI_SET_KEYBIT, i);

    struct uinput_setup usetup = {0};
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor  = 0x1234;
    usetup.id.product = 0x5679;
    strncpy(usetup.name, "Synergy EIS Keyboard", UINPUT_MAX_NAME_SIZE);

    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0 ||
        ioctl(fd, UI_DEV_CREATE) < 0)
    {
        LOG("failed to create keyboard uinput device: %s", strerror(errno));
        close(fd);
        return -1;
    }
    LOG("keyboard uinput device created");
    return fd;
}

static void uinput_emit(int fd, int type, int code, int value)
{
    struct input_event ev = {0};
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) < 0)
        LOG("uinput write error: %s", strerror(errno));
}

static void uinput_sync(int fd)
{
    uinput_emit(fd, EV_SYN, SYN_REPORT, 0);
}

/* ── EIS server background thread ── */

struct eis_thread_ctx {
    struct eis        *eis;
    struct eis_seat   *seat;
    struct eis_device *dev_ptr;
    struct eis_device *dev_kbd;
};

static void *eis_server_thread(void *arg)
{
    struct eis_thread_ctx *ctx = (struct eis_thread_ctx *)arg;
    struct eis *eis = ctx->eis;
    int eis_fd = eis_get_fd(eis);

    while (1) {
        struct pollfd pfd = { .fd = eis_fd, .events = POLLIN };
        int r = poll(&pfd, 1, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            LOG("poll error: %s", strerror(errno));
            break;
        }
        if (!(pfd.revents & POLLIN))
            continue;

        eis_dispatch(eis);

        struct eis_event *ev;
        while ((ev = eis_get_event(eis)) != NULL) {
            enum eis_event_type t = eis_event_get_type(ev);
            switch (t) {

            case EIS_EVENT_CLIENT_CONNECT: {
                struct eis_client *client = eis_event_get_client(ev);
                LOG("EI client connected: %s", eis_client_get_name(client));

                ctx->seat = eis_client_new_seat(client, "default");
                eis_seat_configure_capability(ctx->seat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
                eis_seat_configure_capability(ctx->seat, EIS_DEVICE_CAP_POINTER);
                eis_seat_configure_capability(ctx->seat, EIS_DEVICE_CAP_KEYBOARD);
                eis_seat_configure_capability(ctx->seat, EIS_DEVICE_CAP_BUTTON);
                eis_seat_configure_capability(ctx->seat, EIS_DEVICE_CAP_SCROLL);
                eis_seat_add(ctx->seat);
                eis_client_connect(client);
                break;
            }

            case EIS_EVENT_SEAT_BIND: {
                if (!ctx->seat) break;

                ctx->dev_ptr = eis_seat_new_device(ctx->seat);
                eis_device_configure_name(ctx->dev_ptr, "Synergy pointer");
                eis_device_configure_capability(ctx->dev_ptr, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
                eis_device_configure_capability(ctx->dev_ptr, EIS_DEVICE_CAP_POINTER);
                eis_device_configure_capability(ctx->dev_ptr, EIS_DEVICE_CAP_BUTTON);
                eis_device_configure_capability(ctx->dev_ptr, EIS_DEVICE_CAP_SCROLL);
                eis_device_configure_size(ctx->dev_ptr, screen_w, screen_h);

                struct eis_region *region = eis_device_new_region(ctx->dev_ptr);
                eis_region_set_size(region, screen_w, screen_h);
                eis_region_set_offset(region, 0, 0);
                eis_region_set_physical_scale(region, 1.0);
                eis_region_add(region);

                eis_device_add(ctx->dev_ptr);
                eis_device_resume(ctx->dev_ptr);

                ctx->dev_kbd = eis_seat_new_device(ctx->seat);
                eis_device_configure_name(ctx->dev_kbd, "Synergy keyboard");
                eis_device_configure_capability(ctx->dev_kbd, EIS_DEVICE_CAP_KEYBOARD);
                eis_device_add(ctx->dev_kbd);
                eis_device_resume(ctx->dev_kbd);

                LOG("EIS devices added and resumed");
                break;
            }

            case EIS_EVENT_POINTER_MOTION: {
                double dx = eis_event_pointer_get_dx(ev);
                double dy = eis_event_pointer_get_dy(ev);
                if (uinput_fd_ptr >= 0) {
                    uinput_emit(uinput_fd_ptr, EV_REL, REL_X, (int)dx);
                    uinput_emit(uinput_fd_ptr, EV_REL, REL_Y, (int)dy);
                    uinput_sync(uinput_fd_ptr);
                }
                break;
            }

            case EIS_EVENT_POINTER_MOTION_ABSOLUTE: {
                double ax = eis_event_pointer_get_absolute_x(ev);
                double ay = eis_event_pointer_get_absolute_y(ev);
                if (uinput_fd_ptr >= 0) {
                    uinput_emit(uinput_fd_ptr, EV_ABS, ABS_X, (int)ax);
                    uinput_emit(uinput_fd_ptr, EV_ABS, ABS_Y, (int)ay);
                    uinput_sync(uinput_fd_ptr);
                }
                break;
            }

            case EIS_EVENT_BUTTON_BUTTON: {
                uint32_t btn = eis_event_button_get_button(ev);
                bool pressed = eis_event_button_get_is_press(ev);
                if (uinput_fd_ptr >= 0) {
                    uinput_emit(uinput_fd_ptr, EV_KEY, btn, pressed ? 1 : 0);
                    uinput_sync(uinput_fd_ptr);
                }
                break;
            }

            case EIS_EVENT_SCROLL_DELTA: {
                double sdx = eis_event_scroll_get_dx(ev);
                double sdy = eis_event_scroll_get_dy(ev);
                if (uinput_fd_ptr >= 0) {
                    if (sdy != 0)
                        uinput_emit(uinput_fd_ptr, EV_REL, REL_WHEEL, sdy > 0 ? -1 : 1);
                    if (sdx != 0)
                        uinput_emit(uinput_fd_ptr, EV_REL, REL_HWHEEL, sdx > 0 ? 1 : -1);
                    uinput_sync(uinput_fd_ptr);
                }
                break;
            }

            case EIS_EVENT_SCROLL_DISCRETE: {
                /*
                 * Standard scroll-wheel events.  libeis reports them in
                 * units of 120 per detent (Linux / Windows convention).
                 * REL_WHEEL: +1 = up, -1 = down  →  negate the EIS dy.
                 * REL_HWHEEL: +1 = right, -1 = left  →  same sign as dx.
                 */
                int32_t ddx = eis_event_scroll_get_discrete_dx(ev);
                int32_t ddy = eis_event_scroll_get_discrete_dy(ev);
                if (uinput_fd_ptr >= 0) {
                    if (ddy != 0)
                        uinput_emit(uinput_fd_ptr, EV_REL, REL_WHEEL, -(ddy / 120));
                    if (ddx != 0)
                        uinput_emit(uinput_fd_ptr, EV_REL, REL_HWHEEL, ddx / 120);
                    uinput_sync(uinput_fd_ptr);
                }
                break;
            }

            case EIS_EVENT_SCROLL_STOP:
            case EIS_EVENT_SCROLL_CANCEL:
                /* nothing to do for uinput */
                break;

            case EIS_EVENT_KEYBOARD_KEY: {
                uint32_t key = eis_event_keyboard_get_key(ev);
                bool pressed = eis_event_keyboard_get_key_is_press(ev);
                if (uinput_fd_kbd >= 0) {
                    uinput_emit(uinput_fd_kbd, EV_KEY, key, pressed ? 1 : 0);
                    uinput_sync(uinput_fd_kbd);
                }
                break;
            }

            case EIS_EVENT_CLIENT_DISCONNECT:
                LOG("EI client disconnected");
                break;

            case EIS_EVENT_DEVICE_START_EMULATING:
                LOG("device start emulating");
                break;

            case EIS_EVENT_DEVICE_STOP_EMULATING:
                LOG("device stop emulating");
                break;

            default:
                break;
            }

            eis_event_unref(ev);
        }
    }

    free(ctx);
    return NULL;
}

/* ── our replacement for xdp_session_connect_to_eis ── */

/*
 * bridge_connect_to_eis() — called instead of the real function once the
 * hot-patch is installed.
 *
 * Signature matches libportal:
 *   int xdp_session_connect_to_eis(XdpSession *session, GError **error);
 *
 * We ignore both arguments (no portal involved) and return a real EIS fd
 * backed by /dev/uinput.
 */
static int bridge_connect_to_eis(void *session, void *error_ptr)
{
    (void)session;
    (void)error_ptr;

    LOG("intercepted xdp_session_connect_to_eis — creating EIS server");

    if (uinput_fd_ptr < 0) {
        uinput_fd_ptr = uinput_open_pointer();
        if (uinput_fd_ptr < 0)
            LOG("WARNING: pointer uinput unavailable — check /dev/uinput permissions");
    }
    if (uinput_fd_kbd < 0) {
        uinput_fd_kbd = uinput_open_keyboard();
        if (uinput_fd_kbd < 0)
            LOG("WARNING: keyboard uinput unavailable");
    }

    struct eis *eis = eis_new(NULL);
    if (!eis) {
        LOG("eis_new failed");
        return -1;
    }

    if (eis_setup_backend_fd(eis) < 0) {
        LOG("eis_setup_backend_fd failed");
        eis_unref(eis);
        return -1;
    }

    int client_fd = eis_backend_fd_add_client(eis);
    if (client_fd < 0) {
        LOG("eis_backend_fd_add_client failed: %s", strerror(errno));
        eis_unref(eis);
        return -1;
    }

    LOG("EIS server ready, client fd = %d", client_fd);

    struct eis_thread_ctx *ctx = calloc(1, sizeof(*ctx));
    ctx->eis = eis;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, eis_server_thread, ctx) != 0) {
        LOG("pthread_create failed: %s", strerror(errno));
        free(ctx);
        eis_unref(eis);
        close(client_fd);
        return -1;
    }
    pthread_attr_destroy(&attr);

    return client_fd;
}

/* ── constructor: find load bias and install hot-patch ── */

/*
 * find_synergy_bias() — parse /proc/self/maps to find where synergy-core
 * was loaded.
 *
 * A PIE binary's ELF header is always mapped at file offset 0 in the first
 * segment.  The virtual address of that mapping IS the load bias:
 *
 *     load_bias = VA_of_first_segment   (because file_offset == 0)
 *
 * So:  runtime_address = load_bias + symbol_file_offset
 */
static unsigned long find_synergy_bias(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;

    char line[512];
    unsigned long bias = 0;

    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "synergy-core")) continue;

        unsigned long va_start, va_end, file_off;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s %lx",
                   &va_start, &va_end, perms, &file_off) != 4)
            continue;

        if (file_off == 0) {
            bias = va_start;
            break;
        }
    }
    fclose(f);
    return bias;
}

/*
 * bridge_init() — runs automatically when the .so is loaded by the dynamic
 * linker (before main()).
 *
 * Steps:
 *  1. Read screen dimensions from env.
 *  2. Find synergy-core's load bias via /proc/self/maps.
 *     If not found (e.g. we're inside `--version` or another binary),
 *     the constructor returns early and does nothing.
 *  3. Verify the expected endbr64 prologue at the patch site.
 *  4. mprotect the page(s) RWX.
 *  5. Write the 14-byte absolute-indirect JMP trampoline.
 *  6. Restore page protection to RX.
 */
__attribute__((constructor))
static void bridge_init(void)
{
    detect_screen_size();

    unsigned long bias = find_synergy_bias();
    if (!bias) {
        /* Not running inside synergy-core — nothing to patch. */
        return;
    }
    LOG("synergy-core load bias = 0x%lx", bias);

    uint8_t *target = (uint8_t *)(bias + XDP_CONNECT_EIS_OFFSET);
    LOG("patching xdp_session_connect_to_eis at %p", target);

    /*
     * Sanity check: the function must start with endbr64 (f3 0f 1e fa).
     * If the bytes differ, the offset is wrong for this binary version —
     * abort rather than corrupt the code.
     */
    if (target[0] != 0xf3 || target[1] != 0x0f ||
        target[2] != 0x1e || target[3] != 0xfa)
    {
        LOG("ERROR: unexpected bytes %02x %02x %02x %02x at target "
            "(expected endbr64: f3 0f 1e fa).",
            target[0], target[1], target[2], target[3]);
        LOG("Re-run: nm /opt/Synergy/synergy-core | grep xdp_session_connect_to_eis");
        LOG("Then update XDP_CONNECT_EIS_OFFSET in synergy-eis-bridge.c and recompile.");
        return;
    }

    long     page_size = sysconf(_SC_PAGESIZE);
    uint8_t *page      = (uint8_t *)((uintptr_t)target & ~(uintptr_t)(page_size - 1));

    if (mprotect(page, (size_t)(page_size * 2),
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
    {
        LOG("mprotect(RWX) failed: %s — aborting patch", strerror(errno));
        return;
    }

    /*
     * Write 14-byte absolute indirect JMP:
     *
     *   offset  bytes              meaning
     *   +0      ff 25              jmpq *[rip + disp32]
     *   +2      00 00 00 00        disp32 = 0  →  [rip] after insn = target+6
     *   +6      <8-byte addr>      absolute destination (little-endian)
     *
     * At execution time, rip points to target+6 immediately after the
     * ff25 instruction.  The CPU reads the 8-byte pointer there and jumps.
     */
    uintptr_t dst = (uintptr_t)bridge_connect_to_eis;

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    target[0] = 0xff;
    target[1] = 0x25;
    target[2] = 0x00;
    target[3] = 0x00;
    target[4] = 0x00;
    target[5] = 0x00;
    *((uint64_t *)(target + 6)) = dst;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    mprotect(page, (size_t)(page_size * 2), PROT_READ | PROT_EXEC);

    LOG("hot-patch installed: %p -> %p (bridge_connect_to_eis)", target, (void *)dst);
}
