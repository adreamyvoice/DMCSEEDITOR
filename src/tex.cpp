#include "tex.h"
#include "bc7.h"
#include <cstring>
#include <cmath>
#include <algorithm>

namespace tex {

static void recolorRGB(float& r, float& g, float& b, const Recolor& rc);  // defined below

TexInfo Parse(const Bytes& b) {
    TexInfo ti;
    if (b.size() < 0x10 || memcmp(b.data(), "TEX\0", 4) != 0) return ti;
    uint32_t dims; memcpy(&dims, &b[8], 4);
    ti.mipCount = dims & 0x3F;
    ti.width    = (dims >> 6)  & 0x1FFF;
    ti.height   = (dims >> 19) & 0x1FFF;
    ti.format   = b[0x0D];
    if (ti.width <= 0 || ti.height <= 0 || ti.mipCount <= 0) return ti;
    for (int m = 0; m < ti.mipCount; ++m) {
        uint32_t o = 0; size_t p = 0x10 + (size_t)m * 4;
        if (p + 4 <= b.size()) memcpy(&o, &b[p], 4);
        ti.mipOffsets.push_back(o);
    }
    ti.mip0 = ti.mipOffsets.empty() ? 0 : ti.mipOffsets[0];
    ti.ok = true;
    return ti;
}

int PreviewMip(const TexInfo& ti, int minW) {
    int best = 0;
    for (int m = 0; m < ti.mipCount; ++m) {
        int w = ti.width >> m; if (w < 1) w = 1;
        if (w >= minW) best = m; else break;
    }
    return best;
}

static void rgb565(uint16_t c, int& r, int& g, int& b) {
    r = ((c >> 11) & 0x1F) * 255 / 31;
    g = ((c >> 5)  & 0x3F) * 255 / 63;
    b = ( c        & 0x1F) * 255 / 31;
}
static uint16_t to565(int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)(((r * 31 / 255) << 11) | ((g * 63 / 255) << 5) | (b * 31 / 255));
}

// decode one BC1 colour block (8 bytes) into 16 RGBA pixels
static void decodeBC1(const uint8_t* s, uint8_t out[16][4]) {
    uint16_t c0, c1; memcpy(&c0, s, 2); memcpy(&c1, s + 2, 2);
    int r[4], g[4], b[4];
    rgb565(c0, r[0], g[0], b[0]); rgb565(c1, r[1], g[1], b[1]);
    if (c0 > c1) {
        r[2]=(2*r[0]+r[1])/3; g[2]=(2*g[0]+g[1])/3; b[2]=(2*b[0]+b[1])/3;
        r[3]=(r[0]+2*r[1])/3; g[3]=(g[0]+2*g[1])/3; b[3]=(b[0]+2*b[1])/3;
    } else {
        r[2]=(r[0]+r[1])/2; g[2]=(g[0]+g[1])/2; b[2]=(b[0]+b[1])/2;
        r[3]=g[3]=b[3]=0;
    }
    uint32_t idx; memcpy(&idx, s + 4, 4);
    for (int i = 0; i < 16; ++i) {
        int ci = (idx >> (i * 2)) & 3;
        out[i][0]=(uint8_t)r[ci]; out[i][1]=(uint8_t)g[ci]; out[i][2]=(uint8_t)b[ci]; out[i][3]=255;
    }
}

// decode one BC4 (single-channel) block (8 bytes) into 16 values
static void decodeBC4(const uint8_t* s, uint8_t out[16]) {
    int e0 = s[0], e1 = s[1], r[8];
    r[0]=e0; r[1]=e1;
    if (e0 > e1) { for (int i=1;i<=6;++i) r[i+1] = ((7-i)*e0 + i*e1)/7; }
    else { for (int i=1;i<=4;++i) r[i+1] = ((5-i)*e0 + i*e1)/5; r[6]=0; r[7]=255; }
    uint64_t idx = 0; for (int i=0;i<6;++i) idx |= (uint64_t)s[2+i] << (8*i);
    for (int i=0;i<16;++i) out[i] = (uint8_t)r[(idx >> (3*i)) & 7];
}

bool DecodeRGBA(const Bytes& b, const TexInfo& ti, int mip, std::vector<uint8_t>& rgba, int& outW, int& outH) {
    if (!ti.ok || !ti.recolorable()) return false;
    if (mip < 0 || mip >= (int)ti.mipOffsets.size()) mip = 0;
    int w = ti.width >> mip;  if (w < 1) w = 1;
    int h = ti.height >> mip; if (h < 1) h = 1;
    outW = w; outH = h;
    rgba.assign((size_t)w * h * 4, 0);
    size_t off = ti.mipOffsets[mip];

    if (ti.isRGBA32()) {                              // uncompressed B8G8R8A8: linear, BGRA->RGBA
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            size_t s = off + ((size_t)y * w + x) * 4, d = ((size_t)y * w + x) * 4;
            if (s + 4 > b.size()) return true;
            rgba[d]=b[s+2]; rgba[d+1]=b[s+1]; rgba[d+2]=b[s]; rgba[d+3]=b[s+3];
        }
        return true;
    }
    if (ti.isBC7()) {                                 // BC7: 16-byte 4x4 blocks
        for (int by = 0; by < h; by += 4) for (int bx = 0; bx < w; bx += 4) {
            if (off + 16 > b.size()) return true;
            uint8_t px[16][4]; bc7::DecodeBlock(&b[off], px); off += 16;
            for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) {
                int x = bx + i, y = by + j;
                if (x < w && y < h) { size_t d = ((size_t)y * w + x) * 4; memcpy(&rgba[d], px[j*4+i], 4); }
            }
        }
        return true;
    }
    int blockBytes = ti.has16ByteBlocks() ? 16 : 8;   // BC3 = 8B alpha + 8B colour
    int colourSkip = ti.has16ByteBlocks() ? 8 : 0;
    for (int by = 0; by < h; by += 4) {
        for (int bx = 0; bx < w; bx += 4) {
            if (off + blockBytes > b.size()) return true;
            uint8_t px[16][4];
            if (ti.isBC5()) {                       // normal map: R in block 0, G in block 1
                uint8_t rr[16], gg[16];
                decodeBC4(&b[off], rr); decodeBC4(&b[off+8], gg);
                for (int k=0;k<16;++k) { px[k][0]=rr[k]; px[k][1]=gg[k]; px[k][2]=128; px[k][3]=255; }
            } else decodeBC1(&b[off + colourSkip], px);
            for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) {
                int x = bx + i, y = by + j;
                if (x < w && y < h) {
                    size_t d = ((size_t)y * w + x) * 4;
                    const uint8_t* p = px[j * 4 + i];
                    rgba[d]=p[0]; rgba[d+1]=p[1]; rgba[d+2]=p[2]; rgba[d+3]=255;
                }
            }
            off += blockBytes;
        }
    }
    return true;
}

// --- recolor an endpoint colour through HSV + tint ---
static void rgb2hsv(float r,float g,float b,float&h,float&s,float&v){
    float mx=std::max({r,g,b}), mn=std::min({r,g,b}); v=mx; float d=mx-mn;
    s = mx<=0?0:d/mx;
    if(d<1e-6f){h=0;return;}
    if(mx==r) h=60*fmodf((g-b)/d,6.0f);
    else if(mx==g) h=60*((b-r)/d+2);
    else h=60*((r-g)/d+4);
    if(h<0)h+=360;
}
static void hsv2rgb(float h,float s,float v,float&r,float&g,float&b){
    h=fmodf(h,360.0f); if(h<0)h+=360; float c=v*s; float x=c*(1-fabsf(fmodf(h/60,2)-1)); float m=v-c;
    float rr,gg,bb;
    if(h<60){rr=c;gg=x;bb=0;} else if(h<120){rr=x;gg=c;bb=0;} else if(h<180){rr=0;gg=c;bb=x;}
    else if(h<240){rr=0;gg=x;bb=c;} else if(h<300){rr=x;gg=0;bb=c;} else {rr=c;gg=0;bb=x;}
    r=rr+m; g=gg+m; b=bb+m;
}

static uint16_t recolorEndpoint(uint16_t c, const Recolor& rc) {
    int r,g,b; rgb565(c,r,g,b);
    float fr=r/255.0f, fg=g/255.0f, fb=b/255.0f;
    if (rc.colorize) {
        // luminance * target colour, blended by strength
        float lum = 0.299f*fr + 0.587f*fg + 0.114f*fb;
        float cr = lum*rc.colR, cg = lum*rc.colG, cb = lum*rc.colB;
        float s = rc.colStrength;
        fr = fr*(1-s)+cr*s; fg = fg*(1-s)+cg*s; fb = fb*(1-s)+cb*s;
        return to565((int)(fr*255),(int)(fg*255),(int)(fb*255));
    }
    fr*=rc.tintR; fg*=rc.tintG; fb*=rc.tintB;
    float hh,ss,vv; rgb2hsv(fr,fg,fb,hh,ss,vv);
    hh += rc.hue; ss = std::min(1.0f, ss*rc.sat); vv = std::min(1.0f, vv*rc.val);
    hsv2rgb(hh,ss,vv,fr,fg,fb);
    return to565((int)(fr*255),(int)(fg*255),(int)(fb*255));
}

void Apply(Bytes& b, const TexInfo& ti, const Recolor& rc) {
    if (!ti.ok) return;
    if (ti.isRGBA32()) {                              // uncompressed: recolour each pixel (B,G,R,A)
        size_t off = ti.mip0;
        while (off + 4 <= b.size()) {
            float r = b[off+2]/255.0f, g = b[off+1]/255.0f, bl = b[off]/255.0f;
            recolorRGB(r, g, bl, rc);
            auto clamp = [](float f){ int v=(int)(f*255+0.5f); return (uint8_t)(v<0?0:v>255?255:v); };
            b[off+2]=clamp(r); b[off+1]=clamp(g); b[off]=clamp(bl);   // keep alpha b[off+3]
            off += 4;
        }
        return;
    }
    if (ti.isBC7()) {                                 // decode -> recolour pixels -> re-encode mode 6
        size_t off = ti.mip0;
        while (off + 16 <= b.size()) {
            uint8_t px[16][4]; bc7::DecodeBlock(&b[off], px);
            for (int i = 0; i < 16; ++i) {
                float r = px[i][0]/255.0f, g = px[i][1]/255.0f, bl = px[i][2]/255.0f;
                recolorRGB(r, g, bl, rc);
                auto clamp = [](float f){ int v=(int)(f*255+0.5f); return (uint8_t)(v<0?0:v>255?255:v); };
                px[i][0]=clamp(r); px[i][1]=clamp(g); px[i][2]=clamp(bl);  // keep alpha
            }
            bc7::EncodeBlockMode6(px, &b[off]);
            off += 16;
        }
        return;
    }
    if (ti.isBC5()) {  // normal map: apply Brightness to the two BC4 channel endpoints
        size_t off = ti.mip0;
        while (off + 16 <= b.size()) {
            for (int blk = 0; blk < 16; blk += 8) {
                int e0 = (int)(b[off+blk]   * rc.val);
                int e1 = (int)(b[off+blk+1] * rc.val);
                b[off+blk]   = (uint8_t)(e0<0?0:e0>255?255:e0);
                b[off+blk+1] = (uint8_t)(e1<0?0:e1>255?255:e1);
            }
            off += 16;
        }
        return;
    }
    int blockBytes = ti.has16ByteBlocks() ? 16 : 8;
    int colourSkip = ti.has16ByteBlocks() ? 8 : 0;
    // walk every mip's blocks (data runs contiguously from mip0 to end of file)
    size_t off = ti.mip0;
    while (off + blockBytes <= b.size()) {
        size_t c = off + colourSkip;
        uint16_t c0, c1; memcpy(&c0,&b[c],2); memcpy(&c1,&b[c+2],2);
        uint16_t n0 = recolorEndpoint(c0, rc), n1 = recolorEndpoint(c1, rc);
        // Keep the BC1 mode bit (c0>c1 = opaque 4-colour). If recolor flips the order,
        // swap endpoints AND remap indices (each 2-bit index ^1) so the block is preserved.
        if ((c0 > c1) != (n0 > n1)) {
            uint16_t t=n0; n0=n1; n1=t;
            uint32_t idx; memcpy(&idx,&b[c+4],4); idx ^= 0x55555555u; memcpy(&b[c+4],&idx,4);
        }
        memcpy(&b[c],&n0,2); memcpy(&b[c+2],&n1,2);
        off += blockBytes;
    }
}

static void recolorRGB(float& r, float& g, float& b, const Recolor& rc) {
    if (rc.colorize) {
        float lum = 0.299f*r + 0.587f*g + 0.114f*b, s = rc.colStrength;
        r = r*(1-s)+lum*rc.colR*s; g = g*(1-s)+lum*rc.colG*s; b = b*(1-s)+lum*rc.colB*s;
        return;
    }
    r*=rc.tintR; g*=rc.tintG; b*=rc.tintB;
    float h,sv,v; rgb2hsv(r,g,b,h,sv,v);
    h += rc.hue; sv = std::min(1.0f, sv*rc.sat); v = std::min(1.0f, v*rc.val);
    hsv2rgb(h,sv,v,r,g,b);
}

int RecolorEfl(Bytes& b, const Recolor& rc) {
    // SAFE colour detection. A real RGBA channel is EXACTLY 0.0 or a value >= 1/255 (a real
    // 8-bit colour step) and <= 1.0. The EFL offset table holds small ints like 0x130 which,
    // read as floats, are tiny DENORMALS (~1e-43) — they fail the >=1/255 test, so we never
    // overwrite a pointer/offset (that was the page-fault crash). Matrix rows (1,0,0,0) skipped.
    auto valid = [](float f){ return f == 0.0f || (f >= 1.0f/255.0f && f <= 1.002f); };
    int changed = 0;
    size_t o = 0;
    while (o + 16 <= b.size()) {
        float c[4]; memcpy(c, &b[o], 16);
        bool ok = valid(c[0]) && valid(c[1]) && valid(c[2]) && valid(c[3]);
        int ones = 0, zeros = 0;
        for (int k = 0; k < 4; ++k) { if (c[k] == 1.0f) ++ones; if (c[k] == 0.0f) ++zeros; }
        bool matrixRow = (ones == 1 && zeros == 3);          // identity/transform row, not a colour
        bool hasColour = (c[0] + c[1] + c[2]) > 0.02f;
        if (ok && hasColour && !matrixRow) {
            float r=c[0], g=c[1], bl=c[2];
            recolorRGB(r, g, bl, rc);
            c[0]=r; c[1]=g; c[2]=bl;            // keep alpha (c[3])
            memcpy(&b[o], c, 12);
            ++changed; o += 16;
        } else {
            o += 4;
        }
    }
    return changed;
}

// --- DDS round-trip ----------------------------------------------------------
static uint32_t BlockBytes(int format) { return format == 0x13 ? 8u : 16u; } // BC1=8, rest=16
static const char* FourCC(int format) {
    switch (format) { case 0x13: return "DXT1"; case 0x17: return "DXT3";
                      case 0x19: return "DXT5"; case 0x1F: return "ATI2"; default: return nullptr; }
}

bool ToDDS(const Bytes& tex, const TexInfo& ti, Bytes& out) {
    const char* fcc = FourCC(ti.format);
    if (!ti.ok || !fcc || ti.mip0 == 0 || ti.mip0 > tex.size()) return false;
    out.assign(128, 0);
    memcpy(&out[0], "DDS ", 4);
    auto W = [&](size_t off, uint32_t v) { memcpy(&out[off], &v, 4); };
    W(4, 124);                          // dwSize
    W(8, 0x000A1007);                   // CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT|LINEARSIZE
    W(12, (uint32_t)ti.height);
    W(16, (uint32_t)ti.width);
    W(20, ((ti.width+3)/4) * ((ti.height+3)/4) * BlockBytes(ti.format)); // mip0 linear size
    W(28, (uint32_t)ti.mipCount);
    W(76, 32);                          // ddspf.dwSize
    W(80, 0x4);                         // DDPF_FOURCC
    memcpy(&out[84], fcc, 4);
    W(108, 0x401008);                   // DDSCAPS_TEXTURE|COMPLEX|MIPMAP
    out.insert(out.end(), tex.begin() + ti.mip0, tex.end());
    return true;
}

bool FromDDS(const Bytes& dds, const Bytes& origTex, const TexInfo& ti,
             Bytes& out, std::string& err) {
    if (dds.size() < 128 || memcmp(dds.data(), "DDS ", 4) != 0) { err = "not a DDS file"; return false; }
    auto R = [&](size_t off) { uint32_t v; memcpy(&v, &dds[off], 4); return v; };
    uint32_t h = R(12), w = R(16), mips = R(28);
    char fcc[5] = {0}; memcpy(fcc, &dds[84], 4);
    int fmt = !memcmp(fcc,"DXT1",4) ? 0x13 : !memcmp(fcc,"DXT3",4) ? 0x17 :
              !memcmp(fcc,"DXT5",4) ? 0x19 : (!memcmp(fcc,"ATI2",4)||!memcmp(fcc,"BC5U",4)) ? 0x1F : -1;
    if (fmt < 0) { err = "DDS must be BCn (DXT1/DXT3/DXT5/ATI2) - save it compressed, not uncompressed"; return false; }
    if ((int)w != ti.width || (int)h != ti.height) {
        err = "DDS size must match the texture (" + std::to_string(ti.width) + "x" + std::to_string(ti.height) + ")"; return false; }
    if (fmt != ti.format) { err = "DDS uses a different BCn format than the original - keep the same compression"; return false; }
    if (mips == 0) mips = 1;
    size_t ddsPayload = dds.size() - 128;
    // Fast path: same mip count -> splice into a copy of the original, preserving its exact
    // header and mip-offset table (and any alignment) byte-for-byte.
    if ((int)mips == ti.mipCount && ti.mip0 > 0 && origTex.size() > ti.mip0 &&
        ddsPayload == origTex.size() - ti.mip0) {
        out = origTex;
        memcpy(&out[ti.mip0], &dds[128], ddsPayload);
        return true;
    }
    // Rebuild a fresh .tex header for the DDS's mip count, mips laid contiguously.
    uint32_t bb = BlockBytes(fmt);
    uint32_t headerSize = 0x10 + mips * 4;
    out.assign(headerSize, 0);
    memcpy(&out[0], "TEX\0", 4);
    memcpy(&out[4], &origTex[4], 4);                       // keep versionFlags
    uint32_t dims = (mips & 0x3F) | ((w & 0x1FFF) << 6) | ((h & 0x1FFF) << 19);
    memcpy(&out[8], &dims, 4);
    uint32_t fmtWord = 0; memcpy(&fmtWord, &origTex[0x0C], 4);
    ((uint8_t*)&fmtWord)[1] = (uint8_t)fmt;               // format enum lives in byte @0x0D
    memcpy(&out[0x0C], &fmtWord, 4);
    uint32_t off = headerSize;
    for (uint32_t m = 0; m < mips; ++m) {
        memcpy(&out[0x10 + m*4], &off, 4);
        uint32_t mw = w >> m, mh = h >> m; if (mw < 1) mw = 1; if (mh < 1) mh = 1;
        off += ((mw+3)/4) * ((mh+3)/4) * bb;
    }
    out.resize(headerSize + ddsPayload);
    memcpy(&out[headerSize], &dds[128], ddsPayload);
    return true;
}

// ---- block encoders for the paint brush ----------------------------------------
// Encode 16 RGBA pixels to one opaque BC1 (DXT1) colour block (8 bytes).
static void encBC1(const uint8_t px[16][4], uint8_t out[8]) {
    int mn[3]={255,255,255}, mx[3]={0,0,0};
    for (int i=0;i<16;++i) for (int c=0;c<3;++c){ int v=px[i][c]; if(v<mn[c])mn[c]=v; if(v>mx[c])mx[c]=v; }
    uint16_t c0 = to565(mx[0],mx[1],mx[2]), c1 = to565(mn[0],mn[1],mn[2]);
    if (c0 == c1) { memcpy(out,&c0,2); memcpy(out+2,&c1,2); memset(out+4,0,4); return; }
    if (c0 < c1) { uint16_t t=c0;c0=c1;c1=t; }                 // force opaque 4-colour mode (c0>c1)
    int r[4],g[4],b[4]; rgb565(c0,r[0],g[0],b[0]); rgb565(c1,r[1],g[1],b[1]);
    r[2]=(2*r[0]+r[1])/3; g[2]=(2*g[0]+g[1])/3; b[2]=(2*b[0]+b[1])/3;
    r[3]=(r[0]+2*r[1])/3; g[3]=(g[0]+2*g[1])/3; b[3]=(b[0]+2*b[1])/3;
    uint32_t idx=0;
    for (int i=0;i<16;++i){ int best=0; long be=-1;
        for (int j=0;j<4;++j){ long d=(long)(px[i][0]-r[j])*(px[i][0]-r[j])+(long)(px[i][1]-g[j])*(px[i][1]-g[j])+(long)(px[i][2]-b[j])*(px[i][2]-b[j]);
            if(be<0||d<be){be=d;best=j;} }
        idx |= (uint32_t)best << (2*i); }
    memcpy(out,&c0,2); memcpy(out+2,&c1,2); memcpy(out+4,&idx,4);
}
// Encode 16 single-channel values to one BC4 block (8 bytes), 8-level interpolation.
static void encBC4(const uint8_t v[16], uint8_t out[8]) {
    int mn=255,mx=0; for(int i=0;i<16;++i){ if(v[i]<mn)mn=v[i]; if(v[i]>mx)mx=v[i]; }
    int a0=mx,a1=mn; out[0]=(uint8_t)a0; out[1]=(uint8_t)a1;
    int lv[8];
    if (a0==a1){ memset(out+2,0,6); return; }
    lv[0]=a0; lv[1]=a1; for(int i=1;i<=6;++i) lv[i+1]=((7-i)*a0+i*a1)/7;
    uint64_t idx=0;
    for(int i=0;i<16;++i){ int best=0,be=1<<30; for(int j=0;j<8;++j){ int d=v[i]-lv[j]; d=d<0?-d:d; if(d<be){be=d;best=j;} } idx|=(uint64_t)best<<(3*i); }
    for(int i=0;i<6;++i) out[2+i]=(uint8_t)((idx>>(8*i))&0xFF);
}

// sample an RGBA mip buffer with edge clamp
static inline const uint8_t* clampPx(const std::vector<uint8_t>& buf,int w,int h,int x,int y){
    if(x<0)x=0; if(y<0)y=0; if(x>=w)x=w-1; if(y>=h)y=h-1; return &buf[((size_t)y*w+x)*4];
}

bool EncodeFromRGBA(Bytes& b, const TexInfo& ti, const std::vector<uint8_t>& rgba, std::string& err) {
    if (!ti.ok) { err = "bad texture"; return false; }
    if (!(ti.isBC1()||ti.isBC2()||ti.isBC3()||ti.isBC7()||ti.isRGBA32())) {
        err = "painting not supported for this format (normal map / unknown)"; return false; }
    int W = ti.width, H = ti.height;
    if ((int)rgba.size() != W*H*4) { err = "image size mismatch"; return false; }
    int nmip = (int)ti.mipOffsets.size();
    // build the mip chain in RGBA by box-downsampling the painted image
    std::vector<std::vector<uint8_t>> mips; mips.reserve(nmip);
    mips.push_back(rgba);
    for (int m=1;m<nmip;++m){
        int pw=W>>(m-1), ph=H>>(m-1); if(pw<1)pw=1; if(ph<1)ph=1;
        int mw=W>>m, mh=H>>m; if(mw<1)mw=1; if(mh<1)mh=1;
        const auto& p=mips[m-1]; std::vector<uint8_t> cur((size_t)mw*mh*4);
        for(int y=0;y<mh;++y)for(int x=0;x<mw;++x){
            for(int c=0;c<4;++c){ int s=clampPx(p,pw,ph,2*x,2*y)[c]+clampPx(p,pw,ph,2*x+1,2*y)[c]
                                       +clampPx(p,pw,ph,2*x,2*y+1)[c]+clampPx(p,pw,ph,2*x+1,2*y+1)[c];
                cur[((size_t)y*mw+x)*4+c]=(uint8_t)(s/4); } }
        mips.push_back(std::move(cur));
    }
    for (int m=0;m<nmip;++m){
        int mw=W>>m, mh=H>>m; if(mw<1)mw=1; if(mh<1)mh=1;
        size_t off = ti.mipOffsets[m]; const auto& img = mips[m];
        if (ti.isRGBA32()){
            for(int y=0;y<mh;++y)for(int x=0;x<mw;++x){ const uint8_t*p=&img[((size_t)y*mw+x)*4]; size_t d=off+((size_t)y*mw+x)*4;
                if(d+4>b.size())return true; b[d]=p[2]; b[d+1]=p[1]; b[d+2]=p[0]; b[d+3]=p[3]; }   // RGBA->BGRA
            continue;
        }
        // block formats: gather each 4x4 (edge-clamped) and encode
        for(int by=0;by<mh;by+=4)for(int bx=0;bx<mw;bx+=4){
            uint8_t blk[16][4];
            for(int j=0;j<4;++j)for(int i=0;i<4;++i){ const uint8_t*p=clampPx(img,mw,mh,bx+i,by+j); memcpy(blk[j*4+i],p,4); }
            if(off+ (ti.isBC1()?8u:16u) > b.size()) return true;
            if (ti.isBC1()) { encBC1(blk,&b[off]); off+=8; }
            else if (ti.isBC7()) { bc7::EncodeBlockMode6(blk,&b[off]); off+=16; }
            else { // BC2 / BC3 : alpha half + BC1 colour half
                if (ti.isBC2()){ for(int k=0;k<8;++k){ int a0=blk[k*2][3]>>4, a1=blk[k*2+1][3]>>4; b[off+k]=(uint8_t)(a0|(a1<<4)); } }
                else { uint8_t a[16]; for(int k=0;k<16;++k)a[k]=blk[k][3]; encBC4(a,&b[off]); }
                encBC1(blk,&b[off+8]); off+=16;
            }
        }
    }
    return true;
}

TexInfo ParseDDS(const Bytes& b, size_t base) {
    TexInfo ti;
    if (base + 128 > b.size() || memcmp(&b[base], "DDS ", 4) != 0) return ti;
    auto R = [&](size_t o) { uint32_t v = 0; memcpy(&v, &b[base + o], 4); return v; };
    uint32_t h = R(12), w = R(16), mips = R(28);
    char fcc[5] = {0}; memcpy(fcc, &b[base + 84], 4);
    uint32_t pfFlags = R(80);

    int fmt = 0; uint32_t headerSize = 128;          // DDS header; +20 for the DX10 extension
    bool compressed = true;                          // false -> linear RGBA mips
    if (!memcmp(fcc, "DX10", 4)) {                   // extended header: real format is the DXGI enum
        if (base + 148 > b.size()) return ti;
        uint32_t dxgi = R(128);
        headerSize = 148;
        switch (dxgi) {
            case 70: case 71: case 72: fmt = 0x13; break;          // BC1
            case 73: case 74: case 75: fmt = 0x17; break;          // BC2
            case 76: case 77: case 78: fmt = 0x19; break;          // BC3
            case 82: case 83:          fmt = 0x1F; break;          // BC5
            case 97: case 98: case 99: fmt = 0x20; break;          // BC7
            case 28: case 29: case 87: case 88: fmt = 0x21; compressed = false; break; // R8G8B8A8/B8G8R8A8
            default: return ti;
        }
    } else if (!memcmp(fcc,"DXT1",4)) fmt = 0x13;
    else if (!memcmp(fcc,"DXT3",4)) fmt = 0x17;
    else if (!memcmp(fcc,"DXT5",4)) fmt = 0x19;
    else if (!memcmp(fcc,"ATI2",4) || !memcmp(fcc,"BC5U",4)) fmt = 0x1F;
    else if ((pfFlags & 0x40) && R(88) == 32) { fmt = 0x21; compressed = false; }  // uncompressed 32-bit
    if (!fmt || w == 0 || h == 0) return ti;
    if (mips == 0) mips = 1;
    ti.width = (int)w; ti.height = (int)h; ti.mipCount = (int)mips; ti.format = fmt;

    uint32_t bb = (fmt == 0x13) ? 8 : 16;            // bytes per 4x4 block (BC formats)
    uint32_t off = (uint32_t)base + headerSize;
    for (uint32_t m = 0; m < mips; ++m) {
        if (off >= b.size()) { ti.mipCount = (int)m; break; }     // truncated; keep valid mips
        ti.mipOffsets.push_back(off);
        uint32_t mw = w >> m, mh = h >> m; if (mw < 1) mw = 1; if (mh < 1) mh = 1;
        off += compressed ? ((mw + 3) / 4) * ((mh + 3) / 4) * bb : mw * mh * 4;
    }
    if (ti.mipOffsets.empty()) return ti;
    ti.mip0 = ti.mipOffsets[0];
    ti.ok = true;
    return ti;
}

} // namespace tex
