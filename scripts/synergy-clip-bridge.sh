#!/usr/bin/env bash
# synergy-clip-bridge v4
# Two-way clipboard bridge: Synergy X11 <-> KDE Plasma 5 Wayland
# Needed because KWin 5.27 XWayland clipboard sync is broken.
#
# Key design notes:
#  - Synergy uses XSetSelectionOwner with a stale X11 event timestamp when the
#    screen is entered via keyboard shortcut (EIS/uinput, no fresh X11 events).
#    ICCCM rejects ownership if a newer-timestamped owner (our xclip) exists.
#    FIX: kill xclip IMMEDIATELY on "entering screen" so there is no competing
#    owner and Synergy's stale-timestamp call succeeds.
#  - Hash-file dedup (persists across restarts, shared with log-watcher subshell)
#  - Singleton flock guard prevents duplicate bridge processes

set -u

# -- Singleton guard -----------------------------------------------------------
LOCKFILE=/tmp/synergy-clip-bridge.lock
exec 9>"$LOCKFILE"
flock -n 9 || { echo "synergy-clip-bridge: already running, exiting" >&2; exit 1; }
trap 'flock -u 9' EXIT

# -- Environment --------------------------------------------------------------
export DISPLAY=:1

if [[ -z "${XAUTHORITY:-}" ]]; then
    KWIN_PID=$(pgrep kwin_wayland | head -1)
    XAUTHORITY=$(cat /proc/"$KWIN_PID"/environ 2>/dev/null \
                 | tr '\0' '\n' | grep '^XAUTHORITY=' | cut -d= -f2-)
fi
export XAUTHORITY

# -- Wait until X11 accepts connections (up to 30 s) --------------------------
WAIT=0
until timeout 1 xclip -selection clipboard -o > /dev/null 2>&1 \
      || [[ $WAIT -ge 30 ]]; do
    sleep 1; WAIT=$(( WAIT + 1 ))
done

# -- Persistent dedup + xclip-PID state files ---------------------------------
# Files (not shell vars) so state is shared with log-watcher subshell and
# survives bridge restarts.
HASH_FILE=/tmp/synergy-clip-last-hash
XCLIP_PID_FILE=/tmp/synergy-bridge-xclip-pid

_init=$(timeout 0.5 xclip -selection clipboard -o 2>/dev/null \
        || wl-paste 2>/dev/null || true)
printf '%s' "$(printf '%s' "${_init:-}" | md5sum | cut -c1-32)" > "$HASH_FILE"
: > "$XCLIP_PID_FILE"

# -- Fast path: "entering screen" ---------------------------------------------
# Kill bridge xclip IMMEDIATELY (step 1, before any sleep) so Synergy can call
# XSetSelectionOwner without competing against a fresher-timestamped owner.
# Then wait 0.4s and read X11 (Synergy's Windows clipboard content).
SYNERGY_LOG="${HOME}/.local/state/Synergy/synergy.log"
tail -F "$SYNERGY_LOG" 2>/dev/null \
    | grep --line-buffered "entering screen" \
    | while IFS= read -r _; do
        # Step 1: kill xclip NOW before Synergy calls XSetSelectionOwner
        xclip_pid=$(cat "$XCLIP_PID_FILE" 2>/dev/null || true)
        if [[ -n "$xclip_pid" ]] && kill -0 "$xclip_pid" 2>/dev/null; then
            kill "$xclip_pid" 2>/dev/null || true
        fi
        : > "$XCLIP_PID_FILE"

        # Step 2: brief pause for Synergy to call XSetSelectionOwner
        sleep 0.4

        # Step 3: read X11 (Synergy's Windows content, if received)
        cur_x11=$(timeout 0.5 xclip -selection clipboard -o 2>/dev/null || true)

        if [[ -z "$cur_x11" ]]; then
            # Synergy had no new Windows clipboard -- re-push Wayland->X11
            # so the Linux clipboard remains accessible to X11 apps and Synergy
            cur_wl=$(wl-paste 2>/dev/null || true)
            if [[ -n "$cur_wl" ]]; then
                printf '%s' "$cur_wl" | xclip -selection clipboard -loops 0 2>/dev/null &
                printf '%s' "$!" > "$XCLIP_PID_FILE"
            fi
            continue
        fi

        h=$(printf '%s' "$cur_x11" | md5sum | cut -c1-32)
        last=$(cat "$HASH_FILE" 2>/dev/null || true)
        [[ "$h" = "$last" ]] && continue

        cur_wl=$(wl-paste 2>/dev/null || true)
        [[ "$cur_x11" = "$cur_wl" ]] && { printf '%s' "$h" > "$HASH_FILE"; continue; }
        printf '%s' "$cur_x11" | wl-copy 2>/dev/null || true
        printf '%s' "$h" > "$HASH_FILE"
    done &

# -- Main sync loop -----------------------------------------------------------
XCLIP_PID=""

while true; do
    sleep 0.4

    cur_x11=$(timeout 0.5 xclip -selection clipboard -o 2>/dev/null || true)
    cur_wl=$(wl-paste 2>/dev/null || true)

    [[ -z "$cur_x11" && -z "$cur_wl" ]] && continue

    h_x11=$(printf '%s' "$cur_x11" | md5sum | cut -c1-32)
    h_wl=$(printf '%s'  "$cur_wl"  | md5sum | cut -c1-32)
    last=$(cat "$HASH_FILE" 2>/dev/null || true)

    [[ "$h_x11" = "$last" && "$h_wl" = "$last" ]] && continue

    if [[ "$cur_x11" = "$cur_wl" ]]; then
        printf '%s' "$h_x11" > "$HASH_FILE"
        continue
    fi

    # X11 changed -> Synergy received Windows clipboard -> push to Wayland
    if [[ -n "$cur_x11" && "$h_x11" != "$last" ]]; then
        printf '%s' "$cur_x11" | wl-copy 2>/dev/null || true
        printf '%s' "$h_x11" > "$HASH_FILE"

    # Wayland changed -> Linux app copied -> push to X11 for Synergy to detect
    elif [[ -n "$cur_wl" && "$h_wl" != "$last" ]]; then
        if [[ -n "$XCLIP_PID" ]] && kill -0 "$XCLIP_PID" 2>/dev/null; then
            kill "$XCLIP_PID" 2>/dev/null || true
            wait "$XCLIP_PID" 2>/dev/null || true
        fi
        printf '%s' "$cur_wl" | xclip -selection clipboard -loops 0 2>/dev/null &
        XCLIP_PID=$!
        printf '%s' "$XCLIP_PID" > "$XCLIP_PID_FILE"
        printf '%s' "$h_wl" > "$HASH_FILE"
    fi
done
