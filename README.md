# Synergy EIS Bridge — KDE Plasma 5 Wayland Fix

> **TL;DR** — Synergy 3.x silently exits on KDE Plasma 5 Wayland in about
> 0.15 seconds, causing an infinite restart loop.  This patch fixes it.

---

## Table of Contents

1. [The Problem (10+ year community issue)](#the-problem)
2. [What This Fix Does](#what-this-fix-does)
3. [Prerequisites](#prerequisites)
4. [Quick Install](#quick-install)
5. [Manual Install](#manual-install)
6. [Configuration — screen size](#configuration)
7. [Clipboard Sync (X11 ↔ Wayland)](#clipboard-sync)
8. [Expected Output](#expected-output)
9. [Troubleshooting](#troubleshooting)
10. [Tested On](#tested-on)
11. [How It Works (overview)](#how-it-works)
12. [Technical Deep-Dive](#technical-deep-dive)
13. [License](#license)

---

## The Problem

Synergy has never worked reliably on KDE Plasma / Wayland.  The symptom is
always the same: Synergy connects, then immediately disconnects, over and over.
The GUI shows it restarting continuously with no meaningful error message.

### Why it happens

Synergy 3.x uses the **XDG Desktop Portal** Remote Desktop API to get a secure
input channel from the compositor (KWin).  The relevant call asks KWin for an
**EIS socket** (Emulated Input Server) — a mechanism for applications to inject
keyboard/mouse events into a Wayland session.

The EIS protocol was introduced in **KDE Plasma 6 / KWin 6**.  On **KDE
Plasma 5** (KWin 5.x), `xdp_session_connect_to_eis()` returns -1 because KWin
has no EIS socket to offer.

synergy-core checks the return value and, when it is negative, calls
`g_main_loop_quit()`.  The process exits silently in ≈ 0.15 seconds.  The
Synergy service manager restarts it.  The loop repeats forever.

This has affected every major KDE Plasma 5 distribution since Synergy adopted
the portal API — Ubuntu 20.04/22.04, Kubuntu, openSUSE Leap, Fedora with KDE,
Pop!_OS, and others.

### Why the obvious fix doesn't work

The first thing developers try is exporting `xdp_session_connect_to_eis` from
an LD_PRELOAD library so the linker picks their version at startup.  This works
when the symbol is resolved dynamically (`U` type in `nm` output).

In Synergy's binary, libportal was **statically compiled in**:

```
$ nm /opt/Synergy/synergy-core | grep xdp_session_connect_to_eis
000000000013499e T xdp_session_connect_to_eis
```

`T` means the symbol is **defined** in the binary's text segment.  The call is
a direct `call 0x13499e` with no PLT indirection.  LD_PRELOAD cannot intercept
direct calls.

---

## What This Fix Does

The fix has four parts:

1. **Runtime binary hot-patch** — the shared library (`synergy-eis-bridge.so`)
   uses a `__attribute__((constructor))` function to run code before `main()`.
   It locates the `xdp_session_connect_to_eis` function in the loaded binary,
   uses `mprotect` to make the page temporarily writable, and overwrites the
   first 14 bytes with an absolute-indirect JMP trampoline that redirects
   execution to `bridge_connect_to_eis()` inside the library.

2. **EIS server + uinput injection** — `bridge_connect_to_eis()` creates a
   `libeis` server and returns a valid file descriptor to synergy-core.
   A background thread handles all EIS events (pointer motion, clicks, scroll,
   keyboard keys) and injects them into the kernel via two `/dev/uinput` virtual
   devices: "Synergy EIS Pointer" and "Synergy EIS Keyboard".

3. **Screen dimension fix** — Synergy's DINF protocol message encodes screen
   size as signed 16-bit integers (`SInt16`).  Passing `65535` delivers `-1` to
   the server, which rejects it with *"invalid message from client: DINF"* and
   drops the connection every ~9 seconds.  The bridge reads correct dimensions
   from `SYNERGY_SCREEN_W` / `SYNERGY_SCREEN_H` environment variables set in
   the systemd drop-in.

4. **Clipboard sync bridge** — KWin 5.27's XWayland clipboard bridge is
   completely broken: content copied from Windows (in Synergy's X11 clipboard)
   never reaches Wayland apps, and Wayland clipboard changes never reach
   Synergy for forwarding to Windows.  `synergy-clip-bridge.sh` is a small
   systemd service that polls every 0.4 s and also reacts immediately to
   Synergy's "entering screen" log event to sync both directions.

---

## Prerequisites

| Requirement | Notes |
|---|---|
| KDE Plasma 5 Wayland session | Plasma 6 has native EIS — this patch may not be needed |
| Synergy 3.x client | Binary at `/opt/Synergy/synergy-core` |
| `libeis-dev` (Ubuntu/Debian) | Headers for the EIS server API |
| GCC | For compiling the shared library |
| `/dev/uinput` write access | Via udev rule or `setfacl` |
| `systemd --user` Synergy service | The standard Synergy service setup |

> **Note on libeis vs libportal:**  We need `libeis` (server-side EIS API) to
> create the EIS server endpoint.  `libportal` (client-side portal API) is
> already statically compiled into synergy-core and does not need to be
> installed separately.

---

## Quick Install

```bash
git clone https://github.com/YOUR_USERNAME/synergy-eis-bridge.git
cd synergy-eis-bridge
chmod +x scripts/install.sh
./scripts/install.sh
```

The installer will:
- Detect your synergy-core binary offset automatically using `nm`
- Install build dependencies
- Compile the shared library
- Grant `/dev/uinput` access via a udev rule
- Detect your screen dimensions from `xrandr`
- Write the systemd drop-in
- Restart Synergy

---

## Manual Install

### 1. Install build dependencies

**Debian / Ubuntu / Kubuntu:**
```bash
sudo apt install libeis-dev gcc
```

**Fedora / RHEL:**
```bash
sudo dnf install libeis-devel gcc
```

**Arch Linux:**
```bash
sudo pacman -S libeis
```

### 2. Find the offset for your synergy-core

> This value is version-specific.  Always re-derive it rather than copying
> from somewhere.

```bash
nm /opt/Synergy/synergy-core | grep xdp_session_connect_to_eis
# Expected output (Synergy 3.3.1):
# 000000000013499e T xdp_session_connect_to_eis
```

Note the hex address (e.g. `000000000013499e`).  Edit
`src/synergy-eis-bridge.c` and set:

```c
#define XDP_CONNECT_EIS_OFFSET 0x13499eUL
```

If `nm` is not installed: `sudo apt install binutils`

If the symbol is not found at all, your Synergy version uses a different
approach to acquire the EIS fd and may not need this patch.

### 3. Compile

```bash
mkdir -p ~/.local/lib
cc -O2 -shared -fPIC \
   -o ~/.local/lib/synergy-eis-bridge.so \
   src/synergy-eis-bridge.c \
   -I/usr/include/libei-1.0 \
   -leis -ldl -lpthread
```

If the headers are in a different location:
```bash
find /usr/include -name "libeis.h"
# Adjust -I path accordingly
```

### 4. Grant /dev/uinput access

**Option A — udev rule (persistent, survives reboots):**
```bash
cat <<'EOF' | sudo tee /etc/udev/rules.d/99-uinput-user.rules
KERNEL=="uinput", MODE="0660", GROUP="input", TAG+="uaccess", OPTIONS+="static_node=uinput"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger --name-match=uinput
# Log out and log back in for the uaccess tag to take effect
```

**Option B — ACL on current session (quick test):**
```bash
sudo setfacl -m u:$USER:rw /dev/uinput
```

### 5. Determine your screen size

```bash
xrandr | grep "^Screen 0" | grep -oP "current \K[0-9]+ x [0-9]+"
# Example output: 4080 x 1920
```

**Warning:** Screen dimensions must be ≤ 32767 (maximum `SInt16` value).
If you have an ultra-wide or multi-monitor setup exceeding 32767 in either
dimension, use the size of your primary monitor instead.

### 6. Write the systemd drop-in

```bash
mkdir -p ~/.config/systemd/user/synergy.service.d/
cat > ~/.config/systemd/user/synergy.service.d/wayland-fix.conf <<EOF
[Service]
Environment="LD_PRELOAD=$HOME/.local/lib/synergy-eis-bridge.so"
Environment="SYNERGY_SCREEN_W=4080"
Environment="SYNERGY_SCREEN_H=1920"
EOF
```

Replace `4080` and `1920` with your actual screen dimensions.

### 7. Reload and restart

```bash
systemctl --user daemon-reload
systemctl --user restart synergy.service
```

---

## Clipboard Sync

KWin 5.27 (KDE Plasma 5) does not synchronise the clipboard between XWayland
(`:1`) and Wayland.  Synergy writes Windows clipboard content to the X11
CLIPBOARD selection — but your Wayland apps never see it.  Conversely, text
you copy in a Wayland app is invisible to Synergy's X11 clipboard reader.

The clipboard bridge (`synergy-clip-bridge.service`) fixes both directions.

### How it works

```
Windows → Linux (X11→Wayland):
  Synergy receives clipboard from server
  → calls XSetSelectionOwner (X11 CLIPBOARD owner = synergy-core)
  → bridge polls X11, detects new content
  → fast-path: also triggers within 0.3 s of "entering screen" log event
  → calls wl-copy to push content to Wayland
  → Ctrl+V in any Wayland app now pastes the Windows clipboard ✓

Linux → Windows (Wayland→X11):
  User copies in a Wayland app (wl-copy, Kate, Firefox, …)
  → bridge polls Wayland, detects new content
  → starts xclip -loops 0 as X11 CLIPBOARD owner with the Wayland content
  → Synergy polls X11, reads the new content
  → next time cursor moves to Windows, Synergy pushes to Windows clipboard ✓
```

### Design notes

- **Hash-file dedup** (`/tmp/synergy-clip-last-hash`) stores an MD5 of the
  last-synced content as a file — not an in-process variable — so the state
  survives service restarts and prevents feedback loops.
- **Singleton** via `flock(1)` prevents duplicate bridge processes even if
  `Restart=on-failure` triggers during a restart.
- **xclip -loops 0** is used **only** for the Wayland→X11 direction.  Synergy
  can displace it at any time by calling `XSetSelectionOwner`.  It is *never*
  kept alive in the Windows→Linux direction — doing so would block Synergy from
  setting the X11 clipboard with new Windows content.

### Important: XAUTHORITY injection

The clipboard bridge and Synergy both need `DISPLAY=:1` and a valid
`XAUTHORITY` pointing to the XWayland auth file.  These are injected at login
by a KDE autostart entry (`~/.config/autostart/synergy-xenv.desktop`) that
runs:

```bash
systemctl --user import-environment DISPLAY XAUTHORITY
systemctl --user restart synergy.service synergy-clip-bridge.service
```

The autostart entry is installed by `install.sh` automatically.

### Clipboard not working after Synergy update or X server restart

The XAUTHORITY path changes on every login (`/run/user/1000/xauth_XXXXXXXX`).
The autostart entry re-imports it at login, so a re-login is all that is
normally needed.

If clipboard is still broken after re-login:
```bash
systemctl --user status synergy-clip-bridge.service
journalctl --user -u synergy-clip-bridge.service -n 20
```

---

## Configuration

### Screen dimensions

The most important configuration is matching the screen size to your actual
setup.  Edit `~/.config/systemd/user/synergy.service.d/wayland-fix.conf`:

```ini
[Service]
Environment="LD_PRELOAD=/home/YOUR_USER/.local/lib/synergy-eis-bridge.so"
Environment="SYNERGY_SCREEN_W=YOUR_WIDTH"
Environment="SYNERGY_SCREEN_H=YOUR_HEIGHT"
```

Find your dimensions:
```bash
xrandr | grep "^Screen 0" | grep -oP "current \K[0-9]+ x [0-9]+"
```

After changing the config, reload and restart:
```bash
systemctl --user daemon-reload && systemctl --user restart synergy.service
```

### Running manually (without systemd)

```bash
LD_PRELOAD=~/.local/lib/synergy-eis-bridge.so \
SYNERGY_SCREEN_W=1920 \
SYNERGY_SCREEN_H=1080 \
/opt/Synergy/synergy-core --client --name $(hostname) SERVER_IP:24800
```

---

## Expected Output

Watch the logs while Synergy starts:

```bash
journalctl --user -u synergy.service -f
```

A successful connection produces output similar to:

```
[eis-bridge] screen size from env: 4080x1920
[eis-bridge] synergy-core load bias = 0x555555554000
[eis-bridge] patching xdp_session_connect_to_eis at 0x5555566939e
[eis-bridge] hot-patch installed: 0x5555566939e -> 0x7f...  (bridge_connect_to_eis)
[eis-bridge] intercepted xdp_session_connect_to_eis — creating EIS server
[eis-bridge] pointer uinput device created
[eis-bridge] keyboard uinput device created
[eis-bridge] EIS server ready, client fd = 7
NOTE: connected to server
[eis-bridge] EI client connected: ...
[eis-bridge] EIS devices added and resumed
```

The key line is `NOTE: connected to server` — Synergy is now stable.  Without
the patch it exits before this line ever appears.

---

## Troubleshooting

### "ERROR: unexpected bytes at target (expected endbr64)"

The offset `XDP_CONNECT_EIS_OFFSET` in the source does not match your version
of synergy-core.  Re-derive it:

```bash
nm /opt/Synergy/synergy-core | grep xdp_session_connect_to_eis
```

Update `XDP_CONNECT_EIS_OFFSET` in `src/synergy-eis-bridge.c`, recompile, and
restart.  The `install.sh` script does this automatically.

### "open /dev/uinput failed: Permission denied"

Your user does not have write access to `/dev/uinput`.  Fix:

```bash
sudo setfacl -m u:$USER:rw /dev/uinput
```

For a permanent fix, install the udev rule (see step 4 above) and log out/in.

### Synergy still exits immediately

Check if the bridge is actually being loaded:
```bash
journalctl --user -u synergy.service | grep eis-bridge
```

If there are no `[eis-bridge]` lines, verify `LD_PRELOAD` is set:
```bash
systemctl --user show synergy.service | grep LD_PRELOAD
```

### Mouse/keyboard works but cursor jumps erratically

The screen dimensions are wrong.  Update `SYNERGY_SCREEN_W` / `SYNERGY_SCREEN_H`
to match your actual desktop size.

### "invalid message from client: DINF"

Screen dimensions exceed 32767 (SInt16 max).  Use smaller values.

### Synergy connects but disconnects every ~9 seconds

Old symptom of the DINF bug.  Check your screen dimensions.

### Clipboard not syncing (Windows→Linux)

1. Check that the bridge service is running:
   ```bash
   systemctl --user status synergy-clip-bridge.service
   ```
2. Verify Synergy is actually receiving the Windows clipboard:
   ```bash
   grep "clipboard was updated" ~/.local/state/Synergy/synergy.log | tail -5
   ```
   If nothing appears after switching screens from Windows, the Synergy
   **server** (Windows process) is not detecting clipboard changes.  Restart
   Synergy on Windows.

3. If Synergy does receive the clipboard but it still doesn't paste on Linux:
   ```bash
   DISPLAY=:1 XAUTHORITY=$(cat /proc/$(pgrep -f synergy-clip-bridge.sh)/environ \
     | tr '\0' '\n' | grep ^XAUTHORITY= | cut -d= -f2-) \
     xclip -selection clipboard -o
   wl-paste
   ```
   Both should show the same content.  If X11 has the content but Wayland does
   not, the bridge poll hasn't fired yet — wait 0.4 s or check service logs.

### Clipboard not syncing (Linux→Windows)

1. Copy text on Linux and check that X11 clipboard was updated:
   ```bash
   DISPLAY=:1 XAUTHORITY=/run/user/1000/xauth_* xclip -selection clipboard -o
   ```
2. Move the cursor to Windows and check the Windows clipboard.
3. If X11 has content but Synergy hasn't forwarded it, check Synergy log for
   clipboard events after you exit the Linux screen.

### Stale clipboard content after bridge restart

If you see unexpected old content (e.g. test strings from a previous session),
delete the hash file to reset the bridge state:
```bash
rm /tmp/synergy-clip-last-hash
systemctl --user restart synergy-clip-bridge.service
```

### Hot-patch log shows bias = 0

The library was loaded by a process other than synergy-core (e.g. a helper
binary the service invokes first).  The bridge skips patching when it cannot
find `synergy-core` in `/proc/self/maps`, so this is safe.

### EI client never connects (no "EI client connected" log line)

synergy-core's libei stack may have an initialization timing issue with some
Synergy versions.  File a bug report including your Synergy version and the
full `journalctl` output.

---

## Tested On

| OS | Desktop | Synergy |
|---|---|---|
| Ubuntu 22.04 LTS | KDE Plasma 5.27 / KWin 5.27 Wayland | 3.3.1 |

This fix is x86-64 specific (the trampoline uses x86-64 opcodes).  ARM64 and
other architectures would need a different trampoline sequence.

---

## How It Works

At a high level:

```
synergy-core                 bridge .so
─────────────────            ──────────────────────────────────────────
startup: dlopen(.so)  ──►    bridge_init() runs (constructor)
                             ├─ find bias via /proc/self/maps
                             └─ hot-patch xdp_session_connect_to_eis

...later in main():
calls xdp_sess..._to_eis ──► bridge_connect_to_eis()
                             ├─ create libeis server (eis_setup_backend_fd)
                             ├─ get client fd (eis_backend_fd_add_client)
                             ├─ spawn eis_server_thread (background)
                             └─ return client fd to synergy-core

synergy-core uses fd         eis_server_thread polls EIS fd
as normal libei client ──►  ├─ handle CLIENT_CONNECT → add seat/devices
                             ├─ handle POINTER_MOTION → uinput REL_X/Y
                             ├─ handle POINTER_MOTION_ABSOLUTE → ABS_X/Y
                             ├─ handle BUTTON_BUTTON → EV_KEY BTN_*
                             ├─ handle SCROLL_DELTA → REL_WHEEL
                             └─ handle KEYBOARD_KEY → EV_KEY

                             uinput
                             └─ kernel input layer → KWin → apps
```

For the full binary analysis including disassembly references, call chain
reconstruction, and trampoline details, see [TECHNICAL.md](TECHNICAL.md).

---

## Technical Deep-Dive

See [TECHNICAL.md](TECHNICAL.md).

---

## Uninstall

```bash
./scripts/uninstall.sh
```

Or manually:
```bash
systemctl --user stop synergy.service
rm ~/.config/systemd/user/synergy.service.d/wayland-fix.conf
rm ~/.local/lib/synergy-eis-bridge.so
systemctl --user daemon-reload
systemctl --user start synergy.service
```

---

## License

[MIT License](LICENSE) — do whatever you want with it.  If you improve it,
please open a pull request so others can benefit.

---

*If this fixed Synergy for you, consider posting in the Synergy community
forums or GitHub issues — the more people who know about this fix, the better.*
