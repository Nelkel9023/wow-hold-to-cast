# HoldToCast

Hold-to-cast for a 32-bit **World of Warcraft 3.3.5a client (build 12340)**.

Press and **hold** an action-bar hotkey and the bound spell or ability keeps
re-casting until you release the key, instead of firing only once. This is a
quality-of-life convenience similar to what "hold to repeat" gives in other
games.

Nothing here is a server, a bot, or an automated combat routine. The tool only
replays the exact key press you are already physically holding, through the
game's own input pipeline, at a fixed interval. It stops the instant you
release the key, and it never fires a key you are not holding.

> **This project was built with AI assistance.** The code, the reverse-engineered
> offsets, and this document were produced with the help of an AI coding
> assistant, iteratively and with human review and testing. It is published in
> full so anyone can read, audit, and modify it. See [Transparency](#transparency).

---

## What it does

- While you hold a key that is bound to an action button, the DLL repeats that
  key press every 100 ms.
- It only ever repeats keys the game itself is already handling as action-bar
  bindings. It does not invent input.
- It is gated three ways so it never misbehaves:
  1. **Binding gate** — the game must have delivered that key to the UI as a
     binding (`ExecKey`). This never happens while typing in a chat/edit box,
     so text never triggers casts.
  2. **Physical gate** — the key must still be physically down
     (`GetAsyncKeyState`). If the UI misses a key-up, the DLL notices and stops.
  3. **Focus gate** — the game window must be the foreground window.

## How it works

Two parts:

1. **`dinput8.dll`** — a proxy DLL. The client statically imports
   `DirectInput8Create` from `dinput8.dll`, so placing our DLL next to the
   executable makes Windows load it automatically at startup. We forward every
   export to the real system `dinput8.dll` and, on a worker thread, install our
   hooks.
2. **`HoldToCast` addon** — a tiny notifier. It only calls the Lua global
   `HOLDTOCAST_LOADED()` that the DLL publishes, and prints one line in the chat
   frame saying whether the native module is loaded. All behaviour lives in the
   DLL.

### Hooks

Hooking is done with [MinHook](https://github.com/TsudaKageyu/minhook)
(third-party, BSD-2-Clause — not vendored here, see [Build](#build)).

| Hooked function (build 12340)          | Address    | Purpose |
|----------------------------------------|------------|---------|
| `CGUIBindings::ExecKey`                | `0x563150` | Records which key strings are currently held. Fires only when the UI consumes a key as a binding. |
| `CGWorldFrame::OnLayerUpdate`          | `0x4FA040` | Runs once per frame; does the actual repeating. |
| `FrameScript_Initialize`               | `0x819BB0` | Re-publishes the `HOLDTOCAST_LOADED` Lua global (the script state is rebuilt on every call). |

Supporting reads (not hooked):

| Symbol                                | Address    |
|---------------------------------------|------------|
| `CGUIBindings::GetCommandKey`         | `0x55E750` |
| `CGUIBindings::s_bindings`            | `0xBEADD8` |
| `FrameScript_RegisterFunction`        | `0x817F90` |
| `lua_pushnumber`                      | `0x84E2A0` |

### Why it replays the key (and not `UseAction` directly)

The obvious approach — "call `UseAction(slot)` in a loop" — breaks on the main
action bar because those buttons are **page-relative**: `ACTIONBUTTON1..12`
mean different slots on page 1 vs page 2, and different bars again for the
stealth / druid shapeshift bonus bar. Calling a fixed slot always hits page 1.

Instead, the DLL resolves each `ACTIONBUTTONn` binding to its bound key string,
and while that key is held it **replays the key press** by calling the original
`ExecKey` with the same key. That routes through the client's own binding
handler, so paging, bonus bars, and any addon rebinding (some addons bind action
keys to `CLICK SomeFrame:LeftButton`) all keep working exactly as the game
intended.

## Repository layout

```
src/holdtocast.c        native core: hooks, held-key set, repeat loop, notifier
src/proxy_dinput8.c     dinput8 proxy, exports forwarding, DllMain bootstrap
addon/HoldToCast/       the notifier addon (.toc + .lua)
build/build.ps1         build script (zig cc)
README.md
LICENSE                 MIT
```

## Build

Requirements:

- [zig](https://ziglang.org/download/) (provides the C compiler via `zig cc`).
  Put it on `PATH`, or pass `-Zig <path>`.
- [MinHook](https://github.com/TsudaKageyu/minhook) sources placed at
  `third_party/minhook-master` (download and unzip the release so that
  `third_party/minhook-master/src/hook.c` exists). MinHook is not included
  here because it is a separate project with its own license.

Then:

```powershell
pwsh -File build/build.ps1
```

Output: `build/dinput8.dll` (32-bit, exports `DirectInput8Create` undecorated).

Notes on the toolchain quirks handled in the sources:

- zig wants undecorated export names, so exports use `__asm__("Name")` on the
  definition instead of `__declspec(dllexport)`.
- The 32-bit MinHook build needs `hde32.c` (the 64-bit path uses `hde64.c`).

## Install

1. Copy `build/dinput8.dll` next to the client's `.exe` (same folder).
2. Copy the `addon/HoldToCast` folder into `Interface/AddOns/`.
3. Start the client. You should see a chat line: `HoldToCast: DLL loaded ...`.

## Remove / rollback

Delete `dinput8.dll` from the client folder and remove the `HoldToCast` addon
folder. Nothing is written to the registry or persisted anywhere else.

## Tuning

Constants at the top of `src/holdtocast.c`:

- `CAST_THROTTLE_MS` (default `100`) — repeat interval while held.
- `BINDING_REFRESH_MS` (default `1000`) — how often key bindings are re-read.
- `NUM_ACTION_BUTTONS` (default `12`) — main-bar button count.

## Technical notes

- Build 12340, 32-bit, image base `0x400000`. All addresses above are absolute
  virtual addresses matching a clean 12340 client's `.text` section.
- `UseAction` is a static `__cdecl` function `(int32 slot, const void* target,
  const char* button)`; engine slots are 0-based. (Kept for reference; not used
  by the current implementation.)
- A diagnostic log `holdtocast.log` is written next to the DLL/executable.

## Transparency

- **AI-generated:** written with AI assistance, reviewed and tested by a human.
- **Full source:** everything that runs is in this repository. No network
  access, no telemetry, no obfuscation, no external data collection. The only
  file the program writes is `holdtocast.log`, next to the DLL.
- **No server implied:** this targets a client binary and its own input
  handling only. It contains no server addresses or server-specific behavior.

## License

MIT — see [LICENSE](LICENSE). MinHook is licensed separately under BSD-2-Clause.
