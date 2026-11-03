#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

#define MAX_DEV   16
#define MAX_APP   64
#define MAX_LIST  32
#define MAX_PIDS  256

// GUIDs and raw COM layouts for the undocumented interfaces           */

static const GUID MY_CLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID MY_IID_IMMDeviceEnumerator  = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID MY_CLSID_PolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
static const GUID MY_IID_IPolicyConfig        = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
// Windows 11 / 10 build 21390+
static const GUID MY_IID_APC_21H2     = {0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}};
// Windows 10 1803 - 21H1
static const GUID MY_IID_APC_DOWNLEVEL = {0x2a59116d, 0x6c4f, 0x45e0, {0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58}};

static const PROPERTYKEY MY_PKEY_FriendlyName =
    {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// IPolicyConfig: SetDefaultEndpoint is vtable slot 13
typedef struct PolicyConfig PolicyConfig;
typedef struct PolicyConfigVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(PolicyConfig *, const GUID *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(PolicyConfig *);
    ULONG   (STDMETHODCALLTYPE *Release)(PolicyConfig *);
    void *slots[10];                       // 3..12 (format/period/share/property methods)
    HRESULT (STDMETHODCALLTYPE *SetDefaultEndpoint)(PolicyConfig *, const wchar_t *, int role);
    void *SetEndpointVisibility;
} PolicyConfigVtbl;
struct PolicyConfig { const PolicyConfigVtbl *lpVtbl; };

// AudioPolicyConfig factory: Set/GetPersisted... are vtable slots 25/26.
typedef struct HSTR__ *MY_HSTRING;
typedef struct APCFactory APCFactory;
typedef struct APCFactoryVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(APCFactory *, const GUID *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(APCFactory *);
    ULONG   (STDMETHODCALLTYPE *Release)(APCFactory *);
    void *GetIids, *GetRuntimeClassName, *GetTrustLevel;       // IInspectable: 3..5
    void *unused[19];                                          // 6..24
    HRESULT (STDMETHODCALLTYPE *SetPersistedDefaultAudioEndpoint)(APCFactory *, UINT32 pid, int flow, int role, MY_HSTRING dev);
    HRESULT (STDMETHODCALLTYPE *GetPersistedDefaultAudioEndpoint)(APCFactory *, UINT32 pid, int flow, int role, MY_HSTRING *dev);
    HRESULT (STDMETHODCALLTYPE *ClearAllPersistedApplicationDefaultEndpoints)(APCFactory *);
} APCFactoryVtbl;
struct APCFactory { const APCFactoryVtbl *lpVtbl; };

typedef char assert_policyconfig_slot[(offsetof(PolicyConfigVtbl, SetDefaultEndpoint) == 13 * sizeof(void *)) ? 1 : -1];
typedef char assert_apc_slot[(offsetof(APCFactoryVtbl, SetPersistedDefaultAudioEndpoint) == 25 * sizeof(void *)) ? 1 : -1];

typedef HRESULT (WINAPI *PFN_RoGetActivationFactory)(MY_HSTRING, const GUID *, void **);
typedef HRESULT (WINAPI *PFN_WindowsCreateString)(const wchar_t *, UINT32, MY_HSTRING *);
typedef HRESULT (WINAPI *PFN_WindowsDeleteString)(MY_HSTRING);

static PFN_RoGetActivationFactory pRoGetActivationFactory;
static PFN_WindowsCreateString    pWindowsCreateString;
static PFN_WindowsDeleteString    pWindowsDeleteString;

// Config

typedef struct { wchar_t id[128]; wchar_t name[256]; } DevInfo;
typedef struct { wchar_t slot[32]; wchar_t needle[256]; } DevSpec;
typedef struct { wchar_t exe[96]; wchar_t slot[32]; } AppSpec;

typedef struct {
    DevSpec dev[MAX_DEV]; int ndev;
    AppSpec app[MAX_APP]; int napp;
    wchar_t deskDefault[32];
    UINT mods, vkDesk, vkTv;
    wchar_t keyDesk[16], keyTv[16];
    wchar_t cmdDesk[512], cmdTv[512];
    DWORD settleMs, waitMs, autoDelayMs;
    int beep, autoSwitch;
} Config;

static Config  g_cfg;
static wchar_t g_ini[MAX_PATH];
static wchar_t g_exe[MAX_PATH];

static const wchar_t DEFAULT_INI[] =
    L"; audio_switch.ini - edit, save, then use the tray menu > Reload config.\r\n"
    L"; Put comments on their own line, starting with ';'.\r\n"
    L"\r\n"
    L"[hotkeys]\r\n"
    L"; modifiers: any of ctrl, alt, shift, win joined with + (e.g. ctrl+alt+shift), or none.\r\n"
    L"; Key: 0-9, A-Z or F1-F24. With modifiers=none only F1-F24 are allowed.\r\n"
    L"; F13/F14 are handy for a Stream Deck \"Hotkey\" action and never clash with other apps.\r\n"
    L"modifiers=none\r\n"
    L"desk=F13\r\n"
    L"tv=F14\r\n"
    L"\r\n"
    L"[devices]\r\n"
    L"; slot = part of the playback device name (tray menu > Show audio devices).\r\n"
    L"; Not case sensitive. Use enough text to be unique. The names below are examples.\r\n"
    L"; You can add more slots (e.g. game=Game (...)) and use them in [apps].\r\n"
    L"system=\r\n"
    L"music=\r\n"
    L"chat=\r\n"
    L"tv=\r\n"
    L"\r\n"
    L"[desk]\r\n"
    L"; Where Windows system sounds and unlisted apps go in desk mode (a slot above).\r\n"
    L"default=system\r\n"
    L"\r\n"
    L"[apps]\r\n"
    L"; process name = slot used in desk mode. Use \"default\" to let an app follow the\r\n"
    L"; Windows default device. In TV mode every app listed here goes to the tv slot.\r\n"
    L"; Apps not listed here just follow the Windows default device, so you only need\r\n"
    L"; to list an app to pin it to a specific channel (or to force a running app to move).\r\n"
    L"chrome.exe=music\r\n"
    L"spotify.exe=music\r\n"
    L"discord.exe=chat\r\n"
    L"; steam.exe=system\r\n"
    L"\r\n"
    L"[commands]\r\n"
    L"; Optional: a program to run first (for example a monitor-switching tool).\r\n"
    L"; Then audio switching waits settle_ms. Leave blank to skip.\r\n"
    L"; desk=\r\n"
    L"; tv=\r\n"
    L"settle_ms=3000\r\n"
    L"\r\n"
    L"[options]\r\n"
    L"; How long to wait for a device (like the TV) to appear after switching.\r\n"
    L"wait_for_device_ms=8000\r\n"
    L"; 1 = switch to TV mode when the tv device appears, and back to desk mode when it disappears.\r\n"
    L"auto_switch=1\r\n"
    L"; After a device change, wait this long before acting so brief flickers are ignored.\r\n"
    L"auto_delay_ms=2500\r\n"
    L"; 1 = play a short beep when a switch finishes, 0 = silent.\r\n"
    L"beep=1\r\n";

static void copyw(wchar_t *dst, size_t cap, const wchar_t *src)
{
    wcsncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static void wappend(wchar_t *dst, size_t cap, const wchar_t *fmt, ...)
{
    size_t len = wcslen(dst);
    va_list ap;
    va_start(ap, fmt);
    if (len + 1 < cap)
        _vsnwprintf(dst + len, cap - len - 1, fmt, ap);
    va_end(ap);
}

static wchar_t *trim(wchar_t *s)
{
    wchar_t *e;
    while (*s == L' ' || *s == L'\t') s++;
    e = s + wcslen(s);
    while (e > s && (e[-1] == L' ' || e[-1] == L'\t' || e[-1] == L'\r' || e[-1] == L'\n')) *--e = 0;
    return s;
}

static UINT parse_key(const wchar_t *s)
{
    while (*s == L' ') s++;
    if (!*s) return 0;
    if (!s[1]) {
        wchar_t ch = (wchar_t)towupper(s[0]);
        if ((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z')) return (UINT)ch;
        return 0;
    }
    if (s[0] == L'F' || s[0] == L'f') {
        int n = _wtoi(s + 1);
        if (n >= 1 && n <= 24) return (UINT)(VK_F1 + n - 1);
    }
    return 0;
}

static int is_fkey(UINT vk)
{
    return vk >= VK_F1 && vk <= VK_F24;
}

static UINT parse_mods(const wchar_t *s)
{
    UINT m = 0;
    wchar_t word[16];
    while (*s) {
        size_t n = 0;
        while (*s && !iswalpha(*s)) s++;
        while (*s && iswalpha(*s)) {
            if (n < 15) word[n++] = (wchar_t)towlower(*s);
            s++;
        }
        word[n] = 0;
        if (!n) break;
        if (!wcscmp(word, L"ctrl") || !wcscmp(word, L"control")) m |= MOD_CONTROL;
        else if (!wcscmp(word, L"alt"))   m |= MOD_ALT;
        else if (!wcscmp(word, L"shift")) m |= MOD_SHIFT;
        else if (!wcscmp(word, L"win") || !wcscmp(word, L"windows")) m |= MOD_WIN;
    }
    return m;
}

// Iterate "key=value" entries of a GetPrivateProfileSection buffer.
static int next_pair(wchar_t **cur, wchar_t **key, wchar_t **val)
{
    while (**cur) {
        wchar_t *line = *cur, *eq;
        *cur += wcslen(line) + 1;
        line = trim(line);
        if (!*line || *line == L';' || *line == L'#') continue;
        eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = 0;
        *key = trim(line);
        *val = trim(eq + 1);
        if (!**key) continue;
        return 1;
    }
    return 0;
}

static void load_config(Config *c)
{
    static wchar_t buf[8192];
    wchar_t *cur, *k, *v;
    wchar_t modtxt[64];

    memset(c, 0, sizeof *c);

    GetPrivateProfileStringW(L"hotkeys", L"modifiers", L"ctrl+alt+shift", modtxt, ARRAYSIZE(modtxt), g_ini);
    GetPrivateProfileStringW(L"hotkeys", L"desk", L"F13", c->keyDesk, ARRAYSIZE(c->keyDesk), g_ini);
    GetPrivateProfileStringW(L"hotkeys", L"tv",   L"F14", c->keyTv,   ARRAYSIZE(c->keyTv),   g_ini);
    c->mods   = parse_mods(modtxt);
    c->vkDesk = parse_key(c->keyDesk);
    c->vkTv   = parse_key(c->keyTv);
    if (!c->mods) {
        if (!is_fkey(c->vkDesk)) c->vkDesk = 0;
        if (!is_fkey(c->vkTv))   c->vkTv = 0;
    }
    _wcsupr(c->keyDesk);
    _wcsupr(c->keyTv);

    GetPrivateProfileStringW(L"desk", L"default", L"system", c->deskDefault, ARRAYSIZE(c->deskDefault), g_ini);
    GetPrivateProfileStringW(L"commands", L"desk", L"", c->cmdDesk, ARRAYSIZE(c->cmdDesk), g_ini);
    GetPrivateProfileStringW(L"commands", L"tv",   L"", c->cmdTv,   ARRAYSIZE(c->cmdTv),   g_ini);
    c->settleMs = GetPrivateProfileIntW(L"commands", L"settle_ms", 3000, g_ini);
    c->waitMs   = GetPrivateProfileIntW(L"options", L"wait_for_device_ms", 8000, g_ini);
    c->beep     = GetPrivateProfileIntW(L"options", L"beep", 1, g_ini);
    c->autoSwitch  = GetPrivateProfileIntW(L"options", L"auto_switch", 1, g_ini);
    c->autoDelayMs = GetPrivateProfileIntW(L"options", L"auto_delay_ms", 2500, g_ini);

    memset(buf, 0, sizeof buf);
    GetPrivateProfileSectionW(L"devices", buf, ARRAYSIZE(buf), g_ini);
    cur = buf;
    while (c->ndev < MAX_DEV && next_pair(&cur, &k, &v)) {
        copyw(c->dev[c->ndev].slot, ARRAYSIZE(c->dev[0].slot), k);
        copyw(c->dev[c->ndev].needle, ARRAYSIZE(c->dev[0].needle), v);
        c->ndev++;
    }

    memset(buf, 0, sizeof buf);
    GetPrivateProfileSectionW(L"apps", buf, ARRAYSIZE(buf), g_ini);
    cur = buf;
    while (c->napp < MAX_APP && next_pair(&cur, &k, &v)) {
        copyw(c->app[c->napp].exe, ARRAYSIZE(c->app[0].exe), k);
        copyw(c->app[c->napp].slot, ARRAYSIZE(c->app[0].slot), v);
        c->napp++;
    }
}

// Audio devices

static int list_devices(DevInfo *out, int max)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDeviceCollection *col = NULL;
    UINT count = 0, i;
    int n = 0;

    if (FAILED(CoCreateInstance(&MY_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &MY_IID_IMMDeviceEnumerator, (void **)&en)))
        return 0;

    if (SUCCEEDED(en->lpVtbl->EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col))) {
        col->lpVtbl->GetCount(col, &count);
        for (i = 0; i < count && n < max; i++) {
            IMMDevice *dev = NULL;
            LPWSTR id = NULL;
            if (FAILED(col->lpVtbl->Item(col, i, &dev))) continue;
            if (SUCCEEDED(dev->lpVtbl->GetId(dev, &id)) && id) {
                IPropertyStore *ps = NULL;
                memset(&out[n], 0, sizeof out[n]);
                copyw(out[n].id, ARRAYSIZE(out[n].id), id);
                if (SUCCEEDED(dev->lpVtbl->OpenPropertyStore(dev, STGM_READ, &ps))) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(ps->lpVtbl->GetValue(ps, &MY_PKEY_FriendlyName, &pv)) &&
                        pv.vt == VT_LPWSTR && pv.pwszVal)
                        copyw(out[n].name, ARRAYSIZE(out[n].name), pv.pwszVal);
                    PropVariantClear(&pv);
                    ps->lpVtbl->Release(ps);
                }
                CoTaskMemFree(id);
                n++;
            }
            dev->lpVtbl->Release(dev);
        }
        col->lpVtbl->Release(col);
    }
    en->lpVtbl->Release(en);
    return n;
}

static int ci_contains(const wchar_t *hay, const wchar_t *needle)
{
    wchar_t h[256], n[256];
    copyw(h, ARRAYSIZE(h), hay);
    copyw(n, ARRAYSIZE(n), needle);
    _wcslwr(h);
    _wcslwr(n);
    return wcsstr(h, n) != NULL;
}

static int find_dev(const DevInfo *list, int n, const wchar_t *needle)
{
    int i;
    if (!*needle) return -1;
    for (i = 0; i < n; i++)
        if (!_wcsicmp(list[i].name, needle) || !_wcsicmp(list[i].id, needle)) return i;
    for (i = 0; i < n; i++)
        if (ci_contains(list[i].name, needle)) return i;
    return -1;
}

typedef struct {
    const Config *cfg;
    DevInfo dev[MAX_DEV];
    int state[MAX_DEV];            // 0 untried, 1 found, -1 not found
    wchar_t err[256];
} Resolver;

// Look up a [devices] slot, waiting (for TV etc.) if it is not present yet
static const DevInfo *resolve(Resolver *rs, const wchar_t *slot)
{
    int i;
    DWORD t0;
    DevInfo list[MAX_LIST];

    for (i = 0; i < rs->cfg->ndev; i++)
        if (!_wcsicmp(rs->cfg->dev[i].slot, slot)) break;
    if (i == rs->cfg->ndev) {
        if (!rs->err[0]) _snwprintf(rs->err, 255, L"No [devices] entry named '%s'", slot);
        return NULL;
    }
    if (rs->state[i] == 1)  return &rs->dev[i];
    if (rs->state[i] == -1) return NULL;

    t0 = GetTickCount();
    for (;;) {
        int n = list_devices(list, MAX_LIST);
        int j = find_dev(list, n, rs->cfg->dev[i].needle);
        if (j >= 0) {
            rs->dev[i] = list[j];
            rs->state[i] = 1;
            return &rs->dev[i];
        }
        if (GetTickCount() - t0 >= rs->cfg->waitMs) break;
        Sleep(500);
    }
    rs->state[i] = -1;
    if (!rs->err[0]) _snwprintf(rs->err, 255, L"Device not found: %s", rs->cfg->dev[i].needle);
    return NULL;
}

static int set_default_endpoint(const wchar_t *id)
{
    PolicyConfig *pc = NULL;
    int role, ok = 1;
    if (FAILED(CoCreateInstance(&MY_CLSID_PolicyConfigClient, NULL, CLSCTX_ALL,
                                &MY_IID_IPolicyConfig, (void **)&pc)) || !pc)
        return 0;
    for (role = 0; role < 3; role++)          /* console, multimedia, communications */
        if (FAILED(pc->lpVtbl->SetDefaultEndpoint(pc, id, role))) ok = 0;
    pc->lpVtbl->Release(pc);
    return ok;
}

// Per app routing
static int winrt_init(void)
{
    HMODULE h;
    if (pRoGetActivationFactory) return 1;
    h = LoadLibraryW(L"combase.dll");
    if (!h) return 0;
    pRoGetActivationFactory = (PFN_RoGetActivationFactory)(void *)GetProcAddress(h, "RoGetActivationFactory");
    pWindowsCreateString    = (PFN_WindowsCreateString)(void *)GetProcAddress(h, "WindowsCreateString");
    pWindowsDeleteString    = (PFN_WindowsDeleteString)(void *)GetProcAddress(h, "WindowsDeleteString");
    return pRoGetActivationFactory && pWindowsCreateString && pWindowsDeleteString;
}

static APCFactory *get_factory(void)
{
    static const wchar_t cls[] = L"Windows.Media.Internal.AudioPolicyConfig";
    MY_HSTRING hs = NULL;
    APCFactory *f = NULL;

    if (!winrt_init()) return NULL;
    if (FAILED(pWindowsCreateString(cls, (UINT32)wcslen(cls), &hs))) return NULL;
    if (FAILED(pRoGetActivationFactory(hs, &MY_IID_APC_21H2, (void **)&f)))
        if (FAILED(pRoGetActivationFactory(hs, &MY_IID_APC_DOWNLEVEL, (void **)&f)))
            f = NULL;
    pWindowsDeleteString(hs);
    return f;
}

// mmId == NULL clears the per-app setting (app follows the Windows default).
static int set_app_endpoint(APCFactory *f, DWORD pid, const wchar_t *mmId)
{
    MY_HSTRING hs = NULL;
    int role, ok = 0;

    if (mmId) {
        wchar_t full[300];
        full[0] = 0;
        wappend(full, ARRAYSIZE(full), L"\\\\?\\SWD#MMDEVAPI#%s#{e6327cad-dcec-4949-ae8a-991e976a79d2}", mmId);
        if (FAILED(pWindowsCreateString(full, (UINT32)wcslen(full), &hs))) return 0;
    }
    for (role = 0; role < 3; role++)
        if (SUCCEEDED(f->lpVtbl->SetPersistedDefaultAudioEndpoint(f, (UINT32)pid, 0 /* eRender */, role, hs)))
            ok = 1;
    if (hs) pWindowsDeleteString(hs);
    return ok;
}

static int find_pids(const wchar_t *exe, DWORD *pids, int max)
{
    PROCESSENTRY32W pe;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    int n = 0;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (!_wcsicmp(pe.szExeFile, exe) && n < max) pids[n++] = pe.th32ProcessID;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return n;
}

static void run_command(const wchar_t *cmdline)
{
    wchar_t buf[512];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    copyw(buf, ARRAYSIZE(buf), cmdline);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessW(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 20000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}


// Switching (runs on a worker thread)

#define WM_TRAY      (WM_APP + 1)
#define WM_APP_DONE      (WM_APP + 2)
#define WM_APP_DEVCHANGE (WM_APP + 3)
#define WM_APP_CMD       (WM_APP + 4)
#define TIMER_AUTO       1

typedef struct { int mode; int ok; wchar_t text[512]; } Result;

static HWND g_hwnd;
static volatile LONG g_busy;

static DWORD WINAPI apply_thread(LPVOID param)
{
    int tv = (int)((INT_PTR)param & 1);
    int runCmd = (int)(((INT_PTR)param >> 1) & 1);
    int i, routed = 0, skipped = 0, failed = 0, defOk = 0;
    Result *r = (Result *)calloc(1, sizeof *r);
    Resolver *rs = (Resolver *)calloc(1, sizeof *rs);
    APCFactory *fac = NULL;
    const DevInfo *d;
    const wchar_t *cmd = tv ? g_cfg.cmdTv : g_cfg.cmdDesk;
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    rs->cfg = &g_cfg;
    r->mode = tv;

    if (runCmd && cmd[0]) {
        run_command(cmd);
        Sleep(g_cfg.settleMs);
    }

    // System default
    d = resolve(rs, tv ? L"tv" : g_cfg.deskDefault);
    if (d) defOk = set_default_endpoint(d->id);

    // Per-app overrides
    fac = get_factory();
    if (fac) {
        for (i = 0; i < g_cfg.napp; i++) {
            const AppSpec *a = &g_cfg.app[i];
            const wchar_t *slot = tv ? L"tv" : a->slot;
            DWORD pids[MAX_PIDS];
            int np = find_pids(a->exe, pids, MAX_PIDS), j, anyOk = 0;
            const wchar_t *id = NULL;

            if (np == 0) { skipped++; continue; }
            if (_wcsicmp(slot, L"default") != 0) {
                d = resolve(rs, slot);
                if (!d) { failed++; continue; }
                id = d->id;
            }
            for (j = 0; j < np; j++)
                if (set_app_endpoint(fac, pids[j], id)) anyOk = 1;
            if (anyOk) routed++; else failed++;
        }
        fac->lpVtbl->Release(fac);
    }

    r->ok = defOk && fac && !failed;
    wappend(r->text, ARRAYSIZE(r->text), L"%s mode: %d app%s routed", tv ? L"TV" : L"Desk", routed, routed == 1 ? L"" : L"s");
    if (skipped) wappend(r->text, ARRAYSIZE(r->text), L", %d not running", skipped);
    wappend(r->text, ARRAYSIZE(r->text), L".");
    if (!defOk && !rs->err[0]) wappend(r->text, ARRAYSIZE(r->text), L" Could not set the default device.");
    if (!fac) wappend(r->text, ARRAYSIZE(r->text), L" Per-app routing is not available on this Windows build.");
    if (failed) wappend(r->text, ARRAYSIZE(r->text), L" %d app%s failed.", failed, failed == 1 ? L"" : L"s");
    if (rs->err[0]) wappend(r->text, ARRAYSIZE(r->text), L" %s.", rs->err);

    free(rs);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    PostMessageW(g_hwnd, WM_APP_DONE, 0, (LPARAM)r);
    return 0;
}

// runCmd: also run the optional commands program first. Returns 1 if started.
static int start_apply(int tv, int runCmd)
{
    HANDLE t;
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return 0;
    t = CreateThread(NULL, 0, apply_thread, (LPVOID)(INT_PTR)(tv | (runCmd << 1)), 0, NULL);
    if (t) {
        CloseHandle(t);
        return 1;
    }
    InterlockedExchange(&g_busy, 0);
    return 0;
}

// Tray icon and stuff

#define IDM_DESK     101
#define IDM_TV       102
#define IDM_DEVICES  103
#define IDM_EDIT     104
#define IDM_RELOAD   105
#define IDM_STARTUP  106
#define IDM_EXIT     107

#define HK_DESK 1
#define HK_TV   2

static const wchar_t RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t RUN_VAL[] = L"AudioSwitch";

static HICON g_icon[3];                 // 0 = desk, 1 = tv, 2 = unknown
static int g_mode = -1;                 // -1 unknown, 0 desk, 1 tv
static UINT g_taskbarCreated;

// Auto-switch: watch for audio devices appearing/disappearing

static const GUID MY_IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID MY_IID_IMMNotificationClient = {0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};

static HRESULT STDMETHODCALLTYPE nc_QueryInterface(IMMNotificationClient *This, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &MY_IID_IUnknown) || IsEqualGUID(riid, &MY_IID_IMMNotificationClient)) {
        *ppv = This;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE nc_AddRef(IMMNotificationClient *This)  { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE nc_Release(IMMNotificationClient *This) { (void)This; return 1; }

// These run on a Windows audio thread, so they only post a message.
static HRESULT STDMETHODCALLTYPE nc_OnDeviceStateChanged(IMMNotificationClient *This, LPCWSTR id, DWORD state)
{
    (void)This; (void)id; (void)state;
    PostMessageW(g_hwnd, WM_APP_DEVCHANGE, 0, 0);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE nc_OnDeviceAdded(IMMNotificationClient *This, LPCWSTR id)
{
    (void)This; (void)id;
    PostMessageW(g_hwnd, WM_APP_DEVCHANGE, 0, 0);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE nc_OnDeviceRemoved(IMMNotificationClient *This, LPCWSTR id)
{
    (void)This; (void)id;
    PostMessageW(g_hwnd, WM_APP_DEVCHANGE, 0, 0);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE nc_OnDefaultDeviceChanged(IMMNotificationClient *This, EDataFlow flow, ERole role, LPCWSTR id)
{
    (void)This; (void)flow; (void)role; (void)id;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE nc_OnPropertyValueChanged(IMMNotificationClient *This, LPCWSTR id, const PROPERTYKEY key)
{
    (void)This; (void)id; (void)key;
    return S_OK;
}

static IMMNotificationClientVtbl g_ncVtbl = {
    nc_QueryInterface, nc_AddRef, nc_Release,
    nc_OnDeviceStateChanged, nc_OnDeviceAdded, nc_OnDeviceRemoved,
    nc_OnDefaultDeviceChanged, nc_OnPropertyValueChanged
};
static IMMNotificationClient g_nc = { &g_ncVtbl };
static IMMDeviceEnumerator *g_notifyEnum;

static void notify_register(void)
{
    if (g_notifyEnum) return;
    if (SUCCEEDED(CoCreateInstance(&MY_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                   &MY_IID_IMMDeviceEnumerator, (void **)&g_notifyEnum)) && g_notifyEnum)
        g_notifyEnum->lpVtbl->RegisterEndpointNotificationCallback(g_notifyEnum, &g_nc);
}

static void notify_unregister(void)
{
    if (!g_notifyEnum) return;
    g_notifyEnum->lpVtbl->UnregisterEndpointNotificationCallback(g_notifyEnum, &g_nc);
    g_notifyEnum->lpVtbl->Release(g_notifyEnum);
    g_notifyEnum = NULL;
}

// TV device present -> TV mode, absent -> desk mode (only if that is a change).
static void auto_check(void)
{
    DevInfo list[MAX_LIST];
    int i, n, want;

    if (!g_cfg.autoSwitch) return;
    if (g_busy) {                                   // Try again shortly
        SetTimer(g_hwnd, TIMER_AUTO, 1000, NULL);
        return;
    }
    for (i = 0; i < g_cfg.ndev; i++)
        if (!_wcsicmp(g_cfg.dev[i].slot, L"tv")) break;
    if (i == g_cfg.ndev) return;

    n = list_devices(list, MAX_LIST);
    want = find_dev(list, n, g_cfg.dev[i].needle) >= 0;
    if (want != g_mode) start_apply(want, 0);       // no commands: the monitor change already happened
}

static int parse_cli(void)
{
    int argc = 0, mode = -1;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        const wchar_t *a = argv[1];
        while (*a == L'-' || *a == L'/') a++;
        if (!_wcsicmp(a, L"desk")) mode = 0;
        else if (!_wcsicmp(a, L"tv")) mode = 1;
        else if (!_wcsicmp(a, L"toggle")) mode = 2;
    }
    if (argv) LocalFree(argv);
    return mode;
}

static void hotkey_label(wchar_t *out, size_t cap, const wchar_t *key)
{
    if (g_cfg.mods & MOD_CONTROL) wappend(out, cap, L"Ctrl+");
    if (g_cfg.mods & MOD_ALT)     wappend(out, cap, L"Alt+");
    if (g_cfg.mods & MOD_SHIFT)   wappend(out, cap, L"Shift+");
    if (g_cfg.mods & MOD_WIN)     wappend(out, cap, L"Win+");
    wappend(out, cap, L"%s", key);
}

static HICON make_icon(COLORREF fill)
{
    int s = GetSystemMetrics(SM_CXSMICON);
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, s, s);
    HBITMAP mask = CreateBitmap(s, s, 1, 1, NULL);
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HGDIOBJ oldBmp, oldBr, oldPen;
    ICONINFO ii;
    HICON icon;
    RECT rc = {0, 0, s, s};

    oldBmp = SelectObject(mem, color);
    FillRect(mem, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    oldBr = SelectObject(mem, br);
    oldPen = SelectObject(mem, pen);
    Ellipse(mem, 0, 0, s, s);

    SelectObject(mem, mask);
    PatBlt(mem, 0, 0, s, s, WHITENESS);          // white = transparent
    SelectObject(mem, GetStockObject(BLACK_BRUSH));
    SelectObject(mem, GetStockObject(NULL_PEN));
    Ellipse(mem, 0, 0, s + 1, s + 1);            // black = opaque

    SelectObject(mem, oldBr);
    SelectObject(mem, oldPen);
    SelectObject(mem, oldBmp);

    ii.fIcon = TRUE;
    ii.xHotspot = ii.yHotspot = 0;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    icon = CreateIconIndirect(&ii);

    DeleteObject(br);
    DeleteObject(pen);
    DeleteObject(color);
    DeleteObject(mask);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return icon;
}

static void tray_notify(DWORD msg, const wchar_t *tip, const wchar_t *title, const wchar_t *info, int isError)
{
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon[g_mode < 0 ? 2 : g_mode];
    copyw(nid.szTip, ARRAYSIZE(nid.szTip), tip);
    if (info && *info) {
        nid.uFlags |= NIF_INFO;
        copyw(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);
        copyw(nid.szInfo, ARRAYSIZE(nid.szInfo), info);
        nid.dwInfoFlags = isError ? NIIF_ERROR : NIIF_INFO;
    }
    Shell_NotifyIconW(msg, &nid);
}

static const wchar_t *mode_tip(void)
{
    return g_mode == 1 ? L"Audio: TV" : g_mode == 0 ? L"Audio: Desk" : L"Audio switch (no mode set yet)";
}

static void register_hotkeys(void)
{
    const UINT mods = g_cfg.mods | MOD_NOREPEAT;
    int bad = 0;
    UnregisterHotKey(g_hwnd, HK_DESK);
    UnregisterHotKey(g_hwnd, HK_TV);
    if (!g_cfg.vkDesk || !RegisterHotKey(g_hwnd, HK_DESK, mods, g_cfg.vkDesk)) bad = 1;
    if (!g_cfg.vkTv   || !RegisterHotKey(g_hwnd, HK_TV,   mods, g_cfg.vkTv))   bad = 1;
    if (bad)
        tray_notify(NIM_MODIFY, mode_tip(), L"audio_switch",
                    L"A hotkey is invalid or already used by another program. Check [hotkeys] in the config.", 1);
}

static int startup_enabled(void)
{
    HKEY k;
    LONG r;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    r = RegQueryValueExW(k, RUN_VAL, NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static void startup_set(int on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t val[MAX_PATH + 4];
        _snwprintf(val, ARRAYSIZE(val) - 1, L"\"%s\"", g_exe);
        val[ARRAYSIZE(val) - 1] = 0;
        RegSetValueExW(k, RUN_VAL, 0, REG_SZ, (const BYTE *)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, RUN_VAL);
    }
    RegCloseKey(k);
}

static void open_config(void)
{
    wchar_t arg[MAX_PATH + 4];
    _snwprintf(arg, ARRAYSIZE(arg) - 1, L"\"%s\"", g_ini);
    arg[ARRAYSIZE(arg) - 1] = 0;
    ShellExecuteW(NULL, L"open", L"notepad.exe", arg, NULL, SW_SHOWNORMAL);
}

static void show_devices(void)
{
    DevInfo list[MAX_LIST];
    static wchar_t names[4096], text[6000];
    int n = list_devices(list, MAX_LIST), i;

    names[0] = text[0] = 0;
    for (i = 0; i < n; i++) wappend(names, ARRAYSIZE(names), L"%s\r\n", list[i].name);

    wappend(text, ARRAYSIZE(text), L"Active playback devices:\r\n\r\n%s\r\n", n ? names : L"(none found)\r\n");
    wappend(text, ARRAYSIZE(text),
            L"Put part of a name after a slot in the [devices] section of audio_switch.ini.\r\n"
            L"The list was copied to the clipboard.");

    if (n && OpenClipboard(g_hwnd)) {
        SIZE_T bytes = (wcslen(names) + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        EmptyClipboard();
        if (h) {
            void *p = GlobalLock(h);
            if (p) {
                memcpy(p, names, bytes);
                GlobalUnlock(h);
                SetClipboardData(CF_UNICODETEXT, h);
            } else {
                GlobalFree(h);
            }
        }
        CloseClipboard();
    }
    MessageBoxW(g_hwnd, text, L"audio_switch", MB_OK | MB_ICONINFORMATION);
}

static void show_menu(HWND hwnd)
{
    HMENU m = CreatePopupMenu();
    wchar_t t1[96], t2[96];
    POINT pt;

    t1[0] = t2[0] = 0;
    wappend(t1, ARRAYSIZE(t1), L"Desk mode\t");
    hotkey_label(t1, ARRAYSIZE(t1), g_cfg.keyDesk);
    wappend(t2, ARRAYSIZE(t2), L"TV mode\t");
    hotkey_label(t2, ARRAYSIZE(t2), g_cfg.keyTv);

    AppendMenuW(m, MF_STRING | (g_mode == 0 ? MF_CHECKED : 0), IDM_DESK, t1);
    AppendMenuW(m, MF_STRING | (g_mode == 1 ? MF_CHECKED : 0), IDM_TV, t2);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_DEVICES, L"Show audio devices...");
    AppendMenuW(m, MF_STRING, IDM_EDIT, L"Edit config...");
    AppendMenuW(m, MF_STRING, IDM_RELOAD, L"Reload config");
    AppendMenuW(m, MF_STRING | (startup_enabled() ? MF_CHECKED : 0), IDM_STARTUP, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");

    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_taskbarCreated && msg == g_taskbarCreated) {
        tray_notify(NIM_ADD, mode_tip(), NULL, NULL, 0);
        return 0;
    }
    switch (msg) {
    case WM_HOTKEY:
        if (wp == HK_DESK) start_apply(0, 1);
        else if (wp == HK_TV) start_apply(1, 1);
        return 0;

    case WM_APP_DEVCHANGE:
        if (g_cfg.autoSwitch) SetTimer(hwnd, TIMER_AUTO, g_cfg.autoDelayMs, NULL);   // restarts the delay
        return 0;

    case WM_TIMER:
        if (wp == TIMER_AUTO) {
            KillTimer(hwnd, TIMER_AUTO);
            auto_check();
        }
        return 0;

    case WM_APP_CMD:                              // from the command line
        if (wp == 2) start_apply(g_mode == 1 ? 0 : 1, 1);
        else start_apply((int)wp, 1);
        return 0;

    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) show_menu(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_DESK:    start_apply(0, 1); break;
        case IDM_TV:      start_apply(1, 1); break;
        case IDM_DEVICES: show_devices(); break;
        case IDM_EDIT:    open_config(); break;
        case IDM_STARTUP: startup_set(!startup_enabled()); break;
        case IDM_RELOAD:
            if (g_busy) break;
            load_config(&g_cfg);
            register_hotkeys();
            if (g_cfg.autoSwitch) SetTimer(hwnd, TIMER_AUTO, 500, NULL);
            tray_notify(NIM_MODIFY, mode_tip(), L"audio_switch", L"Config reloaded.", 0);
            break;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_APP_DONE: {
        Result *r = (Result *)lp;
        InterlockedExchange(&g_busy, 0);
        if (r) {
            if (r->ok) g_mode = r->mode;
            tray_notify(NIM_MODIFY, mode_tip(), L"audio_switch", r->ok ? NULL : r->text, 1);
            if (g_cfg.beep) {
                if (!r->ok)       Beep(200, 300);
                else if (r->mode) Beep(500, 100);
                else              Beep(900, 100);
            }
            free(r);
        }
        return 0;
    }

    case WM_DESTROY:
        UnregisterHotKey(hwnd, HK_DESK);
        UnregisterHotKey(hwnd, HK_TV);
        tray_notify(NIM_DELETE, L"", NULL, NULL, 0);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void write_default_ini(void)
{
    HANDLE f = CreateFileW(g_ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wr;
    WORD bom = 0xFEFF;                           // UTF-16 so Windows reads non-ASCII names correctly
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, &bom, sizeof bom, &wr, NULL);
    WriteFile(f, DEFAULT_INI, (DWORD)(wcslen(DEFAULT_INI) * sizeof(wchar_t)), &wr, NULL);
    CloseHandle(f);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow)
{
    WNDCLASSW wc;
    MSG msg;
    HANDLE mutex;
    wchar_t *slash;
    int firstRun, cliMode;

    (void)hPrev; (void)cmdLine; (void)nShow;

    cliMode = parse_cli();                         // -1 none, 0 desk, 1 tv, 2 toggle
    mutex = CreateMutexW(NULL, TRUE, L"AudioSwitchTrayMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (cliMode >= 0) {                        // hand the command to the running copy
            HWND w = FindWindowW(L"AudioSwitchTrayWnd", NULL);
            if (w) PostMessageW(w, WM_APP_CMD, (WPARAM)cliMode, 0);
        }
        return 0;
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    GetModuleFileNameW(NULL, g_exe, ARRAYSIZE(g_exe));
    copyw(g_ini, ARRAYSIZE(g_ini), g_exe);
    slash = wcsrchr(g_ini, L'\\');
    if (slash) *(slash + 1) = 0;
    wcsncat(g_ini, L"audio_switch.ini", ARRAYSIZE(g_ini) - wcslen(g_ini) - 1);

    firstRun = GetFileAttributesW(g_ini) == INVALID_FILE_ATTRIBUTES;
    if (firstRun) write_default_ini();
    load_config(&g_cfg);

    g_icon[0] = make_icon(RGB(0x3B, 0x82, 0xF6));    // desk = blue
    g_icon[1] = make_icon(RGB(0xF5, 0x9E, 0x0B));    // tv = amber 
    g_icon[2] = make_icon(RGB(0x80, 0x80, 0x80));    // unknown = grey

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"AudioSwitchTrayWnd";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"audio_switch", WS_OVERLAPPED, 0, 0, 0, 0,
                             NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    tray_notify(NIM_ADD, mode_tip(), NULL, NULL, 0);
    register_hotkeys();
    notify_register();
    if (g_cfg.autoSwitch) SetTimer(g_hwnd, TIMER_AUTO, 1500, NULL);   // sync with the current display setup
    if (cliMode >= 0) PostMessageW(g_hwnd, WM_APP_CMD, (WPARAM)cliMode, 0);

    if (firstRun) {
        tray_notify(NIM_MODIFY, mode_tip(), L"audio_switch",
                    L"First run: set your device names in the config that just opened, then choose Reload config.", 0);
        open_config();
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    notify_unregister();
    CoUninitialize();
    CloseHandle(mutex);
    return 0;
}
