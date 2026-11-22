// Simulated lossy network test for src/rtp.h
// build: zig cc -O2 -o rtp_test.exe test/rtp_test.c && ./rtp_test.exe
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../src/rtp.h"

#define MAXQ 200000
typedef struct { int to; uint32_t at; uint32_t n; uint8_t d[1300]; } Pkt;
static Pkt *net; static int netn;
static uint32_t now_ms;
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static int loss_pct = 25, dup_pct = 5;

typedef struct { Rtp r; int id; uint32_t expect; uint32_t got; int bad; } Ep;
static Ep A, B;

static void push(int to, const void *f, uint32_t n)
{
    if ((int)(rnd() % 100) < loss_pct) return;
    int copies = (int)(rnd() % 100) < dup_pct ? 2 : 1;
    for (int c = 0; c < copies && netn < MAXQ; ++c) {
        Pkt *p = &net[netn++];
        p->to = to; p->at = now_ms + 50 + rnd() % 250; p->n = n; memcpy(p->d, f, n);
    }
}
static void txA(void *ctx, const void *f, uint32_t n) { (void)ctx; push(1, f, n); }
static void txB(void *ctx, const void *f, uint32_t n) { (void)ctx; push(0, f, n); }
static void rx(void *ctx, int ch, const void *d, uint32_t n)
{
    Ep *e = (Ep *)ctx;
    uint32_t idx; memcpy(&idx, d, 4);
    if (ch != 0 || idx != e->expect || n < 4) { if (e->bad < 5) printf("  ep%d: got %u expected %u\n", e->id, idx, e->expect); e->bad++; }
    for (uint32_t i = 4; i < n; ++i) if (((const uint8_t *)d)[i] != (uint8_t)(idx + i)) { e->bad++; break; }
    e->expect = idx + 1; e->got++;
}
static void deliver(void)
{
    for (int i = 0; i < netn; ) {
        if (net[i].at <= now_ms) {
            Ep *e = net[i].to == 0 ? &A : &B;
            rtp_on_frame(&e->r, net[i].d, net[i].n, now_ms);
            net[i] = net[--netn];
        } else ++i;
    }
}
static void send_msg(Ep *e, uint32_t idx)
{
    uint8_t buf[400]; uint32_t n = 4 + rnd() % 300;
    memcpy(buf, &idx, 4);
    for (uint32_t i = 4; i < n; ++i) buf[i] = (uint8_t)(idx + i);
    int rc = rtp_send(&e->r, 0, buf, n, now_ms);
    if (rc) { printf("  ep%d send rc %d at msg %u\n", e->id, rc, idx); exit(1); }
}

static int run(const char *name, uint32_t msgsA, uint32_t msgsB, uint32_t genA, uint32_t genB)
{
    rtp_send_reset_at(&A.r, genA, now_ms); rtp_send_reset_at(&B.r, genB, now_ms);
    A.expect = B.expect = 0; A.got = B.got = 0; A.bad = B.bad = 0;
    uint32_t sa = 0, sb = 0, t0 = now_ms;
    while ((B.got < msgsA || A.got < msgsB) && now_ms - t0 < 600000) {
        if (sa < msgsA && (now_ms % 13) == 0) send_msg(&A, sa++);   // ~75/s
        if (sb < msgsB && (now_ms % 38) == 0) send_msg(&B, sb++);   // ~26/s
        deliver();
        rtp_tick(&A.r, now_ms); rtp_tick(&B.r, now_ms);
        ++now_ms;
    }
    int ok = B.got == msgsA && A.got == msgsB && !A.bad && !B.bad;
    printf("%-34s %s  B got %u/%u, A got %u/%u, %u s sim, retx A %u B %u, dup %u/%u\n", name, ok ? "PASS" : "FAIL",
           B.got, msgsA, A.got, msgsB, (now_ms - t0) / 1000, A.r.st_retx, B.r.st_retx, A.r.st_dup, B.r.st_dup);
    return ok;
}

int main(void)
{
    net = (Pkt *)malloc(sizeof(Pkt) * MAXQ);
    rtp_init(&A.r, txA, rx, &A); A.id = 0;
    rtp_init(&B.r, txB, rx, &B); B.id = 1;
    int ok = 1;
    ok &= run("1. 25% loss, 5% dup, reorder", 20000, 7000, 111, 222);

    // 2. A starts a new session (new gen): B must adopt it and restart at seq 0
    ok &= run("2. new generation, same peers", 5000, 2000, 333, 444);

    // 3. B's game closes: kill A's stream; A keeps sending old gen -> dropped as stale
    rtp_recv_kill(&B.r);
    uint32_t stale0 = B.r.st_stale;
    for (int i = 0; i < 50; ++i) { send_msg(&A, 900000 + (uint32_t)i); }
    for (int k = 0; k < 2000; ++k) { deliver(); rtp_tick(&A.r, now_ms); rtp_tick(&B.r, now_ms); ++now_ms; }
    int st = B.r.st_stale > stale0 && B.got == 5000;
    printf("%-34s %s  stale frames dropped: %u, nothing delivered\n", "3. dead generation dropped", st ? "PASS" : "FAIL", B.r.st_stale - stale0);
    ok &= st;

    // 4. after that, a fresh session works again
    ok &= run("4. fresh session after dead gen", 3000, 1000, 555, 666);

    // 5. realistic bad network, then extreme loss at a lower volume
    loss_pct = 10;
    ok &= run("5. 10% loss", 20000, 7000, 777, 888);
    loss_pct = 50;
    ok &= run("6. 50% loss (short burst)", 600, 200, 999, 1001);

    printf(ok ? "ALL PASS\n" : "SOME FAILED\n");
    return ok ? 0 : 1;
}
