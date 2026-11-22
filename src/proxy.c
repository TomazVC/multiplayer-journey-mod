// Journey Coop - proxy steam_api64.dll
//
// Journey (Steam) finds companions through public Steam lobbies ("meeting rooms")
// and then talks to them over SteamNetworking P2P. This proxy sits between the
// game and the real steam_api64.dll (renamed to steam_api64_o.dll) and:
//
//   1. Renames every lobby data key the game reads/writes/filters on to
//      "jc<ROOM>_<key>". Normal players never see our lobbies and we never see
//      theirs, so the only possible companion is someone using the same room code.
//   2. Tags our lobbies with jcoop_room=<ROOM> and filters searches on it
//      (worldwide distance), as a second fence.
//   3. Optionally (partner=<SteamID64> in the ini) drops P2P traffic from anyone
//      that is not the partner.
//
// Everything else is forwarded untouched through the .def file.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// config + log

static char     g_dir[MAX_PATH];
static char     g_room[40];
static uint64_t g_partner;
static int      g_log_on = 1;
static FILE    *g_log;
static CRITICAL_SECTION g_log_cs;

static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_log_cs);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_cs);
}

static void load_config(HMODULE self)
{
    GetModuleFileNameA(self, g_dir, MAX_PATH);
    char *slash = strrchr(g_dir, '\\');
    if (slash) slash[1] = 0;

    char ini[MAX_PATH + 32], raw[128], logpath[MAX_PATH + 32];
    snprintf(ini, sizeof ini, "%sjourney-coop.ini", g_dir);

    GetPrivateProfileStringA("coop", "room", "", raw, sizeof raw, ini);
    // keep only [A-Za-z0-9], uppercase, max 32 chars
    int n = 0;
    for (char *p = raw; *p && n < 32; ++p) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) g_room[n++] = c;
    }
    g_room[n] = 0;

    GetPrivateProfileStringA("coop", "partner", "0", raw, sizeof raw, ini);
    g_partner = _strtoui64(raw, NULL, 10);

    g_log_on = GetPrivateProfileIntA("coop", "log", 1, ini);
    if (g_log_on) {
        InitializeCriticalSection(&g_log_cs);
        snprintf(logpath, sizeof logpath, "%sjourney-coop.log", g_dir);
        g_log = fopen(logpath, "w");
    }
    logf_("Journey Coop proxy loaded. dir=%s", g_dir);
    if (g_room[0]) logf_("room code: %s", g_room);
    else           logf_("room code EMPTY -> mod disabled, pure pass-through");
    if (g_partner) logf_("partner SteamID64: %llu (P2P from others is dropped)", (unsigned long long)g_partner);
    else           logf_("partner not set (room code alone isolates the pair)");
}

// ---------------------------------------------------------------------------
// vtable patching

static void patch(void **vtbl, int idx, void *hook, void **orig)
{
    if (vtbl[idx] == hook) return;
    DWORD old;
    if (!VirtualProtect(&vtbl[idx], sizeof(void *), PAGE_READWRITE, &old)) {
        logf_("VirtualProtect failed (idx %d, err %lu)", idx, GetLastError());
        return;
    }
    *orig = vtbl[idx];
    vtbl[idx] = hook;
    VirtualProtect(&vtbl[idx], sizeof(void *), old, &old);
}

// key mapping: "level" -> "jcABC123_level". One buffer per thread per slot.
static const char *map_key(const char *key, int slot)
{
    static __thread char buf[4][320];
    if (!key) return key;
    snprintf(buf[slot & 3], sizeof buf[0], "jc%s_%s", g_room, key);
    return buf[slot & 3];
}

// ---------------------------------------------------------------------------
// ISteamMatchmaking (SteamMatchMaking009) hooks
// On x64 Windows member functions use the normal convention with `this` in RCX.
// CSteamID (8 bytes) is passed by value like a uint64.

enum {
    MM_RequestLobbyList = 4,
    MM_AddStringFilter = 5,
    MM_AddNumericalFilter = 6,
    MM_AddNearValueFilter = 7,
    MM_AddDistanceFilter = 9,
    MM_CreateLobby = 13,
    MM_JoinLobby = 14,
    MM_GetLobbyData = 19,
    MM_SetLobbyData = 20,
    MM_DeleteLobbyData = 23,
};

typedef uint64_t    (*RequestLobbyList_t)(void *);
typedef void        (*AddStringFilter_t)(void *, const char *, const char *, int);
typedef void        (*AddNumericalFilter_t)(void *, const char *, int, int);
typedef void        (*AddNearValueFilter_t)(void *, const char *, int);
typedef void        (*AddDistanceFilter_t)(void *, int);
typedef uint64_t    (*CreateLobby_t)(void *, int, int);
typedef uint64_t    (*JoinLobby_t)(void *, uint64_t);
typedef const char *(*GetLobbyData_t)(void *, uint64_t, const char *);
typedef int8_t      (*SetLobbyData_t)(void *, uint64_t, const char *, const char *);
typedef int8_t      (*DeleteLobbyData_t)(void *, uint64_t, const char *);

static RequestLobbyList_t   o_RequestLobbyList;
static AddStringFilter_t    o_AddStringFilter;
static AddNumericalFilter_t o_AddNumericalFilter;
static AddNearValueFilter_t o_AddNearValueFilter;
static AddDistanceFilter_t  o_AddDistanceFilter;
static CreateLobby_t        o_CreateLobby;
static JoinLobby_t          o_JoinLobby;
static GetLobbyData_t       o_GetLobbyData;
static SetLobbyData_t       o_SetLobbyData;
static DeleteLobbyData_t    o_DeleteLobbyData;

static uint64_t h_RequestLobbyList(void *self)
{
    o_AddStringFilter(self, "jcoop_room", g_room, 0 /* k_ELobbyComparisonEqual */);
    o_AddDistanceFilter(self, 3 /* k_ELobbyDistanceFilterWorldwide */);
    uint64_t call = o_RequestLobbyList(self);
    logf_("RequestLobbyList (room filter added) -> call %llu", (unsigned long long)call);
    return call;
}

static void h_AddStringFilter(void *self, const char *key, const char *val, int cmp)
{
    logf_("  filter string %s %d '%s'", key, cmp, val ? val : "");
    o_AddStringFilter(self, map_key(key, 0), val, cmp);
}

static void h_AddNumericalFilter(void *self, const char *key, int val, int cmp)
{
    logf_("  filter number %s %d %d", key, cmp, val);
    o_AddNumericalFilter(self, map_key(key, 0), val, cmp);
}

static void h_AddNearValueFilter(void *self, const char *key, int val)
{
    logf_("  filter near %s %d", key, val);
    o_AddNearValueFilter(self, map_key(key, 0), val);
}

static void h_AddDistanceFilter(void *self, int d)
{
    // The game's own distance filter is ignored: we always search worldwide
    // (added in RequestLobbyList), so the pair can be anywhere.
    logf_("  game distance filter %d ignored (worldwide)", d);
    (void)self;
}

static uint64_t h_CreateLobby(void *self, int type, int max)
{
    logf_("CreateLobby type=%d max=%d", type, max);
    return o_CreateLobby(self, type, max);
}

static uint64_t h_JoinLobby(void *self, uint64_t lobby)
{
    logf_("JoinLobby %llu", (unsigned long long)lobby);
    return o_JoinLobby(self, lobby);
}

static const char *h_GetLobbyData(void *self, uint64_t lobby, const char *key)
{
    return o_GetLobbyData(self, lobby, map_key(key, 1));
}

static int8_t h_SetLobbyData(void *self, uint64_t lobby, const char *key, const char *val)
{
    o_SetLobbyData(self, lobby, "jcoop_room", g_room);
    logf_("SetLobbyData %llu %s='%s'", (unsigned long long)lobby, key ? key : "", val ? val : "");
    return o_SetLobbyData(self, lobby, map_key(key, 2), val);
}

static int8_t h_DeleteLobbyData(void *self, uint64_t lobby, const char *key)
{
    return o_DeleteLobbyData(self, lobby, map_key(key, 3));
}

static void hook_matchmaking(void *mm)
{
    void **v = *(void ***)mm;
    patch(v, MM_RequestLobbyList,   (void *)h_RequestLobbyList,   (void **)&o_RequestLobbyList);
    patch(v, MM_AddStringFilter,    (void *)h_AddStringFilter,    (void **)&o_AddStringFilter);
    patch(v, MM_AddNumericalFilter, (void *)h_AddNumericalFilter, (void **)&o_AddNumericalFilter);
    patch(v, MM_AddNearValueFilter, (void *)h_AddNearValueFilter, (void **)&o_AddNearValueFilter);
    patch(v, MM_AddDistanceFilter,  (void *)h_AddDistanceFilter,  (void **)&o_AddDistanceFilter);
    patch(v, MM_CreateLobby,        (void *)h_CreateLobby,        (void **)&o_CreateLobby);
    patch(v, MM_JoinLobby,          (void *)h_JoinLobby,          (void **)&o_JoinLobby);
    patch(v, MM_GetLobbyData,       (void *)h_GetLobbyData,       (void **)&o_GetLobbyData);
    patch(v, MM_SetLobbyData,       (void *)h_SetLobbyData,       (void **)&o_SetLobbyData);
    patch(v, MM_DeleteLobbyData,    (void *)h_DeleteLobbyData,    (void **)&o_DeleteLobbyData);
    logf_("ISteamMatchmaking hooked");
}

// ---------------------------------------------------------------------------
// ISteamNetworking (SteamNetworking005) hooks

enum { NET_Send = 0, NET_Read = 2, NET_Accept = 3 };

typedef int8_t (*SendP2P_t)(void *, uint64_t, const void *, uint32_t, int, int);
typedef int8_t (*ReadP2P_t)(void *, void *, uint32_t, uint32_t *, uint64_t *, int);
typedef int8_t (*AcceptP2P_t)(void *, uint64_t);

static SendP2P_t   o_SendP2P;
static ReadP2P_t   o_ReadP2P;
static AcceptP2P_t o_AcceptP2P;

#define MAX_SEEN 16
static uint64_t g_seen[MAX_SEEN];
static void note_peer(uint64_t id, const char *what)
{
    for (int i = 0; i < MAX_SEEN; ++i) {
        if (g_seen[i] == id) return;
        if (!g_seen[i]) {
            g_seen[i] = id;
            logf_("peer %s: SteamID64 %llu%s", what, (unsigned long long)id,
                  (g_partner && id != g_partner) ? "  (NOT partner -> blocked)" : "");
            return;
        }
    }
}

static int allowed(uint64_t id) { return !g_partner || id == g_partner; }

static int8_t h_SendP2P(void *self, uint64_t to, const void *data, uint32_t n, int type, int ch)
{
    if (!allowed(to)) { note_peer(to, "send"); return 0; }
    return o_SendP2P(self, to, data, n, type, ch);
}

static int8_t h_ReadP2P(void *self, void *buf, uint32_t cb, uint32_t *size, uint64_t *from, int ch)
{
    for (int i = 0; i < 64; ++i) {
        int8_t ok = o_ReadP2P(self, buf, cb, size, from, ch);
        if (!ok) return ok;
        note_peer(*from, "connected");
        if (allowed(*from)) return ok;
        // drop stranger packet and try the next one
    }
    return 0;
}

static int8_t h_AcceptP2P(void *self, uint64_t id)
{
    note_peer(id, "session request");
    if (!allowed(id)) return 0;
    return o_AcceptP2P(self, id);
}

static void hook_networking(void *net)
{
    void **v = *(void ***)net;
    patch(v, NET_Send,   (void *)h_SendP2P,   (void **)&o_SendP2P);
    patch(v, NET_Read,   (void *)h_ReadP2P,   (void **)&o_ReadP2P);
    patch(v, NET_Accept, (void *)h_AcceptP2P, (void **)&o_AcceptP2P);
    logf_("ISteamNetworking hooked");
}

// ---------------------------------------------------------------------------
// ISteamClient017 hooks: catch the matchmaking / networking interfaces

enum { CL_GetISteamMatchmaking = 10, CL_GetISteamNetworking = 16 };
typedef void *(*GetIface_t)(void *, int32_t, int32_t, const char *);
static GetIface_t o_GetMM, o_GetNet;

static void *h_GetMM(void *self, int32_t user, int32_t pipe, const char *ver)
{
    void *r = o_GetMM(self, user, pipe, ver);
    if (r && ver && strcmp(ver, "SteamMatchMaking009") == 0) hook_matchmaking(r);
    else if (r) logf_("unexpected matchmaking version %s, not hooked", ver ? ver : "?");
    return r;
}

static void *h_GetNet(void *self, int32_t user, int32_t pipe, const char *ver)
{
    void *r = o_GetNet(self, user, pipe, ver);
    if (r && ver && strcmp(ver, "SteamNetworking005") == 0) hook_networking(r);
    else if (r) logf_("unexpected networking version %s, not hooked", ver ? ver : "?");
    return r;
}

// ---------------------------------------------------------------------------
// exported entry point (see steam_api64.def)

typedef void *(*CreateInterface_t)(const char *);
static CreateInterface_t o_CreateInterface;

__declspec(dllexport) void *Proxy_SteamInternal_CreateInterface(const char *ver)
{
    if (!o_CreateInterface) {
        char path[MAX_PATH + 32];
        snprintf(path, sizeof path, "%ssteam_api64_o.dll", g_dir);
        HMODULE h = GetModuleHandleA("steam_api64_o.dll");
        if (!h) h = LoadLibraryA(path);
        if (h) o_CreateInterface = (CreateInterface_t)GetProcAddress(h, "SteamInternal_CreateInterface");
        if (!o_CreateInterface) { logf_("FATAL: steam_api64_o.dll not found"); return NULL; }
    }
    void *r = o_CreateInterface(ver);
    logf_("CreateInterface(%s) -> %p", ver ? ver : "?", r);
    if (r && g_room[0] && ver && strcmp(ver, "SteamClient017") == 0) {
        void **v = *(void ***)r;
        patch(v, CL_GetISteamMatchmaking, (void *)h_GetMM,  (void **)&o_GetMM);
        patch(v, CL_GetISteamNetworking,  (void *)h_GetNet, (void **)&o_GetNet);
        logf_("ISteamClient017 hooked");
    }
    return r;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        load_config(inst);
    } else if (reason == DLL_PROCESS_DETACH && g_log) {
        logf_("unloading");
        fclose(g_log);
        g_log = NULL;
    }
    return TRUE;
}
