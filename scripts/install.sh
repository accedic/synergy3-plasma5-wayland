#!/usr/bin/env bash
# install.sh — install the Synergy EIS bridge on KDE Plasma 5 Wayland
#
# Run as your normal user (NOT root).  The script will sudo only when needed
# (package installation, udev rule).
#
# Prerequisites: Synergy 3.x installed to /opt/Synergy, active Synergy
# systemd user service, KDE Plasma 5 Wayland session.

set -euo pipefail

# ── constants ──────────────────────────────────────────────────────────────
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_FILE="$REPO_DIR/src/synergy-eis-bridge.c"
SYNERGY_BIN="${SYNERGY_BIN:-/opt/Synergy/synergy-core}"
LIB_DIR="$HOME/.local/lib"
SO_FILE="$LIB_DIR/synergy-eis-bridge.so"
SYSTEMD_DROP_IN="$HOME/.config/systemd/user/synergy.service.d/wayland-fix.conf"
UDEV_RULE="/etc/udev/rules.d/99-uinput-user.rules"

BOLD=$'\e[1m'; GREEN=$'\e[32m'; YELLOW=$'\e[33m'; RED=$'\e[31m'; RESET=$'\e[0m'
info()    { echo "${GREEN}[+]${RESET} $*";  }
warn()    { echo "${YELLOW}[!]${RESET} $*"; }
error()   { echo "${RED}[✗]${RESET} $*" >&2; }
heading() { echo; echo "${BOLD}── $* ──${RESET}"; }

# ── step 1: check we're on Plasma 5 Wayland ───────────────────────────────
heading "Environment check"
if [[ "${WAYLAND_DISPLAY:-}" == "" && "${XDG_SESSION_TYPE:-}" != "wayland" ]]; then
    warn "WAYLAND_DISPLAY is not set.  This fix is only needed on Wayland."
    warn "If you are on X11 with XWayland, Synergy may work without this patch."
fi
KDE_VER=$(kf5-config --version 2>/dev/null | grep -oP 'KDE Frameworks: \K[0-9]+' || echo "")
if [[ -n "$KDE_VER" && "$KDE_VER" -ge 6 ]]; then
    warn "KDE Frameworks $KDE_VER detected.  Plasma 6 has native EIS support;"
    warn "this patch may be unnecessary.  Continuing anyway."
fi

if [[ ! -x "$SYNERGY_BIN" ]]; then
    error "synergy-core not found at $SYNERGY_BIN"
    error "Set SYNERGY_BIN=/path/to/synergy-core and re-run."
    exit 1
fi
info "synergy-core: $SYNERGY_BIN"

# ── step 2: detect binary offset ─────────────────────────────────────────
heading "Symbol offset detection"
NM_OUT=$(nm "$SYNERGY_BIN" 2>/dev/null | grep xdp_session_connect_to_eis || true)
if [[ -z "$NM_OUT" ]]; then
    error "xdp_session_connect_to_eis not found in $SYNERGY_BIN."
    error "This binary does not use libportal's EIS call or nm is unavailable."
    exit 1
fi
OFFSET_HEX=$(echo "$NM_OUT" | awk '{print $1}')
SYMBOL_TYPE=$(echo "$NM_OUT" | awk '{print $2}')
info "Symbol: $NM_OUT"

if [[ "$SYMBOL_TYPE" != "T" ]]; then
    warn "Symbol type is '$SYMBOL_TYPE', not 'T' (statically compiled)."
    warn "If it is 'U', LD_PRELOAD symbol interception may work without hot-patching."
fi

# ── step 3: install build dependencies ───────────────────────────────────
heading "Build dependencies"
install_pkg() {
    dpkg -s "$1" &>/dev/null && info "$1 already installed" && return
    info "Installing $1 ..."
    sudo apt-get install -y "$1"
}

if command -v apt-get &>/dev/null; then
    install_pkg libeis-dev
    install_pkg gcc
elif command -v dnf &>/dev/null; then
    warn "Fedora/RHEL detected.  Attempting dnf install libeis-devel ..."
    sudo dnf install -y libeis-devel gcc
elif command -v pacman &>/dev/null; then
    warn "Arch detected.  Attempting pacman -S libeis ..."
    sudo pacman -S --noconfirm libeis
else
    warn "Cannot auto-install packages.  Please install libeis development headers manually."
fi

# Check that the header is reachable
EI_HEADER=$(find /usr/include -name "libeis.h" 2>/dev/null | head -1 || true)
if [[ -z "$EI_HEADER" ]]; then
    error "libeis.h not found.  Install libeis-dev / libeis-devel and retry."
    exit 1
fi
EI_INCLUDE_DIR=$(dirname "$EI_HEADER")
info "libeis headers: $EI_INCLUDE_DIR"

# ── step 4: patch offset into source ──────────────────────────────────────
heading "Source preparation"
PATCHED_SRC=$(mktemp /tmp/synergy-eis-bridge-XXXXXX.c)
trap 'rm -f "$PATCHED_SRC"' EXIT

sed "s/#define XDP_CONNECT_EIS_OFFSET.*/#define XDP_CONNECT_EIS_OFFSET 0x${OFFSET_HEX}UL/" \
    "$SRC_FILE" > "$PATCHED_SRC"

info "Offset 0x$OFFSET_HEX written to patched source"

# ── step 5: compile ────────────────────────────────────────────────────────
heading "Compilation"
mkdir -p "$LIB_DIR"
cc -O2 -shared -fPIC \
    -o "$SO_FILE" \
    "$PATCHED_SRC" \
    -I"$EI_INCLUDE_DIR" \
    -leis -ldl -lpthread
info "Compiled: $SO_FILE"

# ── step 6: /dev/uinput access ────────────────────────────────────────────
heading "/dev/uinput access"

if [[ -r /dev/uinput && -w /dev/uinput ]]; then
    info "/dev/uinput already accessible — nothing to do"
else
    info "Granting $USER access to /dev/uinput via udev rule ..."
    cat <<'EOF' | sudo tee "$UDEV_RULE" > /dev/null
KERNEL=="uinput", MODE="0660", GROUP="input", TAG+="uaccess", OPTIONS+="static_node=uinput"
EOF
    sudo udevadm control --reload-rules
    sudo udevadm trigger --name-match=uinput
    info "udev rule installed: $UDEV_RULE"
    info "You may need to log out and back in for the uaccess tag to take effect."
    # Fallback: ACL on current session
    if command -v setfacl &>/dev/null; then
        sudo setfacl -m u:"$USER":rw /dev/uinput
        info "ACL applied for current session: setfacl -m u:$USER:rw /dev/uinput"
    fi
fi

# ── step 7: detect screen size ────────────────────────────────────────────
heading "Screen size"
SCREEN_SIZE=$(xrandr 2>/dev/null | grep -oP '^Screen 0:.*current \K[0-9]+ x [0-9]+' | head -1 || true)
if [[ -n "$SCREEN_SIZE" ]]; then
    SW=$(echo "$SCREEN_SIZE" | awk '{print $1}')
    SH=$(echo "$SCREEN_SIZE" | awk '{print $3}')
    info "Detected virtual desktop: ${SW}x${SH}"
else
    warn "Could not detect screen size from xrandr; defaulting to 1920x1080"
    warn "Edit $SYSTEMD_DROP_IN after install to set SYNERGY_SCREEN_W/H."
    SW=1920
    SH=1080
fi

if (( SW > 32767 || SH > 32767 )); then
    warn "Screen size ${SW}x${SH} exceeds SInt16 max (32767)."
    warn "Synergy protocol will receive negative values and reject DINF."
    warn "Use the per-monitor size instead of the combined desktop."
fi

# ── step 8: write systemd drop-in ─────────────────────────────────────────
heading "Systemd drop-in"
DROP_IN_DIR=$(dirname "$SYSTEMD_DROP_IN")
mkdir -p "$DROP_IN_DIR"

cat > "$SYSTEMD_DROP_IN" <<EOF
# Generated by synergy-eis-bridge install.sh
[Service]
Environment="LD_PRELOAD=$SO_FILE"
Environment="SYNERGY_SCREEN_W=$SW"
Environment="SYNERGY_SCREEN_H=$SH"
EOF

info "Drop-in: $SYSTEMD_DROP_IN"

# ── step 9: clipboard bridge ──────────────────────────────────────────────
heading "Clipboard bridge (X11 <-> Wayland)"
# KWin 5.27 does not reliably sync the clipboard between XWayland and Wayland.
# Install a lightweight polling bridge that covers both directions.

CLIP_BRIDGE_SCRIPT="$HOME/.local/bin/synergy-clip-bridge.sh"
CLIP_BRIDGE_SERVICE="$HOME/.config/systemd/user/synergy-clip-bridge.service"
AUTOSTART_DESKTOP="$HOME/.config/autostart/synergy-xenv.desktop"

# Install xclip if missing (wl-clipboard is a hard dep, usually present)
if command -v apt-get &>/dev/null; then
    install_pkg xclip
    install_pkg wl-clipboard
fi

mkdir -p "$HOME/.local/bin"
cp "$REPO_DIR/scripts/synergy-clip-bridge.sh" "$CLIP_BRIDGE_SCRIPT"
chmod +x "$CLIP_BRIDGE_SCRIPT"
info "Bridge script: $CLIP_BRIDGE_SCRIPT"

cp "$REPO_DIR/systemd/synergy-clip-bridge.service" "$CLIP_BRIDGE_SERVICE"
info "Bridge service: $CLIP_BRIDGE_SERVICE"

# KDE autostart: import XAUTHORITY at login and restart both services
mkdir -p "$(dirname "$AUTOSTART_DESKTOP")"
cp "$REPO_DIR/autostart/synergy-xenv.desktop" "$AUTOSTART_DESKTOP"
info "Autostart entry: $AUTOSTART_DESKTOP"

# ── step 10: reload and restart synergy + clipboard bridge ────────────────
heading "Restarting Synergy + clipboard bridge"
systemctl --user daemon-reload
systemctl --user enable synergy-clip-bridge.service
if systemctl --user import-environment DISPLAY XAUTHORITY 2>/dev/null; then
    true  # imported successfully
fi

if systemctl --user is-active --quiet synergy.service 2>/dev/null; then
    systemctl --user restart synergy.service
    info "synergy.service restarted"
elif systemctl --user list-unit-files synergy.service &>/dev/null; then
    systemctl --user start synergy.service
    info "synergy.service started"
else
    warn "No synergy.service unit found."
    warn "Start Synergy manually with:"
    warn "  LD_PRELOAD=$SO_FILE SYNERGY_SCREEN_W=$SW SYNERGY_SCREEN_H=$SH $SYNERGY_BIN"
fi

systemctl --user restart synergy-clip-bridge.service && info "synergy-clip-bridge.service started" || \
    warn "Could not start clipboard bridge service (may need re-login for XAUTHORITY)"

# ── done ─────────────────────────────────────────────────────────────────
heading "Done"
echo
echo "Installation complete.  To verify, watch the Synergy logs:"
echo
echo "  journalctl --user -u synergy.service -f"
echo
echo "A successful connection looks like:"
echo "  [eis-bridge] hot-patch installed: ..."
echo "  [eis-bridge] EIS server ready, client fd = N"
echo "  [eis-bridge] pointer uinput device created"
echo "  [eis-bridge] keyboard uinput device created"
echo "  NOTE: connected to server"
echo
echo "Check clipboard bridge:"
echo "  journalctl --user -u synergy-clip-bridge.service -f"
echo
echo "If the offset check fails (unexpected bytes), re-run:"
echo "  nm $SYNERGY_BIN | grep xdp_session_connect_to_eis"
echo "and update XDP_CONNECT_EIS_OFFSET in src/synergy-eis-bridge.c, then"
echo "re-run this script."
