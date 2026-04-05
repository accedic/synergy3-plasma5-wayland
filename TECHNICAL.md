# Technical Deep-Dive — Synergy EIS Bridge

This document explains the root-cause chain that breaks Synergy 3.x on KDE
Plasma 5 Wayland, why the obvious fix does not work, and how the hot-patch
approach solves it at the binary level.

---

## Table of Contents

1. [Environment](#environment)
2. [Call chain inside synergy-core](#call-chain)
3. [Root Cause 1 — No EIS socket in Plasma 5](#root-cause-1)
4. [Root Cause 2 — Static linking defeats LD_PRELOAD](#root-cause-2)
5. [Root Cause 3 — SInt16 screen dimensions](#root-cause-3)
6. [The Hot-Patch Solution](#the-hot-patch-solution)
    - [Finding the load bias](#finding-the-load-bias)
    - [The 14-byte trampoline](#the-14-byte-trampoline)
    - [Why not a 5-byte rel32 JMP?](#why-not-a-5-byte-rel32-jmp)
    - [Why not dlsym?](#why-not-dlsym)
    - [Why not popen/xrandr in the constructor?](#why-not-popenxrandr-in-the-constructor)
7. [The EIS Server](#the-eis-server)
8. [Event flow summary](#event-flow-summary)

---

## Environment

| Component | Version |
|---|---|
| OS | Ubuntu 22.04 LTS |
| Desktop | KDE Plasma 5.27.12 |
| Compositor | KWin 5.27.11 (Wayland) |
| Synergy | 3.3.1 |
| Binary | `/opt/Synergy/synergy-core` |
| Server | Windows, Synergy 3.3.1 |

---

## Call Chain

When synergy-core starts and an XDP Remote Desktop portal session becomes
active, the following call chain fires (reconstructed from disassembly and
glib signals):

```
cb_session_started(session, result, user_data)   [callback, xdp_session_start_finish]
  └─ xdp_session_start_finish(session, result)   → returns TRUE on Plasma 5
       → triggers g_signal_emit "session-started"
           └─ request_eis(session)
                └─ xdp_session_connect_to_eis(session, NULL)
                     → returns -1  (Plasma 5: no EIS socket)
                └─ if (fd < 0):  [js instruction at 0xd3f91]
                     └─ g_main_loop_quit(loop)   → process exits
```

The critical branch is at binary file offset `0xd3f88`–`0xd3f95`:

```asm
; 0xd3f88
callq  0x13499e          ; e8 11 0a 06 00 — direct call to xdp_session_connect_to_eis
; 0xd3f8d  (rax = returned fd)
test   eax, eax
; 0xd3f91
js     0xd4013           ; jump if signed (i.e. if eax < 0)
; 0xd4013
callq  g_main_loop_quit
```

The process exits in approximately **0.15 seconds** from startup.

---

## Root Cause 1 — No EIS socket in Plasma 5

**EIS** (Emulated Input Server) is the Wayland mechanism that allows portal
clients (like Synergy) to inject keyboard/mouse events into the session.  It
was designed specifically to replace the old X11 `XTest` injection API.

Timeline:
- EIS protocol designed and merged into `libei` / `libeis`
- KDE Plasma 6 / KWin 6 added KWin's EIS compositor support
- Synergy 3.x adopted `xdp_session_connect_to_eis()` as its input injection method
- **KDE Plasma 5 never received EIS support** — it was not backported to KWin 5.x

When `xdp_session_connect_to_eis()` is called on Plasma 5, the XDG Desktop
Portal's Remote Desktop implementation asks KWin for an EIS socket fd.  KWin
responds "not supported" and the portal returns `-1` to the caller.

---

## Root Cause 2 — Static linking defeats LD_PRELOAD

### How LD_PRELOAD interception works (when it works)

The dynamic linker resolves undefined symbols from shared libraries at
runtime.  For functions called from a dynamically-linked binary:

1. The compiler generates a call through the **PLT** (Procedure Linkage Table):
   ```asm
   callq  plt_entry_for_xdp_session_connect_to_eis
   ```
2. The PLT entry jumps through the **GOT** (Global Offset Table):
   ```asm
   jmpq   *[got_slot_for_xdp_session_connect_to_eis]
   ```
3. On first call, the PLT stub invokes the dynamic linker's resolver
   (`_dl_runtime_resolve`), which searches `LD_PRELOAD` libraries first.
4. If our library exports `xdp_session_connect_to_eis`, the resolver uses
   our symbol and patches the GOT slot to point to it.

This is the standard mechanism behind every LD_PRELOAD wrapper ever written.

### Why it does not work for synergy-core

Inspect the symbol table:

```bash
$ nm /opt/Synergy/synergy-core | grep xdp_session_connect_to_eis
000000000013499e T xdp_session_connect_to_eis
```

The `T` type means the symbol is **defined** in the binary's own `.text`
section at file offset `0x13499e`.  This means **libportal was statically
linked** into synergy-core.  The Synergy build system compiled libportal
source directly into the binary.

Confirm: `ldd` shows no libportal:
```bash
$ ldd /opt/Synergy/synergy-core | grep -E 'libei|portal'
    libei.so.1 => /lib/x86_64-linux-gnu/libei.so.1
```

`libei` (the client-side EI library used by synergy-core's input consumer) is
a *dynamic* dependency.  `libportal` (which provides `xdp_session_connect_to_eis`)
is *absent* from the dynamic section — it was compiled in.

The call at `0xd3f88` is:
```
e8 11 0a 06 00   callq  0x13499e
```

`e8` is a relative near call.  The operand `11 0a 06 00` is the signed 32-bit
displacement from the next instruction:

```
0xd3f8d + 0x00060a11 = 0x13499e  ✓
```

This is a **direct call** hardwired into the binary.  There is no PLT entry, no
GOT slot, and therefore nothing the dynamic linker can redirect.

### Comparison: T symbol vs U symbol

| nm type | Meaning | LD_PRELOAD works? |
|---|---|---|
| `T` | Defined in this binary's text segment (statically linked) | **No** — direct call, no PLT |
| `U` | Undefined — resolved dynamically from shared libraries | **Yes** — goes through PLT/GOT |

---

## Root Cause 3 — SInt16 screen dimensions

Synergy's wire protocol encodes screen width and height in `DINF` messages
using `SInt16` — a signed 16-bit integer (range -32768 to +32767).

The default screen size reported by older versions of synergy-core on Wayland
was `65535 × 65535` (the value used when no explicit size was configured).
In two's complement 16-bit arithmetic:

```
65535 = 0xFFFF = -1 (as int16_t)
```

The Synergy server receives `-1 × -1` as the screen dimensions, considers
this invalid, logs *"invalid message from client HOSTNAME: DINF"*, and drops
the connection.  The client reconnects, sends DINF again, gets dropped again.
This produced a secondary disconnect loop every ~9 seconds on top of the
primary crash loop.

**Fix:** Pass the real combined desktop dimensions:
```bash
xrandr | grep "^Screen 0" | grep -oP "current \K[0-9]+ x [0-9]+"
# → 4080 x 1920
```

Set `SYNERGY_SCREEN_W=4080` and `SYNERGY_SCREEN_H=1920` in the systemd
drop-in.  The bridge reads these via `getenv()` and passes them when creating
the EIS region and uinput device extents.

---

## The Hot-Patch Solution

Since LD_PRELOAD symbol interception is not viable, we need to modify the
binary's behavior at runtime.  The approach:

1. Load a `.so` via `LD_PRELOAD` (this still works — we just cannot override
   symbols via the dynamic linker).
2. Use `__attribute__((constructor))` to run code *before* `main()`.
3. From within that constructor, **overwrite the target function** in memory
   to jump to our replacement.

### Finding the load bias

The binary is PIE (Position-Independent Executable), so ASLR randomizes where
it loads.  We need the load bias (difference between file offsets and runtime
virtual addresses).

Linux maps a PIE binary's ELF segments with the first segment at file offset 0.
The virtual address of that mapping **is** the load bias:

```
VA = file_offset + load_bias
```

Since the first segment always has `file_offset == 0`:

```
load_bias = VA_of_segment_with_file_offset_0
```

We parse `/proc/self/maps` looking for a mapping from `synergy-core` with
file offset 0:

```c
while (fgets(line, sizeof(line), f)) {
    if (!strstr(line, "synergy-core")) continue;
    unsigned long va_start, va_end, file_off;
    sscanf(line, "%lx-%lx %*s %lx", &va_start, &va_end, &file_off);
    if (file_off == 0) { bias = va_start; break; }
}
```

Example maps entry:
```
555555554000-555556e24000 r--p 00000000 08:01 1234567 /opt/Synergy/synergy-core
```

Here `file_off = 0x00000000`, so `bias = 0x555555554000`.

```
target = bias + 0x13499e = 0x555555554000 + 0x13499e = 0x5555566939e
```

**Safety check:** verify the expected `endbr64` prologue (`f3 0f 1e fa`) at
`target` before writing anything.  If the bytes differ, the offset is wrong
for this binary version and we abort rather than corrupt code.

### The 14-byte trampoline

We cannot use a standard 5-byte relative JMP (`e9 <rel32>`) because:

- The `.so` is loaded far from the binary in virtual memory (several petabytes
  apart in typical ASLR layouts).
- A signed 32-bit displacement can reach at most ±2 GiB from the JMP
  instruction.  The distance here is far beyond that range.

We use a **14-byte absolute indirect JMP** instead:

```
offset  bytes                    meaning
+0      ff 25 00 00 00 00        jmpq *[rip + 0]
+6      <8-byte little-endian>   absolute destination address
```

`ff 25` is `jmpq *r/m64` with a `RIP-relative` addressing mode.
The 32-bit displacement is `00 00 00 00` = 0.  After the processor fetches
the instruction (6 bytes), RIP points to `target + 6`.  The CPU dereferences
the 8-byte value at `[rip+0]` = `[target+6]` and jumps to it.

This works regardless of where the `.so` was mapped — the absolute address is
embedded directly in the 8 bytes following the JMP instruction.

Encoding (x86-64 little-endian):

```c
uintptr_t dst = (uintptr_t)bridge_connect_to_eis;

target[0] = 0xff;                               // jmpq
target[1] = 0x25;                               // *[rip + disp32]
target[2] = target[3] = target[4] = target[5] = 0x00; // disp32 = 0
*((uint64_t *)(target + 6)) = dst;              // 8-byte absolute address
```

Memory fence instructions bracket the writes to prevent compiler and CPU
reordering:

```c
__atomic_thread_fence(__ATOMIC_SEQ_CST);
// ... write bytes ...
__atomic_thread_fence(__ATOMIC_SEQ_CST);
```

After writing, `mprotect` restores `PROT_READ | PROT_EXEC` (removing `PROT_WRITE`).

### Why not a 5-byte rel32 JMP?

A `e9 <disp32>` JMP encodes a signed 32-bit offset from the instruction after
the JMP to the destination.  This can reach ±2 GiB.

With ASLR:
- `synergy-core` loads at e.g. `0x555555554000`
- Our `.so` loads at e.g. `0x7f3e12300000`
- Distance: `0x7f3e12300000 - 0x555555554000 ≈ 0x29e8bcbac000` ≈ 46 TiB

46 TiB >> 2 GiB, so `rel32` cannot reach.  14-byte absolute works for
any address in the 64-bit virtual address space.

### Why not dlsym?

One alternative is to use `dlsym(RTLD_DEFAULT, "xdp_session_connect_to_eis")`
to get the address of the function in the binary and then overwrite it.

This fails because `RTLD_DEFAULT` searches the **dynamic symbol table** (`.dynsym`).
Since libportal was statically linked, `xdp_session_connect_to_eis` is not in
the dynamic symbol table — only in the full `.symtab` which is a debugging
aid and not used by the linker at runtime.

```bash
$ nm -D /opt/Synergy/synergy-core | grep xdp_session_connect_to_eis
# (no output — not in .dynsym)
$ nm    /opt/Synergy/synergy-core | grep xdp_session_connect_to_eis
000000000013499e T xdp_session_connect_to_eis   # only in .symtab
```

`dlsym` returns NULL.  We must compute the address manually using the load
bias approach.

### Why not popen/xrandr in the constructor?

The constructor `bridge_init()` runs before `main()` — which means before the
Wayland socket connection, before GLib's event loop, and before the application
has obtained a display connection.

Calling `popen("xrandr")` / `system()` in this context would:
1. `fork()` — which duplicates file descriptors and internal state
2. `exec("xrandr")` — which tries to connect to the Wayland display
3. Block waiting for the compositor to respond — but the compositor is
   waiting for our process to finish initializing

This is a classic deadlock.  The solution is to pass screen dimensions via
environment variables set in the systemd drop-in, which are already in the
process's environment by the time the constructor runs.

---

## The EIS Server

Once the trampoline is in place, `bridge_connect_to_eis()` runs whenever
synergy-core calls `xdp_session_connect_to_eis()`.

**EIS** (Emulated Input Server) is a socket-based protocol.  On the server
side we use `libeis`:

```c
struct eis *eis = eis_new(NULL);
eis_setup_backend_fd(eis);              // creates a server that uses fds
int client_fd = eis_backend_fd_add_client(eis);   // returns client-side fd
```

`client_fd` is the file descriptor that the *client* (synergy-core's libei)
will use to talk to our server.  We return this to synergy-core as the result
of `xdp_session_connect_to_eis()`.

Synergy-core's libei then:
1. Sends `CLIENT_CONNECT` with its capabilities
2. Waits for `SEAT_BIND` acknowledgement
3. Registers devices and starts emulating input

A background thread on our side handles all of this and relays events to the
kernel via `/dev/uinput`.

### uinput devices

Two virtual input devices are created:

| Device | Name | Events |
|---|---|---|
| Pointer | "Synergy EIS Pointer" | `EV_REL` (X/Y), `EV_ABS` (X/Y), `EV_KEY` (BTN_*), `EV_SYN` |
| Keyboard | "Synergy EIS Keyboard" | `EV_KEY` (0..KEY_MAX), `EV_SYN` |

The pointer registers both relative (`REL_X`/`REL_Y`) and absolute
(`ABS_X`/`ABS_Y`) capabilities, matching the EIS device capabilities:

- `EIS_DEVICE_CAP_POINTER` — relative motion
- `EIS_DEVICE_CAP_POINTER_ABSOLUTE` — absolute motion
- `EIS_DEVICE_CAP_BUTTON` — mouse buttons
- `EIS_DEVICE_CAP_SCROLL` — scroll wheel
- `EIS_DEVICE_CAP_KEYBOARD` — keyboard keys

The EIS region matches the combined virtual desktop size so that absolute
coordinates map correctly.

---

## Event Flow Summary

```
Windows host
  │
  │  TCP/TLS port 24800  (Synergy protocol)
  ▼
synergy-core (Linux client)
  │
  │  libei client API — EIS socket (our client_fd)
  ▼
bridge eis_server_thread
  │
  ├─ EIS_EVENT_POINTER_MOTION         → uinput REL_X, REL_Y
  ├─ EIS_EVENT_POINTER_MOTION_ABSOLUTE→ uinput ABS_X, ABS_Y
  ├─ EIS_EVENT_BUTTON_BUTTON          → uinput EV_KEY BTN_*
  ├─ EIS_EVENT_SCROLL_DELTA           → uinput REL_WHEEL/REL_HWHEEL
  └─ EIS_EVENT_KEYBOARD_KEY           → uinput EV_KEY <keycode>
       │
       │  /dev/uinput "Synergy EIS Pointer"  + "Synergy EIS Keyboard"
       ▼
Linux kernel input subsystem
  │
  │  evdev events
  ▼
KWin Wayland compositor input stack
  │
  │  Wayland protocols (wl_pointer, wl_keyboard, etc.)
  ▼
Applications on the Wayland session
```

---

## Key Numbers (Synergy 3.3.1, x86-64)

| Symbol | File offset | nm type |
|---|---|---|
| `xdp_session_connect_to_eis` | `0x13499e` | `T` (statically linked) |
| Call site (`request_eis`) | `0xd3f88` | — |
| Signed-jump-on-fail | `0xd3f91` | — |
| `g_main_loop_quit` call | `0xd4013` | — |

These offsets are specific to Synergy 3.3.1.  For other versions, derive them:

```bash
nm /opt/Synergy/synergy-core | grep xdp_session_connect_to_eis
```
