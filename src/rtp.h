// rtp.h - tiny reliable, ordered transport over an unreliable datagram channel.
//
// Why: Steam's own "reliable" P2P stream got stuck between Brazil and Italy after
// a game session ended on one side while the other was still sending (only ~7
// packets got through afterwards). Unreliable Steam packets kept working, so the
// mod carries the game's packets itself: unreliable datagrams + our own sequence
// numbers, ACKs (cumulative + 64-bit selective) and retransmissions.
//
// Every game session uses a fresh random "generation" (gen). Frames from a gen
// the receiver has declared dead are dropped, so leftovers of an old session can
// never jam the next one.
//
// Single peer, not thread safe (the caller holds a lock). Header-only so the
// test program can include it too.

#ifndef JC_RTP_H
#define JC_RTP_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RTP_SWIN     1024   // send buffer (packets)
#define RTP_RWIN     1024   // receive reorder buffer (packets)
#define RTP_INFLIGHT 256    // max unacked packets on the wire
#define RTP_MAXPAY   1150   // payload limit (unreliable Steam packets max 1200 bytes)
#define RTP_RETX_BUDGET 64  // max retransmissions per tick

#pragma pack(push, 1)
typedef struct {
    uint8_t  m0, m1;    // 'J' 'R'
    uint8_t  type;      // 'D' data, 'A' ack only
    uint8_t  ch;        // game channel of the payload
    uint32_t gen;       // sender's send generation (data frames)
    uint32_t seq;       // sequence number in gen (data frames)
    uint32_t ack_gen;   // receiver side: gen being acknowledged
    uint32_t ack;       // all seq < ack received
    uint64_t sack;      // bit i: seq ack+1+i received
} RtpHdr;
#pragma pack(pop)

typedef struct { uint8_t *d; uint16_t n; uint8_t ch, used, acked; uint32_t sent_at; } RtpS;
typedef struct { uint8_t *d; uint16_t n; uint8_t ch, used; } RtpR;

typedef void (*rtp_tx_fn)(void *ctx, const void *frame, uint32_t n);
typedef void (*rtp_rx_fn)(void *ctx, int ch, const void *data, uint32_t n);

typedef struct {
    uint32_t s_gen, s_next, s_una;
    RtpS     sb[RTP_SWIN];
    uint32_t r_gen, r_next, dead_gen;
    RtpR     rb[RTP_RWIN];
    int      ack_pending;
    uint32_t last_ack_tx;
    uint32_t rto;          // retransmission timeout (ms)
    uint32_t last_ack_rx;  // last time the peer acknowledged our current stream
    int      switched;     // partner switched to a new gen while we had one
    int      adopted;      // a new incoming gen was adopted
    uint32_t st_retx, st_stale, st_full, st_dup;
    rtp_tx_fn tx; rtp_rx_fn rx; void *ctx;
} Rtp;

static void rtp_init(Rtp *r, rtp_tx_fn tx, rtp_rx_fn rx, void *ctx)
{
    memset(r, 0, sizeof *r);
    r->tx = tx; r->rx = rx; r->ctx = ctx;
    r->rto = 400;
}

static void rtp_send_reset_at(Rtp *r, uint32_t gen, uint32_t now)
{
    for (int i = 0; i < RTP_SWIN; ++i) { free(r->sb[i].d); memset(&r->sb[i], 0, sizeof r->sb[i]); }
    r->s_gen = gen; r->s_next = r->s_una = 0;
    r->last_ack_rx = now;
}

static void rtp_send_reset(Rtp *r, uint32_t gen)
{
    for (int i = 0; i < RTP_SWIN; ++i) { free(r->sb[i].d); memset(&r->sb[i], 0, sizeof r->sb[i]); }
    r->s_gen = gen; r->s_next = r->s_una = 0;
}

static void rtp_recv_kill(Rtp *r)
{
    if (r->r_gen) r->dead_gen = r->r_gen;
    for (int i = 0; i < RTP_RWIN; ++i) { free(r->rb[i].d); memset(&r->rb[i], 0, sizeof r->rb[i]); }
    r->r_gen = 0; r->r_next = 0;
}

static void rtp_fill_ack(Rtp *r, RtpHdr *h, uint32_t now)
{
    h->ack_gen = r->r_gen;
    h->ack = r->r_next;
    uint64_t s = 0;
    for (int i = 0; i < 64; ++i) {
        RtpR *b = &r->rb[(r->r_next + 1 + (uint32_t)i) % RTP_RWIN];
        if (r->r_gen && b->used) s |= (uint64_t)1 << i;
    }
    h->sack = s;
    r->ack_pending = 0;
    r->last_ack_tx = now;
}

static void rtp_tx_slot(Rtp *r, uint32_t seq, uint32_t now)
{
    RtpS *s = &r->sb[seq % RTP_SWIN];
    uint8_t frame[sizeof(RtpHdr) + RTP_MAXPAY];
    RtpHdr *h = (RtpHdr *)frame;
    h->m0 = 'J'; h->m1 = 'R'; h->type = 'D'; h->ch = s->ch;
    h->gen = r->s_gen; h->seq = seq;
    rtp_fill_ack(r, h, now);
    memcpy(frame + sizeof(RtpHdr), s->d, s->n);
    r->tx(r->ctx, frame, (uint32_t)(sizeof(RtpHdr) + s->n));
    s->sent_at = now | 1;
}

// 0 ok, -1 no session, -2 too big, -3 buffer full
static int rtp_send(Rtp *r, int ch, const void *data, uint32_t n, uint32_t now)
{
    if (!r->s_gen) return -1;
    if (n > RTP_MAXPAY) return -2;
    if (r->s_next - r->s_una >= RTP_SWIN) { ++r->st_full; return -3; }
    uint32_t seq = r->s_next++;
    RtpS *s = &r->sb[seq % RTP_SWIN];
    free(s->d);
    s->d = (uint8_t *)malloc(n ? n : 1);
    if (!s->d) { --r->s_next; return -3; }
    memcpy(s->d, data, n);
    s->n = (uint16_t)n; s->ch = (uint8_t)ch; s->used = 1; s->acked = 0; s->sent_at = 0;
    if (seq - r->s_una < RTP_INFLIGHT) rtp_tx_slot(r, seq, now);
    return 0;
}

static void rtp_ack_slot(Rtp *r, uint32_t seq)
{
    RtpS *s = &r->sb[seq % RTP_SWIN];
    if (!s->used || s->acked) return;
    s->acked = 1;
    free(s->d); s->d = NULL;
}

static void rtp_advance_una(Rtp *r)
{
    while (r->s_una != r->s_next) {
        RtpS *s = &r->sb[r->s_una % RTP_SWIN];
        if (!s->acked) break;
        memset(s, 0, sizeof *s);
        ++r->s_una;
    }
}

static void rtp_on_frame(Rtp *r, const void *frame, uint32_t n, uint32_t now)
{
    if (n < sizeof(RtpHdr)) return;
    const RtpHdr *h = (const RtpHdr *)frame;
    if (h->m0 != 'J' || h->m1 != 'R') return;

    // acknowledgements for our send stream
    if (r->s_gen && h->ack_gen == r->s_gen) {
        r->last_ack_rx = now;
        uint32_t ack = h->ack;
        if (ack - r->s_una <= r->s_next - r->s_una) {           // within [una, next]
            for (uint32_t q = r->s_una; q != ack; ++q) rtp_ack_slot(r, q);
        }
        for (int i = 0; i < 64; ++i) {
            if (!(h->sack >> i & 1)) continue;
            uint32_t q = ack + 1 + (uint32_t)i;
            if (q - r->s_una < r->s_next - r->s_una) rtp_ack_slot(r, q);
        }
        rtp_advance_una(r);
        // fast retransmit: the peer reports later packets but not the head
        if (h->sack && r->s_una != r->s_next) {
            RtpS *hd = &r->sb[r->s_una % RTP_SWIN];
            if (hd->used && !hd->acked && hd->sent_at && now - hd->sent_at >= r->rto / 3)
                hd->sent_at = 0;
        }
    }

    if (h->type != 'D') return;
    uint32_t pay = n - (uint32_t)sizeof(RtpHdr);
    if (!h->gen || h->gen == r->dead_gen) { ++r->st_stale; return; }
    if (h->gen != r->r_gen) {
        if (r->r_gen) r->switched = 1;
        for (int i = 0; i < RTP_RWIN; ++i) { free(r->rb[i].d); memset(&r->rb[i], 0, sizeof r->rb[i]); }
        r->r_gen = h->gen; r->r_next = 0;
        r->adopted = 1;
    }
    r->ack_pending = 1;
    uint32_t seq = h->seq;
    if (seq - r->r_next >= 0x80000000u) { ++r->st_dup; return; }   // seq < r_next: duplicate
    if (seq - r->r_next >= RTP_RWIN) return;                       // too far ahead
    RtpR *b = &r->rb[seq % RTP_RWIN];
    if (b->used) { ++r->st_dup; return; }
    b->d = (uint8_t *)malloc(pay ? pay : 1);
    if (!b->d) return;
    memcpy(b->d, (const uint8_t *)frame + sizeof(RtpHdr), pay);
    b->n = (uint16_t)pay; b->ch = h->ch; b->used = 1;
    for (;;) {
        RtpR *x = &r->rb[r->r_next % RTP_RWIN];
        if (!x->used) break;
        r->rx(r->ctx, x->ch, x->d, x->n);
        free(x->d);
        memset(x, 0, sizeof *x);
        ++r->r_next;
    }
}

static void rtp_tick(Rtp *r, uint32_t now)
{
    int budget = RTP_RETX_BUDGET;
    // peer silent for 3 s (its game closed, or the network is down): only probe
    // with the oldest packet instead of flooding the link with retransmissions
    uint32_t span = (r->s_una != r->s_next && now - r->last_ack_rx > 3000) ? 1 : RTP_INFLIGHT;
    for (uint32_t q = r->s_una; q != r->s_next && q - r->s_una < span && budget > 0; ++q) {
        RtpS *s = &r->sb[q % RTP_SWIN];
        if (!s->used || s->acked) continue;
        if (!s->sent_at || now - s->sent_at >= r->rto) {
            if (s->sent_at) ++r->st_retx;
            rtp_tx_slot(r, q, now);
            --budget;
        }
    }
    if (r->ack_pending && now - r->last_ack_tx >= 15) {
        RtpHdr h;
        memset(&h, 0, sizeof h);
        h.m0 = 'J'; h.m1 = 'R'; h.type = 'A';
        rtp_fill_ack(r, &h, now);
        r->tx(r->ctx, &h, sizeof h);
    }
}

#endif
