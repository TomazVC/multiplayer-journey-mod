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
//   3. Learns the partner's SteamID (saved as partner= in the ini), drops P2P
//      traffic from anyone else, and keeps a permanent warm P2P link with the
//      partner so long-distance connections don't time out (see networking part).
//
// Everything else is forwarded untouched through the .def file.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "rtp.h"

// ---------------------------------------------------------------------------
// config + log

static char     g_dir[MAX_PATH];
static char     g_room[40];
static uint64_t g_partner;
static int      g_log_on = 1;
static int      g_debug = 1;
static FILE    *g_log;
static CRITICAL_SECTION g_log_cs;
static CRITICAL_SECTION g_net_cs;
static char     g_ini[MAX_PATH + 32];

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
    InitializeCriticalSection(&g_net_cs);
    GetModuleFileNameA(self, g_dir, MAX_PATH);
    char *slash = strrchr(g_dir, '\\');
    if (slash) slash[1] = 0;

    char raw[128], logpath[MAX_PATH + 32];
    char *ini = g_ini;
    snprintf(g_ini, sizeof g_ini, "%sjourney-coop.ini", g_dir);

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

static uint64_t h_GetLobbyMember(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

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
    void *dummy;
    patch(v, 18 /* GetLobbyMemberByIndex */, (void *)h_GetLobbyMember, &dummy);
    logf_("ISteamMatchmaking hooked");
}

// ---------------------------------------------------------------------------
// ISteamNetworking (SteamNetworking005) + the permanent partner link
//
// Problem seen between Brazil and Italy: a fresh Steam P2P route takes 14-20 s
// to come up in one direction, and Journey gives up on a connection after ~20 s
// without hearing back. Result: one player sees the other, the other sees nobody.
//
// Fix: keep our OWN Steam P2P session with the partner open all the time, with a
// small ping/pong once per second on a private channel (LINK_CH). The route is
// then always warm, so when the game connects its packets flow at once.
//
// Keeping the session open changes what Steam tells the game, so the mod
// emulates the normal behaviour for the partner:
//   * the game "closing" the session only marks it closed for the game
//     (the Steam session, and the warm route, stay up);
//   * while the game considers the partner disconnected, the partner's game
//     packets are held back, and the game gets a synthesized
//     P2PSessionRequest_t exactly like Steam would send for a new session;
//   * Steam session requests / connect failures for the partner are handled by
//     the mod and not shown to the game while it isn't connected to the partner.
//
// The partner's SteamID is learned from the lobby (only our pair can be in a
// room with our code) or from the first P2P contact, and saved to the ini so the
// next launch warms the link right away.

enum { NET_Send = 0, NET_Avail = 1, NET_Read = 2, NET_Accept = 3, NET_Close = 4 };
#define LINK_CH 7
#define GAME_CHANNELS 4           // game channels the mod holds back while idle (0..3)
#define QMAX 512

typedef int8_t (*SendP2P_t)(void *, uint64_t, const void *, uint32_t, int, int);
typedef int8_t (*AvailP2P_t)(void *, uint32_t *, int);
typedef int8_t (*ReadP2P_t)(void *, void *, uint32_t, uint32_t *, uint64_t *, int);
typedef int8_t (*AcceptP2P_t)(void *, uint64_t);
typedef int8_t (*CloseP2P_t)(void *, uint64_t);

static void *g_net;               // the ISteamNetworking005 object
static void *g_client;            // the ISteamClient017 object
static uint64_t g_self;           // our own SteamID64 (0 = unknown yet)
static int g_abi_hidden = -1;     // 1 = CSteamID returned through hidden pointer (MSVC member ABI)

#define R_SEND(to, d, n, t, ch)       ((SendP2P_t)g_net_orig[NET_Send])(g_net, to, d, n, t, ch)
#define R_AVAIL(sz, ch)               ((AvailP2P_t)g_net_orig[NET_Avail])(g_net, sz, ch)
#define R_READ(b, cb, sz, from, ch)   ((ReadP2P_t)g_net_orig[NET_Read])(g_net, b, cb, sz, from, ch)
#define R_ACCEPT(id)                  ((AcceptP2P_t)g_net_orig[NET_Accept])(g_net, id)

static CRITICAL_SECTION g_net_cs; // guards everything below

enum { GAME_IDLE = 0, GAME_ACTIVE = 1 };
static int   g_game_state = GAME_IDLE; // the game's view of its session with the partner
static int   g_notified;               // synthesized request already given since idle
static DWORD g_notified_at;
static DWORD g_closed_at;              // when the game last closed the partner session
static int   g_link_up;
static DWORD g_last_ping, g_last_heard, g_last_rtt_log, g_link_since;

typedef struct { uint64_t from; uint32_t n; int ch; uint8_t *data; } QPkt;
static QPkt g_q[QMAX];
static int  g_qn;

// --- own reliable transport for the game's packets (see rtp.h) ---------------
#define DATA_CH 8
static Rtp g_rtp;
static uint32_t g_q_drops;

static void rtp_tx_cb(void *ctx, const void *frame, uint32_t n)
{
    (void)ctx;
    if (g_partner) R_SEND(g_partner, frame, n, 0 /* unreliable */, DATA_CH);
}

static void rtp_rx_cb(void *ctx, int ch, const void *data, uint32_t n)
{
    (void)ctx;
    if (g_qn >= QMAX) { ++g_q_drops; return; }
    uint8_t *buf = (uint8_t *)malloc(n ? n : 1);
    if (!buf) return;
    memcpy(buf, data, n);
    g_q[g_qn].from = g_partner; g_q[g_qn].n = n; g_q[g_qn].ch = ch; g_q[g_qn].data = buf;
    ++g_qn;
}

static uint32_t new_gen(void)
{
    uint32_t g;
    do { g = (GetTickCount() * 2654435761u) ^ ((uint32_t)rand() << 12) ^ (uint32_t)rand() ^ (uint32_t)g_self; }
    while (!g || g == g_rtp.dead_gen || g == g_rtp.s_gen);
    return g;
}

// per game-session counters (game channels only), for the log
static uint32_t g_s_sent, g_s_recv;
static DWORD    g_s_open, g_s_first_recv;
static int      g_s_checked;

static void session_open(const char *how)
{
    rtp_send_reset_at(&g_rtp, new_gen(), GetTickCount());   // fresh stream for this game session
    g_s_sent = g_s_recv = 0;
    g_s_open = GetTickCount();
    g_s_first_recv = 0;
    g_s_checked = 0;
    logf_("game %s partner session OPEN (link %s, %d packets waiting)", how,
          g_link_up ? "warm" : "NOT warm yet", g_qn);
}

static void session_count_recv(void)
{
    ++g_s_recv;
    if (!g_s_first_recv) {
        g_s_first_recv = GetTickCount();
        logf_("session: first game packet from partner after %lu ms", (unsigned long)(g_s_first_recv - g_s_open));
    }
}


static int looks_like_steamid(uint64_t v) { return (v >> 52) == 0x011 && (uint32_t)v != 0; }

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

static void set_partner(uint64_t id, const char *how)
{
    if (g_partner || !looks_like_steamid(id) || id == g_self) return;
    g_partner = id;
    char v[32];
    snprintf(v, sizeof v, "%llu", (unsigned long long)id);
    WritePrivateProfileStringA("coop", "partner", v, g_ini);
    logf_("partner learned (%s): SteamID64 %llu -> saved to journey-coop.ini, permanent link starts now",
          how, (unsigned long long)id);
}

// --- held-back packet queue -------------------------------------------------

static int q_find(int ch)
{
    for (int i = 0; i < g_qn; ++i) if (g_q[i].ch == ch) return i;
    return -1;
}

static void q_drop(int i)
{
    free(g_q[i].data);
    memmove(&g_q[i], &g_q[i + 1], (size_t)(g_qn - i - 1) * sizeof g_q[0]);
    --g_qn;
}

static void q_clear(void) { while (g_qn) q_drop(g_qn - 1); }

// --- Steam callbacks (P2PSessionRequest_t 1202, P2PSessionConnectFail_t 1203) --
// CCallbackBase layout (MSVC x64): vtable, uint8 flags @8, int iCallback @12.
// vtable: [0] Run(param, bool, SteamAPICall_t) [1] Run(param) [2] GetCallbackSizeBytes()
// (for CCallback both Run overloads do the same thing).

typedef struct CCB { void **vtbl; uint8_t flags; int icb; } CCB;
typedef void (*RegCB_t)(void *, int);
typedef void (*UnregCB_t)(void *);
typedef void (*RunCB_t)(void *, void *, uint64_t, uint64_t);

static CCB *g_game_req_cb, *g_game_fail_cb;
static CCB  g_my_req_cb, g_my_fail_cb;

static void game_cb_run(CCB *cb, void *param)
{
    if (cb) ((RunCB_t)cb->vtbl[1])(cb, param, 0, 0);
}

static void cb_req_run(CCB *self, void *param, uint64_t a, uint64_t b)
{
    (void)self; (void)a; (void)b;
    uint64_t id = *(uint64_t *)param;
    if (!g_net) { game_cb_run(g_game_req_cb, param); return; }
    EnterCriticalSection(&g_net_cs);
    if (!g_partner) set_partner(id, "incoming P2P");
    int partner = g_partner && id == g_partner;
    int active = g_game_state == GAME_ACTIVE;
    LeaveCriticalSection(&g_net_cs);
    if (partner) {
        R_ACCEPT(id); // keep the route; the game is told later, when its own packets arrive
        logf_("link: Steam session request from partner, accepted by the mod (game %s)",
              active ? "connected -> forwarded" : "not connected -> held");
        if (active) game_cb_run(g_game_req_cb, param);
        return;
    }
    game_cb_run(g_game_req_cb, param);
}

static void cb_fail_run(CCB *self, void *param, uint64_t a, uint64_t b)
{
    (void)self; (void)a; (void)b;
    uint64_t id = *(uint64_t *)param;
    int err = ((uint8_t *)param)[8];
    EnterCriticalSection(&g_net_cs);
    int partner = g_partner && id == g_partner;
    int active = g_game_state == GAME_ACTIVE;
    if (partner) g_link_up = 0;
    LeaveCriticalSection(&g_net_cs);
    logf_("link: Steam P2P connect FAIL with %llu (error %d)%s", (unsigned long long)id, err,
          partner && !active ? " -> hidden from game, mod keeps retrying" : "");
    if (!partner || active) game_cb_run(g_game_fail_cb, param);
}

static int cb_req_size(CCB *s)  { (void)s; return 8; }
static int cb_fail_size(CCB *s) { (void)s; return 16; }
static void *g_my_req_vt[3]  = { (void *)cb_req_run,  (void *)cb_req_run,  (void *)cb_req_size };
static void *g_my_fail_vt[3] = { (void *)cb_fail_run, (void *)cb_fail_run, (void *)cb_fail_size };

static FARPROC orig_fn(const char *name)
{
    HMODULE h = GetModuleHandleA("steam_api64_o.dll");
    if (!h) {
        char path[MAX_PATH + 32];
        snprintf(path, sizeof path, "%ssteam_api64_o.dll", g_dir);
        h = LoadLibraryA(path);
    }
    return h ? GetProcAddress(h, name) : NULL;
}

__declspec(dllexport) void Proxy_SteamAPI_RegisterCallback(void *cb, int id)
{
    static RegCB_t real;
    if (!real) real = (RegCB_t)orig_fn("SteamAPI_RegisterCallback");
    CCB *c = (CCB *)cb;
    if (g_room[0] && (id == 1202 || id == 1203)) {
        CCB *mine = id == 1202 ? &g_my_req_cb : &g_my_fail_cb;
        if (id == 1202) g_game_req_cb = c; else g_game_fail_cb = c;
        c->flags |= 1;     // look registered to the game's own Unregister()
        c->icb = id;
        mine->vtbl = id == 1202 ? g_my_req_vt : g_my_fail_vt;
        real(mine, id);
        logf_("Steam callback %d (%s) routed through the mod", id, id == 1202 ? "P2PSessionRequest" : "P2PSessionConnectFail");
        return;
    }
    real(cb, id);
}

__declspec(dllexport) void Proxy_SteamAPI_UnregisterCallback(void *cb)
{
    static UnregCB_t real;
    if (!real) real = (UnregCB_t)orig_fn("SteamAPI_UnregisterCallback");
    if (cb && (cb == g_game_req_cb || cb == g_game_fail_cb)) {
        CCB *mine = cb == g_game_req_cb ? &g_my_req_cb : &g_my_fail_cb;
        if (cb == g_game_req_cb) g_game_req_cb = NULL; else g_game_fail_cb = NULL;
        ((CCB *)cb)->flags &= (uint8_t)~1;
        real(mine);
        return;
    }
    real(cb);
}

// --- own SteamID (ISteamUser019::GetSteamID, vtable slot 2) -----------------

static void ensure_self(void)
{
    static int tries;
    if (g_self || !g_client || tries > 5) return;
    ++tries;
    typedef int32_t (*GetH_t)(void);
    GetH_t gu = (GetH_t)orig_fn("SteamAPI_GetHSteamUser"), gp = (GetH_t)orig_fn("SteamAPI_GetHSteamPipe");
    if (!gu || !gp) return;
    int32_t hu = gu(), hp = gp();
    if (!hu || !hp) return;
    void *user = ((void *(*)(void *, int32_t, int32_t, const char *))(*(void ***)g_client)[5])(g_client, hu, hp, "SteamUser019");
    if (!user) return;
    uint64_t out = 0;
    uint64_t ret = (uint64_t)((uint64_t (*)(void *, uint64_t *))(*(void ***)user)[2])(user, &out);
    if (looks_like_steamid(out)) { g_self = out; g_abi_hidden = 1; }
    else if (looks_like_steamid(ret)) { g_self = ret; g_abi_hidden = 0; }
    if (g_self) logf_("my SteamID64: %llu", (unsigned long long)g_self);
    else logf_("could not read my SteamID (lobby partner learning off)");
}

// --- the pump: runs every frame from SteamAPI_RunCallbacks ----------------------

#pragma pack(push, 1)
typedef struct { char j, c, kind; uint32_t t; } LinkMsg; // kind 'P' ping, 'O' pong
#pragma pack(pop)

static void pump(void)
{
    if (!g_net || !g_room[0]) return;
    ensure_self();
    EnterCriticalSection(&g_net_cs);
    DWORD now = GetTickCount();
    uint64_t partner = g_partner;

    if (partner && now - g_last_ping >= 1000) {
        LinkMsg m = { 'J', 'C', 'P', now };
        R_SEND(partner, &m, sizeof m, 0 /* unreliable */, LINK_CH);
        g_last_ping = now;
    }

    uint32_t sz, got;
    uint64_t from;
    LinkMsg m;
    while (R_AVAIL(&sz, LINK_CH)) {
        if (!R_READ(&m, sizeof m, &got, &from, LINK_CH)) break;
        if (from != partner || got != sizeof m || m.j != 'J' || m.c != 'C') continue;
        g_last_heard = now;
        if (!g_link_up) {
            g_link_up = 1;
            g_link_since = now;
            logf_("link UP with partner %llu (route warm)", (unsigned long long)from);
        }
        if (m.kind == 'P') {
            LinkMsg r = { 'J', 'C', 'O', m.t };
            R_SEND(from, &r, sizeof r, 0, LINK_CH);
        } else if (m.kind == 'O') {
            uint32_t rtt = now - m.t, rto = rtt * 3 / 2 + 100;
            g_rtp.rto = rto < 200 ? 200 : rto > 1500 ? 1500 : rto;
            if (now - g_last_rtt_log >= 30000) {
                g_last_rtt_log = now;
                logf_("link: ping %lu ms", (unsigned long)rtt);
            }
        }
    }
    // game packets carried by our own transport
    {
        static uint8_t fbuf[2048];
        while (R_AVAIL(&sz, DATA_CH)) {
            if (!R_READ(fbuf, sizeof fbuf, &got, &from, DATA_CH)) break;
            if (from != partner) continue;
            uint32_t before = g_rtp.r_gen;
            rtp_on_frame(&g_rtp, fbuf, got, now);
            if (g_rtp.r_gen != before && g_rtp.r_gen)
                logf_("transport: partner started a new stream (gen %08x)", g_rtp.r_gen);
        }
        if (g_rtp.switched) {
            g_rtp.switched = 0;
            if (g_game_state == GAME_ACTIVE) {
                rtp_send_reset_at(&g_rtp, new_gen(), now);
                logf_("transport: partner restarted its session -> our stream restarted too");
            }
        }
        rtp_tick(&g_rtp, now);
    }

    if (g_game_state == GAME_ACTIVE && !g_s_checked && now - g_s_open > 10000) {
        g_s_checked = 1;
        logf_("session +10s: game sent %u packets, received %u%s (retransmits %u, in flight %u)", g_s_sent, g_s_recv,
              g_s_recv ? "" : "  <-- partner's game packets are NOT arriving",
              g_rtp.st_retx, g_rtp.s_next - g_rtp.s_una);
    }
    if (g_link_up && now - g_last_heard > 8000) {
        g_link_up = 0;
        logf_("link DOWN (no ping for 8 s, partner closed the game or network trouble); retrying");
    }

    // While the game is not connected to the partner, hold back the partner's
    // game packets and tell the game a new session is being requested.
    if (partner && g_game_state == GAME_IDLE) {
        for (int ch = 0; ch < GAME_CHANNELS; ++ch) {
            while (g_qn < QMAX && R_AVAIL(&sz, ch)) {
                uint8_t *buf = (uint8_t *)malloc(sz ? sz : 1);
                if (!buf) break;
                if (!R_READ(buf, sz, &got, &from, ch)) { free(buf); break; }
                if (from != partner || now - g_closed_at < 3000) { free(buf); continue; } // stranger / stale
                g_q[g_qn].from = from; g_q[g_qn].n = got; g_q[g_qn].ch = ch; g_q[g_qn].data = buf;
                ++g_qn;
            }
        }
        if (g_notified && now - g_notified_at > 5000) {
            logf_("link: game did not accept the partner within 5 s, dropping %d held packets", g_qn);
            g_notified = 0;
            q_clear();
            rtp_recv_kill(&g_rtp);
        }
        if (g_qn && !g_notified) {
            g_notified = 1;
            g_notified_at = now;
            LeaveCriticalSection(&g_net_cs);
            logf_("link: partner's game is connecting (%d packets held) -> notifying the game", g_qn);
            uint64_t req = partner;
            if (g_game_req_cb) game_cb_run(g_game_req_cb, &req);
            else logf_("WARNING: game has no P2PSessionRequest callback registered");
            return;
        }
    }
    LeaveCriticalSection(&g_net_cs);
}

typedef void (*RunCallbacks_t)(void);
__declspec(dllexport) void Proxy_SteamAPI_RunCallbacks(void)
{
    static RunCallbacks_t real;
    if (!real) real = (RunCallbacks_t)orig_fn("SteamAPI_RunCallbacks");
    pump();
    real();
}

// --- game-facing hooks ----------------------------------------------------

static uint32_t g_type_seen; // bit (type*8 + ch) for ch < 8

static int8_t h_SendP2P(void *self, uint64_t to, const void *data, uint32_t n, int type, int ch)
{
    (void)self;
    if (!allowed(to)) { note_peer(to, "send"); return 0; }
    if (type >= 0 && type < 4 && ch >= 0 && ch < 8) {
        uint32_t bit = 1u << (type * 8 + ch);
        if (!(g_type_seen & bit)) {
            g_type_seen |= bit;
            logf_("game sends type %d on channel %d (e.g. %u bytes)", type, ch, n);
        }
    }
    EnterCriticalSection(&g_net_cs);
    if (!g_partner) set_partner(to, "game connecting to it");
    if (to == g_partner && g_game_state != GAME_ACTIVE) {
        g_game_state = GAME_ACTIVE;
        session_open("->");
    }
    if (to == g_partner && ch >= 0 && ch < GAME_CHANNELS) {
        ++g_s_sent;
        int rc = rtp_send(&g_rtp, ch, data, n, GetTickCount());
        if (rc == 0) { LeaveCriticalSection(&g_net_cs); return 1; }
        static int warned;
        if (warned++ < 5) logf_("transport: send rc %d (%u bytes) -> using Steam directly", rc, n);
    }
    LeaveCriticalSection(&g_net_cs);
    return R_SEND(to, data, n, type, ch);
}

static int8_t h_AvailP2P(void *self, uint32_t *size, int ch)
{
    (void)self;
    if (ch == LINK_CH || ch == DATA_CH) return 0;
    EnterCriticalSection(&g_net_cs);
    int held = g_partner && g_game_state == GAME_IDLE && ch < GAME_CHANNELS;
    int i = g_game_state == GAME_ACTIVE ? q_find(ch) : -1;
    if (i >= 0) { *size = g_q[i].n; LeaveCriticalSection(&g_net_cs); return 1; }
    LeaveCriticalSection(&g_net_cs);
    if (held) return 0; // partner packets wait in the pump until the game accepts
    return R_AVAIL(size, ch);
}

static int8_t h_ReadP2P(void *self, void *buf, uint32_t cb, uint32_t *size, uint64_t *from, int ch)
{
    (void)self;
    if (ch == LINK_CH || ch == DATA_CH) return 0;
    EnterCriticalSection(&g_net_cs);
    int held = g_partner && g_game_state == GAME_IDLE && ch < GAME_CHANNELS;
    int i = g_game_state == GAME_ACTIVE ? q_find(ch) : -1;
    if (i >= 0) {
        uint32_t n = g_q[i].n < cb ? g_q[i].n : cb;
        memcpy(buf, g_q[i].data, n);
        *size = n;
        *from = g_q[i].from;
        q_drop(i);
        session_count_recv();
        LeaveCriticalSection(&g_net_cs);
        return 1;
    }
    LeaveCriticalSection(&g_net_cs);
    if (held) return 0;
    for (int k = 0; k < 64; ++k) {
        int8_t ok = R_READ(buf, cb, size, from, ch);
        if (!ok) return ok;
        note_peer(*from, "connected");
        if (allowed(*from)) {
            if (g_partner && *from == g_partner) {
                EnterCriticalSection(&g_net_cs);
                session_count_recv();
                LeaveCriticalSection(&g_net_cs);
            }
            return ok;
        }
        // drop stranger packet and try the next one
    }
    return 0;
}

static int8_t h_AcceptP2P(void *self, uint64_t id)
{
    (void)self;
    note_peer(id, "session request");
    EnterCriticalSection(&g_net_cs);
    if (!g_partner) set_partner(id, "game accepted it");
    if (!allowed(id)) { LeaveCriticalSection(&g_net_cs); return 0; }
    if (id == g_partner && g_game_state != GAME_ACTIVE) {
        g_game_state = GAME_ACTIVE;
        session_open("<-");
    }
    LeaveCriticalSection(&g_net_cs);
    return R_ACCEPT(id);
}

static int8_t h_CloseP2P(void *self, uint64_t id)
{
    EnterCriticalSection(&g_net_cs);
    if (g_partner && id == g_partner) {
        int was_active = g_game_state == GAME_ACTIVE;
        DWORD now = GetTickCount();
        if (was_active)
            logf_("game closed its session with partner after %lu s (game sent %u, received %u packets)",
                  (unsigned long)((now - g_s_open) / 1000), g_s_sent, g_s_recv);
        g_game_state = GAME_IDLE;
        g_notified = 0;
        g_closed_at = now;
        q_clear();
        // End our streams for this session. Leftover packets of the partner's
        // old session are dropped by generation; the Steam session stays open.
        if (was_active)
            logf_("transport: session ended (retransmits %u, stale dropped %u, queue drops %u)",
                  g_rtp.st_retx, g_rtp.st_stale, g_q_drops);
        rtp_send_reset(&g_rtp, 0);
        rtp_recv_kill(&g_rtp);
        LeaveCriticalSection(&g_net_cs);
        return 1;
    }
    LeaveCriticalSection(&g_net_cs);
    return ((CloseP2P_t)g_net_orig[NET_Close])(self, id);
}

// GetLobbyMemberByIndex (matchmaking slot 18) returns a CSteamID; with the MSVC
// member ABI the result goes through a hidden pointer (2nd argument).
static uint64_t h_GetLobbyMember(uint64_t self, uint64_t b, uint64_t c, uint64_t d,
                                 uint64_t e, uint64_t f, uint64_t g, uint64_t h)
{
    uint64_t r = ((Any8_t)g_mm_orig[18])(self, b, c, d, e, f, g, h);
    uint64_t id = 0;
    if (g_abi_hidden == 1 && b) id = *(uint64_t *)b;
    else if (g_abi_hidden == 0) id = r;
    if (id && id != g_self && !g_partner) {
        EnterCriticalSection(&g_net_cs);
        set_partner(id, "same lobby");
        LeaveCriticalSection(&g_net_cs);
    }
    return r;
}

static void hook_networking(void *net)
{
    static int done; if (done) return; done = 1;
    g_net = net;
    rtp_init(&g_rtp, rtp_tx_cb, rtp_rx_cb, NULL);
    srand(GetTickCount() ^ GetCurrentProcessId());
    void **v = *(void ***)net;
    for (int i = 0; i < 22; ++i) patch(v, i, NET_TR[i], &g_net_orig[i]);
    void *dummy;
    patch(v, NET_Send,   (void *)h_SendP2P,   &dummy);
    patch(v, NET_Avail,  (void *)h_AvailP2P,  &dummy);
    patch(v, NET_Read,   (void *)h_ReadP2P,   &dummy);
    patch(v, NET_Accept, (void *)h_AcceptP2P, &dummy);
    patch(v, NET_Close,  (void *)h_CloseP2P,  &dummy);
    logf_("ISteamNetworking hooked (permanent partner link on channel %d, game packets on own transport, channel %d)", LINK_CH, DATA_CH);
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
        g_client = r;
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
