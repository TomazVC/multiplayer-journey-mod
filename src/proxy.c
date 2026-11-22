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
static int      g_debug = 1;
static FILE    *g_log;
static CRITICAL_SECTION g_log_cs;
static CRITICAL_SECTION g_peer_cs;

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
    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_peer_cs);
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
    g_debug  = GetPrivateProfileIntA("coop", "debug", 1, ini);
    if (g_log_on) {
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

// ---------------------------------------------------------------------------
// Generic tracers: log every call the game makes on each vtable slot (first 3
// calls, then on every power of two) and forward with all argument registers
// plus 4 stack slots untouched. Specific hooks are installed on top of these.

typedef uint64_t (*Any8_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static const char *MM_NAMES[38] = {
    "GetFavoriteGameCount", "GetFavoriteGame", "AddFavoriteGame", "RemoveFavoriteGame",
    "RequestLobbyList", "AddRequestLobbyListStringFilter", "AddRequestLobbyListNumericalFilter",
    "AddRequestLobbyListNearValueFilter", "AddRequestLobbyListFilterSlotsAvailable",
    "AddRequestLobbyListDistanceFilter", "AddRequestLobbyListResultCountFilter",
    "AddRequestLobbyListCompatibleMembersFilter", "GetLobbyByIndex", "CreateLobby", "JoinLobby",
    "LeaveLobby", "InviteUserToLobby", "GetNumLobbyMembers", "GetLobbyMemberByIndex", "GetLobbyData",
    "SetLobbyData", "GetLobbyDataCount", "GetLobbyDataByIndex", "DeleteLobbyData", "GetLobbyMemberData",
    "SetLobbyMemberData", "SendLobbyChatMsg", "GetLobbyChatEntry", "RequestLobbyData",
    "SetLobbyGameServer", "GetLobbyGameServer", "SetLobbyMemberLimit", "GetLobbyMemberLimit",
    "SetLobbyType", "SetLobbyJoinable", "GetLobbyOwner", "SetLobbyOwner", "SetLinkedLobby",
};
static const char *NET_NAMES[22] = {
    "SendP2PPacket", "IsP2PPacketAvailable", "ReadP2PPacket", "AcceptP2PSessionWithUser",
    "CloseP2PSessionWithUser", "CloseP2PChannelWithUser", "GetP2PSessionState", "AllowP2PPacketRelay",
    "CreateListenSocket", "CreateP2PConnectionSocket", "CreateConnectionSocket", "DestroySocket",
    "DestroyListenSocket", "SendDataOnSocket", "IsDataAvailableOnSocket", "RetrieveDataFromSocket",
    "IsDataAvailable", "RetrieveData", "GetSocketInfo", "GetListenSocketInfo",
    "GetSocketConnectionType", "GetMaxPacketSize",
};

static void *g_mm_orig[38], *g_net_orig[22];
static volatile LONG g_mm_cnt[38], g_net_cnt[22];

static void trace(const char *iface, const char *name, volatile LONG *cnt)
{
    LONG n = InterlockedIncrement(cnt);
    if (n <= 3 || (n & (n - 1)) == 0) logf_("call %s::%s (#%ld)", iface, name, n);
}

#define TR(kind, i)                                                                        \
    static uint64_t tr_##kind##_##i(uint64_t a, uint64_t b, uint64_t c, uint64_t d,       \
                                    uint64_t e, uint64_t f, uint64_t g, uint64_t h)       \
    {                                                                                      \
        trace(#kind, kind##_NAMES_U[i], &g_##kind##_cnt[i]);                               \
        return ((Any8_t)g_##kind##_orig[i])(a, b, c, d, e, f, g, h);                       \
    }
#define mm_NAMES_U MM_NAMES
#define net_NAMES_U NET_NAMES
TR(mm, 0) TR(mm, 1) TR(mm, 2) TR(mm, 3) TR(mm, 4) TR(mm, 5) TR(mm, 6) TR(mm, 7) TR(mm, 8) TR(mm, 9) TR(mm, 10) TR(mm, 11) TR(mm, 12) TR(mm, 13) TR(mm, 14) TR(mm, 15) TR(mm, 16) TR(mm, 17) TR(mm, 18) TR(mm, 19) TR(mm, 20) TR(mm, 21) TR(mm, 22) TR(mm, 23) TR(mm, 24) TR(mm, 25) TR(mm, 26) TR(mm, 27) TR(mm, 28) TR(mm, 29) TR(mm, 30) TR(mm, 31) TR(mm, 32) TR(mm, 33) TR(mm, 34) TR(mm, 35) TR(mm, 36) TR(mm, 37)
TR(net, 0) TR(net, 1) TR(net, 2) TR(net, 3) TR(net, 4) TR(net, 5) TR(net, 6) TR(net, 7) TR(net, 8) TR(net, 9) TR(net, 10) TR(net, 11) TR(net, 12) TR(net, 13) TR(net, 14) TR(net, 15) TR(net, 16) TR(net, 17) TR(net, 18) TR(net, 19) TR(net, 20) TR(net, 21)

static void *MM_TR[38] = {
    tr_mm_0, tr_mm_1, tr_mm_2, tr_mm_3, tr_mm_4, tr_mm_5, tr_mm_6, tr_mm_7, tr_mm_8, tr_mm_9,
    tr_mm_10, tr_mm_11, tr_mm_12, tr_mm_13, tr_mm_14, tr_mm_15, tr_mm_16, tr_mm_17, tr_mm_18, tr_mm_19,
    tr_mm_20, tr_mm_21, tr_mm_22, tr_mm_23, tr_mm_24, tr_mm_25, tr_mm_26, tr_mm_27, tr_mm_28, tr_mm_29,
    tr_mm_30, tr_mm_31, tr_mm_32, tr_mm_33, tr_mm_34, tr_mm_35, tr_mm_36, tr_mm_37,
};
static void *NET_TR[22] = {
    tr_net_0, tr_net_1, tr_net_2, tr_net_3, tr_net_4, tr_net_5, tr_net_6, tr_net_7, tr_net_8, tr_net_9,
    tr_net_10, tr_net_11, tr_net_12, tr_net_13, tr_net_14, tr_net_15, tr_net_16, tr_net_17, tr_net_18,
    tr_net_19, tr_net_20, tr_net_21,
};

static void hook_matchmaking(void *mm)
{
    static int done; if (done) return; done = 1;
    void **v = *(void ***)mm;
    for (int i = 0; i < 38; ++i) patch(v, i, MM_TR[i], &g_mm_orig[i]);
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

// --- link warm-up -----------------------------------------------------------
// Long-distance Steam P2P (e.g. Brazil <-> Italy) can take 10-15 s before the
// route is up. Unreliable packets sent in that window can be thrown away, so the
// game's handshake reply never reaches the other side and its ~20 s timeout
// drops the connection (one side sees the other, the other sees nobody).
// Fix: until we have received something from a peer, every packet to it is sent
// Reliable (Steam queues it and delivers once the route is up). "NoDelay" sends,
// which Steam discards when the route isn't ready, become plain Unreliable.

enum { P2P_Unreliable = 0, P2P_UnreliableNoDelay = 1, P2P_Reliable = 2, P2P_ReliableBuffered = 3 };
#define WARMUP_MAX_MS 60000

typedef struct { uint64_t id; DWORD first_send, first_recv; uint32_t upgraded; } Peer;
static Peer g_peers[8];


static Peer *peer_get(uint64_t id)
{
    Peer *free_slot = NULL;
    for (int i = 0; i < 8; ++i) {
        if (g_peers[i].id == id) return &g_peers[i];
        if (!g_peers[i].id && !free_slot) free_slot = &g_peers[i];
    }
    if (!free_slot) { free_slot = &g_peers[0]; }
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->id = id;
    return free_slot;
}

static uint32_t g_type_seen; // bit (type*8 + ch) for ch < 8
static int8_t h_SendP2P(void *self, uint64_t to, const void *data, uint32_t n, int type, int ch)
{
    if (!allowed(to)) { note_peer(to, "send"); return 0; }

    if (type >= 0 && type < 4 && ch >= 0 && ch < 8) {
        uint32_t bit = 1u << (type * 8 + ch);
        if (!(g_type_seen & bit)) {
            g_type_seen |= bit;
            logf_("game sends type %d on channel %d (e.g. %u bytes)", type, ch, n);
        }
    }

    int t = type == P2P_UnreliableNoDelay ? P2P_Unreliable : type;
    EnterCriticalSection(&g_peer_cs);
    Peer *p = peer_get(to);
    DWORD now = GetTickCount();
    if (!p->first_send) {
        p->first_send = now ? now : 1;
        logf_("link: first packet to %llu, warm-up on (reliable until they answer)", (unsigned long long)to);
    }
    if (!p->first_recv && now - p->first_send < WARMUP_MAX_MS && t == P2P_Unreliable && n <= 1200) {
        t = P2P_Reliable;
        p->upgraded++;
    }
    LeaveCriticalSection(&g_peer_cs);
    return o_SendP2P(self, to, data, n, t, ch);
}

static void link_heard(uint64_t id)
{
    EnterCriticalSection(&g_peer_cs);
    Peer *p = peer_get(id);
    if (!p->first_recv) {
        DWORD now = GetTickCount();
        p->first_recv = now ? now : 1;
        if (p->first_send)
            logf_("link: first packet from %llu, %lu ms after our first send (%u packets sent reliable during warm-up)",
                  (unsigned long long)id, (unsigned long)(now - p->first_send), p->upgraded);
        else
            logf_("link: first packet from %llu (they spoke first)", (unsigned long long)id);
    }
    LeaveCriticalSection(&g_peer_cs);
}

static int8_t h_ReadP2P(void *self, void *buf, uint32_t cb, uint32_t *size, uint64_t *from, int ch)
{
    for (int i = 0; i < 64; ++i) {
        int8_t ok = o_ReadP2P(self, buf, cb, size, from, ch);
        if (!ok) return ok;
        note_peer(*from, "connected");
        if (allowed(*from)) { link_heard(*from); return ok; }
        // drop stranger packet and try the next one
    }
    return 0;
}

typedef int8_t (*CloseP2P_t)(void *, uint64_t);
static CloseP2P_t o_CloseP2P;
static int8_t h_CloseP2P(void *self, uint64_t id)
{
    EnterCriticalSection(&g_peer_cs);
    Peer *p = peer_get(id);
    DWORD now = GetTickCount();
    logf_("link: game closed session with %llu (sent for %lu s, heard from them: %s)",
          (unsigned long long)id, p->first_send ? (unsigned long)((now - p->first_send) / 1000) : 0UL,
          p->first_recv ? "yes" : "NO");
    memset(p, 0, sizeof *p); // next session warms up again
    LeaveCriticalSection(&g_peer_cs);
    return o_CloseP2P(self, id);
}

static int8_t h_AcceptP2P(void *self, uint64_t id)
{
    note_peer(id, "session request");
    if (!allowed(id)) return 0;
    return o_AcceptP2P(self, id);
}

static void hook_networking(void *net)
{
    static int done; if (done) return; done = 1;
    void **v = *(void ***)net;
    for (int i = 0; i < 22; ++i) patch(v, i, NET_TR[i], &g_net_orig[i]);
    patch(v, NET_Send,   (void *)h_SendP2P,   (void **)&o_SendP2P);
    patch(v, NET_Read,   (void *)h_ReadP2P,   (void **)&o_ReadP2P);
    patch(v, NET_Accept, (void *)h_AcceptP2P, (void **)&o_AcceptP2P);
    patch(v, 4 /* CloseP2PSessionWithUser */, (void *)h_CloseP2P, (void **)&o_CloseP2P);
    logf_("ISteamNetworking hooked (warm-up: reliable until partner answers)");
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
// Capture the game's own debug output (OutputDebugStringA) into our log, so we
// can see Journey's internal "MatchMaking::..." / "Matchmaker::..." messages.

typedef void (WINAPI *ODS_t)(LPCSTR);
static ODS_t o_ODS;


static void WINAPI h_ODS(LPCSTR s)
{
    if (s && g_log) {
        char line[1024];
        size_t n = strlen(s);
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, s, n);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) --n;
        line[n] = 0;
        if (n) logf_("game| %s", line);
    }
    if (o_ODS) o_ODS(s);
}

static void hook_iat(HMODULE mod, const char *dll, const char *fn, void *hook, void **orig)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp((char *)(base + d->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *iat   = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, fn) != 0) continue;
            DWORD old;
            VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            *orig = (void *)iat->u1.Function;
            iat->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            logf_("IAT hook %s!%s ok", dll, fn);
            return;
        }
    }
    logf_("IAT hook %s!%s not found", dll, fn);
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
    static int iat_done;
    if (!iat_done) {
        iat_done = 1;
        if (g_debug && g_log)
            hook_iat(GetModuleHandleA(NULL), "KERNEL32.dll", "OutputDebugStringA", (void *)h_ODS, (void **)&o_ODS);
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
