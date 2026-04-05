#!/usr/bin/env bash
# uninstall.sh — remove the Synergy EIS bridge + clipboard bridge

set -euo pipefail

SO_FILE="$HOME/.local/lib/synergy-eis-bridge.so"
SYSTEMD_DROP_IN="$HOME/.config/systemd/user/synergy.service.d/wayland-fix.conf"
CLIP_BRIDGE_SCRIPT="$HOME/.local/bin/synergy-clip-bridge.sh"
CLIP_BRIDGE_SERVICE="$HOME/.config/systemd/user/synergy-clip-bridge.service"
AUTOSTART_DESKTOP="$HOME/.config/autostart/synergy-xenv.desktop"
UDEV_RULE="/etc/udev/rules.d/99-uinput-user.rules"

BOLD=$'\e[1m'; GREEN=$'\e[32m'; YELLOW=$'\e[33m'; RESET=$'\e[0m'
info()    { echo "${GREEN}[+]${RESET} $*"; }
warn()    { echo "${YELLOW}[!]${RESET} $*"; }
heading() { echo; echo "${BOLD}── $* ──${RESET}"; }

heading "Stopping services"
if systemctl --user is-active --quiet synergy.service 2>/dev/null; then
    systemctl --user stop synergy.service
    info "synergy.service stopped"
fi
if systemctl --user is-active --quiet synergy-clip-bridge.service 2>/dev/null; then
    systemctl --user stop synergy-clip-bridge.service
    info "synergy-clip-bridge.service stopped"
fi
systemctl --user disable synergy-clip-bridge.service 2>/dev/null || true

heading "Removing clipboard bridge"
[[ -f "$CLIP_BRIDGE_SERVICE" ]] && rm -f "$CLIP_BRIDGE_SERVICE" && info "Removed: $CLIP_BRIDGE_SERVICE"
[[ -f "$CLIP_BRIDGE_SCRIPT" ]]  && rm -f "$CLIP_BRIDGE_SCRIPT"  && info "Removed: $CLIP_BRIDGE_SCRIPT"
[[ -f "$AUTOSTART_DESKTOP" ]]   && rm -f "$AUTOSTART_DESKTOP"   && info "Removed: $AUTOSTART_DESKTOP"

heading "Removing systemd drop-in"
if [[ -f "$SYSTEMD_DROP_IN" ]]; then
    rm -f "$SYSTEMD_DROP_IN"
    info "Removed: $SYSTEMD_DROP_IN"
    # remove the dir if now empty
    rmdir --ignore-fail-on-non-empty "$(dirname "$SYSTEMD_DROP_IN")"
else
    warn "Drop-in not found (already removed?): $SYSTEMD_DROP_IN"
fi
systemctl --user daemon-reload
info "systemd user daemon reloaded"

heading "Removing shared library"
if [[ -f "$SO_FILE" ]]; then
    rm -f "$SO_FILE"
    info "Removed: $SO_FILE"
else
    warn "Library not found: $SO_FILE"
fi

heading "Removing udev rule"
if [[ -f "$UDEV_RULE" ]]; then
    echo -n "Remove udev rule $UDEV_RULE? [y/N] "
    read -r ans
    if [[ "${ans,,}" == "y" ]]; then
        sudo rm -f "$UDEV_RULE"
        sudo udevadm control --reload-rules
        info "udev rule removed"
    else
        warn "Skipped udev rule removal"
    fi
else
    warn "udev rule not found (already removed?): $UDEV_RULE"
fi

heading "Restarting Synergy"
if systemctl --user list-unit-files synergy.service &>/dev/null; then
    systemctl --user start synergy.service
    info "synergy.service restarted without the bridge"
fi

heading "Done"
echo "The EIS bridge has been removed.  Synergy will now behave as it did before."
echo "On KDE Plasma 5 Wayland it will likely exit immediately again."
