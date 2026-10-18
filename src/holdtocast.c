/* HoldToCast native core.
 *
 * All logic lives here. The companion addon only calls HOLDTOCAST_LOADED()
 * once to report whether this DLL is present.
 *
 * Two cooperating hooks, both on the main thread:
 *   - CGUIBindings::ExecKey fires for a key only when the game UI consumes it
 *     as a binding (never while typing in a chat/edit box, never when the
 *     window is unfocused). It records which key strings are currently held.
 *   - CGWorldFrame::OnLayerUpdate runs every frame. For each action button's
 *     bound key that is held and physically down, it *replays* the key press
 *     through the game's own binding dispatch (the original ExecKey).
 *
 * Replaying the binding instead of calling UseAction directly means all of the
 * client's action-bar logic is preserved: page 1-6 paging, the bonus bar
 * (stealth / druid shapeshift), and any rebinding (some addons bind action
 * keys to "CLICK <frame>:LeftButton" instead of the plain ACTIONBUTTONn
 * command).
 */
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

#include "MinHook.h"

#define WOW_IMAGEBASE 0x00400000u
static __inline void* va(uintptr_t a) {
    return (void*)((uintptr_t)GetModuleHandleA(NULL) + (a - WOW_IMAGEBASE));
}

/* ---- client addresses (VAs, verified against Wow.exe) ---- */
#define VA_CGWorldFrame_OnLayerUpdate    0x004FA040u
#define VA_CGUIBindings_GetCommandKey    0x0055E750u
#define VA_CGUIBindings_ExecKey          0x00563150u
#define VA_s_bindings                    0x00BEADD8u
#define VA_FrameScript_RegisterFunction  0x00817F90u
#define VA_FrameScript_Initialize        0x00819BB0u
#define VA_lua_pushnumber                0x0084E2A0u

#define BINDING_MODE_ACTIVE 4
#define NUM_ACTION_BUTTONS  12
#define CAST_THROTTLE_MS    100
#define BINDING_REFRESH_MS  1000
#define MAX_ACTIVE_KEYS     64

typedef struct lua_State lua_State;
typedef int (*lua_CFunction)(lua_State*);

typedef void        (__attribute__((thiscall)) *fn_OnLayerUpdate)(void* self, float elapsedSec);
typedef const char* (__attribute__((thiscall)) *fn_GetCommandKey)(void* self, int mode, const char* command, int index);
typedef int32_t     (__attribute__((thiscall)) *fn_ExecKey)(void* self, uint32_t modifiers, const char* key, int32_t isDown, int32_t allowPressure, int mode);
typedef void        (__cdecl *fn_RegisterFunction)(const char* name, lua_CFunction fn);
typedef int         (__cdecl *fn_InitSignature)(int32_t profile);
typedef void        (__cdecl *fn_lua_pushnumber)(lua_State* L, double n);

static fn_GetCommandKey     g_GetCommandKey;
static fn_RegisterFunction  g_RegisterFunction;
static fn_lua_pushnumber    g_lua_pushnumber;

static fn_OnLayerUpdate     g_orig_update = NULL;
static fn_ExecKey           g_orig_execkey = NULL;
static fn_InitSignature     g_orig_init = NULL;

/* ---------------- logging ---------------- */
static char g_logpath[MAX_PATH];
static CRITICAL_SECTION g_loglock;
static int g_loginit = 0;

static void htc_log(const char* fmt, ...) {
    if (!g_loginit || !g_logpath[0]) return;
    EnterCriticalSection(&g_loglock);
    FILE* f = fopen(g_logpath, "a");
    if (f) {
        va_list ap; va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_loglock);
}

void HoldToCast_SetLogPath(const char* path) {
    if (!g_loginit) { InitializeCriticalSection(&g_loglock); g_loginit = 1; }
    lstrcpynA(g_logpath, path, MAX_PATH);
}

/* ---------------- key name -> virtual key ---------------- */
static int KeyToVK(const char* key) {
    if (!key || !key[0]) return 0;
    const char* p = key;
    const char* dash = NULL;
    for (const char* q = key; *q; q++) if (*q == '-') dash = q;
    if (dash) p = dash + 1;
    if (p[0] >= '0' && p[0] <= '9' && p[1] == 0) return p[0];
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == 0)
        return (p[0] >= 'a') ? (p[0] - 32) : p[0];
    if (p[0] == 'F') {
        int n = 0, i = 1;
        while (p[i] >= '0' && p[i] <= '9') { n = n * 10 + (p[i] - '0'); i++; }
        if (i > 1 && p[i] == 0 && n >= 1 && n <= 24) return 0x70 + (n - 1);
    }
    if (!lstrcmpiA(p, "NUMPADDECIMAL")) return 0x6E;
    if (!_strnicmp(p, "NUMPAD", 6) && p[6] >= '0' && p[6] <= '9' && p[7] == 0) return 0x60 + (p[6] - '0');
    return 0;
}

/* ---------------- held-key set (driven by ExecKey) ---------------- */
typedef struct { char key[24]; uint32_t mods; int mode; } ActiveEntry;
static ActiveEntry g_active[MAX_ACTIVE_KEYS];
static int g_activeN = 0;

static int ActiveFind(const char* k) {
    if (!k || !k[0]) return -1;
    for (int i = 0; i < g_activeN; i++)
        if (!lstrcmpiA(g_active[i].key, k)) return i;
    return -1;
}
static void ActiveAdd(const char* k, uint32_t mods, int mode) {
    if (!k || !k[0] || ActiveFind(k) >= 0) return;
    if (g_activeN >= MAX_ACTIVE_KEYS) g_activeN = 0;
    lstrcpynA(g_active[g_activeN].key, k, 24);
    g_active[g_activeN].mods = mods;
    g_active[g_activeN].mode = mode;
    g_activeN++;
}
static void ActiveDel(const char* k) {
    int i = ActiveFind(k);
    if (i < 0) return;
    g_active[i] = g_active[g_activeN - 1];
    g_activeN--;
}

/* ---------------- slot -> bound key ---------------- */
static char g_slotKey[NUM_ACTION_BUTTONS][24];
static int  g_slotVk[NUM_ACTION_BUTTONS];
static char g_cmdBuf[NUM_ACTION_BUTTONS][24];
static HWND g_gameWnd = NULL;

static void ResolveBindings(void) {
    void* b = *(void**)va(VA_s_bindings);
    if (!b) return;
    for (int i = 0; i < NUM_ACTION_BUTTONS; i++) {
        const char* k = g_GetCommandKey(b, BINDING_MODE_ACTIVE, g_cmdBuf[i], 0);
        if (k) { lstrcpynA(g_slotKey[i], k, 24); g_slotVk[i] = KeyToVK(k); }
        else   { g_slotKey[i][0] = 0; g_slotVk[i] = 0; }
    }
}

static void Replay(const ActiveEntry* e) {
    void* b = *(void**)va(VA_s_bindings);
    if (b) g_orig_execkey(b, e->mods, e->key, 1, 0, e->mode);
}

static void Tick(void) {
    static DWORD lastResolve = 0, nextCast = 0;
    DWORD now = GetTickCount();

    if (now - lastResolve >= BINDING_REFRESH_MS) { ResolveBindings(); lastResolve = now; }
    if (now < nextCast) return;

    int busy = 0;
    for (int s = 0; s < NUM_ACTION_BUTTONS; s++) {
        if (!g_slotKey[s][0]) continue;
        int idx = ActiveFind(g_slotKey[s]);
        int down = g_slotVk[s] && (GetAsyncKeyState(g_slotVk[s]) & 0x8000);
        if (idx >= 0 && !down) { ActiveDel(g_slotKey[s]); idx = -1; }   /* catch missed key-up */
        if (idx >= 0) busy = 1;
    }
    if (!busy) return;
    if (!g_gameWnd) g_gameWnd = GetForegroundWindow();
    if (g_gameWnd && GetForegroundWindow() != g_gameWnd) return;

    for (int s = 0; s < NUM_ACTION_BUTTONS; s++) {
        if (!g_slotKey[s][0]) continue;
        int idx = ActiveFind(g_slotKey[s]);
        if (idx >= 0 && (GetAsyncKeyState(g_slotVk[s]) & 0x8000))
            Replay(&g_active[idx]);
    }
    nextCast = now + CAST_THROTTLE_MS;
}

static void __attribute__((thiscall)) hook_OnLayerUpdate(void* self, float dt) {
    g_orig_update(self, dt);
    Tick();
}

static int32_t __attribute__((thiscall)) hook_ExecKey(void* self, uint32_t modifiers, const char* key, int32_t isDown, int32_t allowPressure, int mode) {
    if (isDown) ActiveAdd(key, modifiers, mode); else ActiveDel(key);
    return g_orig_execkey(self, modifiers, key, isDown, allowPressure, mode);
}

/* ---------------- Lua notifier ---------------- */
static int l_loaded(lua_State* L) {
    g_lua_pushnumber(L, 1);
    return 1;
}

static int32_t __cdecl hook_Init(int32_t profile) {
    int32_t r = g_orig_init(profile);
    g_RegisterFunction("HOLDTOCAST_LOADED", l_loaded);  /* republish: state rebuilt each call */
    return r;
}

/* ---------------- init ---------------- */
void HoldToCast_Init(void) {
    htc_log("HoldToCast_Init wowbase=%p", (void*)GetModuleHandleA(NULL));

    g_GetCommandKey    = (fn_GetCommandKey)va(VA_CGUIBindings_GetCommandKey);
    g_RegisterFunction = (fn_RegisterFunction)va(VA_FrameScript_RegisterFunction);
    g_lua_pushnumber   = (fn_lua_pushnumber)va(VA_lua_pushnumber);

    for (int i = 0; i < NUM_ACTION_BUTTONS; i++)
        wsprintfA(g_cmdBuf[i], "ACTIONBUTTON%d", i + 1);

    if (MH_Initialize() != MH_OK) { htc_log("MH_Initialize FAILED"); return; }

    if (MH_CreateHook(va(VA_FrameScript_Initialize), (LPVOID)&hook_Init, (LPVOID*)&g_orig_init) == MH_OK)
        MH_EnableHook(va(VA_FrameScript_Initialize));
    if (MH_CreateHook(va(VA_CGWorldFrame_OnLayerUpdate), (LPVOID)&hook_OnLayerUpdate, (LPVOID*)&g_orig_update) == MH_OK)
        MH_EnableHook(va(VA_CGWorldFrame_OnLayerUpdate));
    if (MH_CreateHook(va(VA_CGUIBindings_ExecKey), (LPVOID)&hook_ExecKey, (LPVOID*)&g_orig_execkey) == MH_OK)
        MH_EnableHook(va(VA_CGUIBindings_ExecKey));

    htc_log("hooks: init=%p update=%p execkey=%p", (void*)g_orig_init, (void*)g_orig_update, (void*)g_orig_execkey);
}
