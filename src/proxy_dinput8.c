/* DINPUT8.dll proxy. Wow.exe statically imports DirectInput8Create, so this
 * DLL loads automatically at process start; we install the HoldToCast hook
 * (on a worker thread, to avoid doing work under the loader lock) and forward
 * every export to the real system dinput8.dll.
 *
 * The __asm__ labels on the definitions force undecorated export names:
 * zig/mingw would otherwise emit the stdcall-decorated _Name@N form, which
 * Wow.exe's import by name would not resolve. */
#include <windows.h>
#include <guiddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void HoldToCast_Init(void);
void HoldToCast_SetLogPath(const char* path);

static char g_logpath[MAX_PATH];
static void plog(const char* s) {
    if (!g_logpath[0]) return;
    FILE* f = fopen(g_logpath, "a");
    if (f) { fputs(s, f); fputc('\n', f); fclose(f); }
}

static HMODULE g_real = NULL;

static HMODULE load_real_dinput8(void) {
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH - 16);
    if (n == 0 || n >= MAX_PATH - 16) return NULL;
    lstrcatA(path, "\\dinput8.dll");
    return LoadLibraryA(path);
}

static FARPROC real_proc(const char* name) {
    if (!g_real) return NULL;
    return GetProcAddress(g_real, name);
}

static DWORD WINAPI init_thread(LPVOID unused) {
    (void)unused;
    HoldToCast_Init();
    return 0;
}

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter) __asm__("DirectInput8Create");
HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
    typedef HRESULT (WINAPI *fn_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    fn_t fn = (fn_t)real_proc("DirectInput8Create");
    if (!fn) return E_FAIL;
    return fn(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) __asm__("DllGetClassObject");
HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    typedef HRESULT (WINAPI *fn_t)(REFCLSID, REFIID, LPVOID*);
    fn_t fn = (fn_t)real_proc("DllGetClassObject");
    if (!fn) return E_FAIL;
    return fn(rclsid, riid, ppv);
}

HRESULT WINAPI DllCanUnloadNow(void) __asm__("DllCanUnloadNow");
HRESULT WINAPI DllCanUnloadNow(void) {
    typedef HRESULT (WINAPI *fn_t)(void);
    fn_t fn = (fn_t)real_proc("DllCanUnloadNow");
    if (!fn) return S_FALSE;
    return fn();
}

HRESULT WINAPI DllRegisterServer(void) __asm__("DllRegisterServer");
HRESULT WINAPI DllRegisterServer(void) {
    typedef HRESULT (WINAPI *fn_t)(void);
    fn_t fn = (fn_t)real_proc("DllRegisterServer");
    if (!fn) return E_FAIL;
    return fn();
}

HRESULT WINAPI DllUnregisterServer(void) __asm__("DllUnregisterServer");
HRESULT WINAPI DllUnregisterServer(void) {
    typedef HRESULT (WINAPI *fn_t)(void);
    fn_t fn = (fn_t)real_proc("DllUnregisterServer");
    if (!fn) return E_FAIL;
    return fn();
}

void* WINAPI GetdfDIJoystick(void) __asm__("GetdfDIJoystick");
void* WINAPI GetdfDIJoystick(void) {
    typedef void* (WINAPI *fn_t)(void);
    fn_t fn = (fn_t)real_proc("GetdfDIJoystick");
    if (!fn) return NULL;
    return fn();
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        GetModuleFileNameA(hinstDLL, g_logpath, MAX_PATH);
        { char* p = strrchr(g_logpath, '\\'); if (p) lstrcpynA(p + 1, "holdtocast.log", 32); }
        HoldToCast_SetLogPath(g_logpath);
        plog("=== dinput8 proxy DllMain ATTACH ===");
        g_real = load_real_dinput8();
        plog(g_real ? "real dinput8 loaded" : "real dinput8 LOAD FAILED");
        HANDLE h = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
