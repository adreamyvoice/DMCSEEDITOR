#include "bc7.h"
#include "bc7_tables.h"   // kPart2[64][16], kPart3[64][16] (verified vs Pillow)
#include <cstring>

namespace bc7 {

// Per-mode parameters (BC7 spec, modes 0..7). Index by mode.
static const int NS [8] = {3,2,3,2,1,1,1,2};   // number of subsets
static const int PB [8] = {4,6,6,6,0,0,0,6};   // partition bits
static const int RB [8] = {0,0,0,0,2,2,0,0};   // rotation bits
static const int ISB[8] = {0,0,0,0,1,0,0,0};   // index-selection bit
static const int CB [8] = {4,6,5,7,5,7,7,5};   // colour bits per component
static const int AB [8] = {0,0,0,0,6,8,7,5};   // alpha bits (0 = opaque)
static const int EPB[8] = {1,0,0,1,0,0,1,1};   // per-endpoint p-bits
static const int SPB[8] = {0,1,0,0,0,0,0,0};   // per-subset shared p-bits
static const int IB [8] = {3,3,2,2,2,2,4,2};   // primary index bits
static const int IB2[8] = {0,0,0,0,3,2,0,0};   // secondary index bits

// Interpolation weights (numerator over 64).
static const int W2[4]  = {0,21,43,64};
static const int W3[8]  = {0,9,18,27,37,46,55,64};
static const int W4[16] = {0,4,9,13,17,21,26,30,34,38,43,47,51,55,60,64};
static const int* Wt(int bits){ return bits==2?W2:bits==3?W3:W4; }

// Fixup (anchor) index tables: the pixel in each non-first subset whose index high bit is
// implicit 0. (Canonical BC7 tables.)
static const int kAnchor2[64] = {
   15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
   15, 2, 8, 2, 2, 8, 8,15, 2, 8, 2, 2, 8, 8, 2, 2,
   15,15, 6, 8, 2, 8,15,15, 2, 8, 2, 2, 2,15,15, 6,
    6, 2, 6, 8,15,15, 2, 2,15,15,15,15,15, 2, 2,15 };
static const int kAnchor3a[64] = {
    3, 3,15,15, 8, 3,15,15, 8, 8, 6, 6, 6, 5, 3, 3,
    3, 3, 8,15, 3, 3, 6,10, 5, 8, 8, 6, 8, 5,15,15,
    8,15, 3, 5, 6,10, 8,15,15, 3,15, 5,15,15,15,15,
    3,15, 5, 5, 5, 8, 5,10, 5,10, 8,13,15,12, 3, 3 };
static const int kAnchor3b[64] = {
   15, 8, 8, 3,15,15, 3, 8,15,15,15,15,15,15,15, 8,
   15, 8,15, 3,15, 8,15, 8, 3,15, 6,10,15,15,10, 8,
   15, 3,15,10,10, 8, 9,10, 6,15, 8,15, 3, 6, 6, 8,
   15, 3,15,15,15,15,15,15,15,15,15,15, 3,15,15, 8 };

struct BitReader {
    const uint8_t* d; int pos = 0;
    explicit BitReader(const uint8_t* p) : d(p) {}
    uint32_t get(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) { v |= ((uint32_t)((d[pos>>3] >> (pos&7)) & 1)) << i; ++pos; }
        return v;
    }
};

static inline uint8_t expand(uint32_t v, int bits) {            // replicate high bits to 8
    v <<= (8 - bits);
    return (uint8_t)(v | (v >> bits));
}
static inline int interp(int a, int b, int w) { return (a*(64-w) + b*w + 32) >> 6; }

static int subsetOf(int mode, int part, int px) {
    if (NS[mode] == 1) return 0;
    return NS[mode] == 2 ? kPart2[part][px] : kPart3[part][px];
}
static bool isAnchor(int mode, int part, int px, int subset) {
    if (subset == 0) return px == 0;                            // subset 0 anchor is always pixel 0
    if (NS[mode] == 2) return px == kAnchor2[part];
    return px == (subset == 1 ? kAnchor3a[part] : kAnchor3b[part]);
}

void DecodeBlock(const uint8_t in[16], uint8_t out[16][4]) {
    // mode = number of leading zero bits in the first set bit
    int mode = -1;
    for (int i = 0; i < 8; ++i) if (in[0] & (1 << i)) { mode = i; break; }
    if (mode < 0) { for (int i=0;i<16;++i){ out[i][0]=out[i][1]=out[i][2]=0; out[i][3]=255; } return; }

    BitReader br(in);
    br.get(mode + 1);                                           // consume mode marker
    int ns = NS[mode];
    int part = PB[mode]  ? (int)br.get(PB[mode])  : 0;
    int rot  = RB[mode]  ? (int)br.get(RB[mode])  : 0;
    int isel = ISB[mode] ? (int)br.get(ISB[mode]) : 0;

    int ne = ns * 2;                                            // endpoints
    int ep[6][4];                                               // [endpoint][RGBA], up to 6
    for (int c = 0; c < 3; ++c) for (int e = 0; e < ne; ++e) ep[e][c] = (int)br.get(CB[mode]);
    if (AB[mode]) for (int e = 0; e < ne; ++e) ep[e][3] = (int)br.get(AB[mode]);
    else          for (int e = 0; e < ne; ++e) ep[e][3] = 255;

    int pbit[6] = {0,0,0,0,0,0};
    if (EPB[mode]) for (int e = 0; e < ne; ++e) pbit[e] = (int)br.get(1);
    if (SPB[mode]) { int sp[3]; for (int s=0;s<ns;++s) sp[s]=(int)br.get(1);
                     for (int e=0;e<ne;++e) pbit[e]=sp[e>>1]; }

    // assemble endpoints to 8-bit
    int cbits = CB[mode] + ((EPB[mode]||SPB[mode]) ? 1 : 0);
    int abits = AB[mode] ? AB[mode] + ((EPB[mode]||SPB[mode]) ? 1 : 0) : 0;
    for (int e = 0; e < ne; ++e) {
        for (int c = 0; c < 3; ++c) {
            uint32_t v = (uint32_t)ep[e][c];
            if (EPB[mode]||SPB[mode]) v = (v << 1) | pbit[e];
            ep[e][c] = expand(v, cbits);
        }
        if (AB[mode]) {
            uint32_t v = (uint32_t)ep[e][3];
            if (EPB[mode]||SPB[mode]) v = (v << 1) | pbit[e];
            ep[e][3] = expand(v, abits);
        } // else already 255
    }

    // index bits: read primary then (if any) secondary, honouring anchor short-reads
    int idx1[16], idx2[16];
    for (int p = 0; p < 16; ++p) {
        int s = subsetOf(mode, part, p);
        int bits = IB[mode] - (isAnchor(mode, part, p, s) ? 1 : 0);
        idx1[p] = (int)br.get(bits);
    }
    if (IB2[mode]) for (int p = 0; p < 16; ++p) {
        int s = subsetOf(mode, part, p);
        int bits = IB2[mode] - (isAnchor(mode, part, p, s) ? 1 : 0);
        idx2[p] = (int)br.get(bits);
    }

    for (int p = 0; p < 16; ++p) {
        int s = subsetOf(mode, part, p);
        int e0 = s*2, e1 = s*2 + 1;
        int cIdx, aIdx, cBitsN, aBitsN;
        if (IB2[mode] == 0) { cIdx = aIdx = idx1[p]; cBitsN = aBitsN = IB[mode]; }
        else if (isel == 0) { cIdx = idx1[p]; cBitsN = IB[mode];  aIdx = idx2[p]; aBitsN = IB2[mode]; }
        else                { cIdx = idx2[p]; cBitsN = IB2[mode]; aIdx = idx1[p]; aBitsN = IB[mode]; }
        int cw = Wt(cBitsN)[cIdx], aw = Wt(aBitsN)[aIdx];
        int r = interp(ep[e0][0], ep[e1][0], cw);
        int g = interp(ep[e0][1], ep[e1][1], cw);
        int b = interp(ep[e0][2], ep[e1][2], cw);
        int a = interp(ep[e0][3], ep[e1][3], aw);
        if (rot == 1) { int t=a;a=r;r=t; }                      // swap A<->R
        else if (rot == 2) { int t=a;a=g;g=t; }                 // swap A<->G
        else if (rot == 3) { int t=a;a=b;b=t; }                 // swap A<->B
        out[p][0]=(uint8_t)r; out[p][1]=(uint8_t)g; out[p][2]=(uint8_t)b; out[p][3]=(uint8_t)a;
    }
}

// ---- Mode-6 encoder: 1 subset, RGBA 7-bit endpoints + 1 p-bit each, 4-bit indices ----
struct BitWriter {
    uint8_t* d; int pos = 0;
    explicit BitWriter(uint8_t* p) : d(p) { memset(p, 0, 16); }
    void put(uint32_t v, int n) { for (int i=0;i<n;++i){ if((v>>i)&1) d[pos>>3]|=(1<<(pos&7)); ++pos; } }
};

void EncodeBlockMode6(const uint8_t in[16][4], uint8_t out[16]) {
    // bounding-box endpoints across the 16 pixels (per channel min/max)
    int lo[4] = {255,255,255,255}, hi[4] = {0,0,0,0};
    for (int p = 0; p < 16; ++p) for (int c = 0; c < 4; ++c) {
        int v = in[p][c]; if (v < lo[c]) lo[c] = v; if (v > hi[c]) hi[c] = v;
    }
    // 7-bit endpoint + p-bit: value contributing to decode is (q<<1)|pbit expanded from 8 bits.
    // Choose p-bit per endpoint = LSB of the 8-bit target so the reconstruction is closest.
    int q0[4], q1[4], pb0 = 0, pb1 = 0;
    // pick p-bits from the average LSBs (cheap, fine for recolour)
    pb0 = (lo[0]&1); pb1 = (hi[0]&1);
    for (int c = 0; c < 4; ++c) {
        q0[c] = (lo[c] >> 1);                                   // top 7 bits
        q1[c] = (hi[c] >> 1);
    }
    // reconstruct the 8-bit endpoints the decoder will see (to compute indices the same way)
    auto rec = [](int q, int pb){ uint32_t v=((uint32_t)q<<1)|pb; v<<=0; /*8 bits already*/ return (int)v; };
    int E0[4], E1[4];
    for (int c=0;c<4;++c){ E0[c]=rec(q0[c],pb0); E1[c]=rec(q1[c],pb1); }

    // choose 4-bit index per pixel minimising squared error along the endpoint line
    int idx[16];
    for (int p = 0; p < 16; ++p) {
        int best = 0; long bestErr = -1;
        for (int w = 0; w < 16; ++w) {
            int ww = W4[w]; long err = 0;
            for (int c = 0; c < 4; ++c) { int v = interp(E0[c], E1[c], ww); int dlt = v - in[p][c]; err += (long)dlt*dlt; }
            if (bestErr < 0 || err < bestErr) { bestErr = err; best = w; }
        }
        idx[p] = best;
    }
    // anchor (pixel 0) must have high bit 0 → if not, swap endpoints and invert indices
    if (idx[0] & 8) {
        for (int c=0;c<4;++c){ int t=q0[c];q0[c]=q1[c];q1[c]=t; }
        int t=pb0;pb0=pb1;pb1=t;
        for (int p=0;p<16;++p) idx[p] = 15 - idx[p];
    }

    BitWriter bw(out);
    bw.put(1u << 6, 7);                                         // mode 6 marker (bit 6 set)
    for (int c = 0; c < 4; ++c) { bw.put((uint32_t)q0[c], 7); bw.put((uint32_t)q1[c], 7); }
    bw.put((uint32_t)pb0, 1); bw.put((uint32_t)pb1, 1);
    for (int p = 0; p < 16; ++p) bw.put((uint32_t)idx[p], p == 0 ? 3 : 4);   // anchor pixel 0 = 3 bits
}

} // namespace bc7
