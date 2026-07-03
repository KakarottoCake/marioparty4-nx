// GX -> OpenGL translation layer (compiled with TARGET_PC so the GX immediate
// mode + texture calls become real functions instead of GameCube FIFO writes).
// Scope: enough of GX to draw 2D sprites (sprput.c/sprman.c). 3D comes later.
#ifdef __SWITCH__

#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <math.h>
#include <string.h>
#include "gfx_switch.h"

extern void OSReport(const char* msg, ...);

// ---------------------------------------------------------------------------
// Matrix state + math (Mtx = f32[3][4], Mtx44 = f32[4][4], row-major)
// ---------------------------------------------------------------------------
static float s_proj[4][4];   // current projection (GXSetProjection)
static float s_pos[3][4];    // current position/modelview matrix (GXLoadPosMtxImm)

void PSMTXIdentity(Mtx m) {
    memset(m, 0, sizeof(float) * 12);
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

void PSMTXCopy(const Mtx src, Mtx dst) { memcpy(dst, src, sizeof(float) * 12); }

void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab) {
    Mtx r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            float s = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
            if (j == 3) s += a[i][3];  // b's implicit 4th row is (0,0,0,1)
            r[i][j] = s;
        }
    }
    memcpy(ab, r, sizeof(r));
}

void PSMTXScale(Mtx m, float x, float y, float z) {
    PSMTXIdentity(m);
    m[0][0] = x; m[1][1] = y; m[2][2] = z;
}

void PSMTXTrans(Mtx m, float x, float y, float z) {
    PSMTXIdentity(m);
    m[0][3] = x; m[1][3] = y; m[2][3] = z;
}

void PSMTXRotAxisRad(Mtx m, const Vec* axis, float rad) {
    float x = axis->x, y = axis->y, z = axis->z;
    float len = sqrtf(x * x + y * y + z * z);
    if (len > 0.0f) { x /= len; y /= len; z /= len; }
    float c = cosf(rad), s = sinf(rad), t = 1.0f - c;
    m[0][0] = t*x*x + c;   m[0][1] = t*x*y - s*z; m[0][2] = t*x*z + s*y; m[0][3] = 0;
    m[1][0] = t*x*y + s*z; m[1][1] = t*y*y + c;   m[1][2] = t*y*z - s*x; m[1][3] = 0;
    m[2][0] = t*x*z - s*y; m[2][1] = t*y*z + s*x; m[2][2] = t*z*z + c;   m[2][3] = 0;
}

// Concatenate a translation onto m (result = Trans(x,y,z) * m): just offset col 3.
void mtxTransCat(Mtx m, float x, float y, float z) {
    m[0][3] += x; m[1][3] += y; m[2][3] += z;
}

void C_MTXOrtho(Mtx44 m, float t, float b, float l, float r, float n, float f) {
    memset(m, 0, sizeof(float) * 16);
    m[0][0] = 2.0f / (r - l);   m[0][3] = -(r + l) / (r - l);
    m[1][1] = 2.0f / (t - b);   m[1][3] = -(t + b) / (t - b);
    m[2][2] = -1.0f / (f - n);  m[2][3] = -n / (f - n);
    m[3][3] = 1.0f;
}

void GXSetProjection(const void* mtx, GXProjectionType type) {
    (void)type;
    memcpy(s_proj, mtx, sizeof(float) * 16);
}

void GXLoadPosMtxImm(const void* mtx, u32 id) {
    (void)id;
    memcpy(s_pos, mtx, sizeof(float) * 12);
}

// ---------------------------------------------------------------------------
// Texture state + GameCube texture decoding
// ---------------------------------------------------------------------------
typedef struct { const void* data; int w, h, fmt; } MyTexObj;

// Cache decoded GL textures keyed by source data pointer (data is re-used across
// frames; decoding every frame would be far too slow).
#define TEXCACHE_MAX 512
static struct { const void* key; unsigned int tex; } s_texCache[TEXCACHE_MAX];
static int s_texCacheN = 0;

static unsigned int s_curTex = 0;   // currently bound GL texture (GX_TEXMAP0)
static float s_tint[4] = {1, 1, 1, 1};

static inline void PutRGBA(unsigned char* p, int idx, int r, int g, int b, int a) {
    p[idx*4+0] = (unsigned char)r; p[idx*4+1] = (unsigned char)g;
    p[idx*4+2] = (unsigned char)b; p[idx*4+3] = (unsigned char)a;
}

// Decode a GameCube (tiled, big-endian) texture into a linear RGBA8 buffer.
static void DecodeGCTexture(int fmt, const unsigned char* src, int w, int h,
                            unsigned char* out) {
    // Default: opaque white (so unsupported formats still show a shape).
    for (int i = 0; i < w * h; i++) PutRGBA(out, i, 255, 255, 255, 255);
    if (!src) return;

    if (fmt == GX_TF_RGBA8) {
        // 4x4 tiles, 64 bytes each: 32 bytes AR then 32 bytes GB.
        int o = 0;
        for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 4) {
            for (int k = 0; k < 16; k++) {         // AR plane
                int px = tx + (k % 4), py = ty + (k / 4);
                int a = src[o++], r = src[o++];
                if (px < w && py < h) { out[(py*w+px)*4+3] = a; out[(py*w+px)*4+0] = r; }
            }
            for (int k = 0; k < 16; k++) {         // GB plane
                int px = tx + (k % 4), py = ty + (k / 4);
                int g = src[o++], b = src[o++];
                if (px < w && py < h) { out[(py*w+px)*4+1] = g; out[(py*w+px)*4+2] = b; }
            }
        }
    } else if (fmt == GX_TF_RGB5A3 || fmt == GX_TF_RGB565) {
        int o = 0;
        for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 4)
        for (int k = 0; k < 16; k++) {
            int px = tx + (k % 4), py = ty + (k / 4);
            unsigned v = (src[o] << 8) | src[o + 1];  // big-endian u16
            o += 2;
            int r, g, b, a;
            if (fmt == GX_TF_RGB565) {
                r = ((v >> 11) & 0x1F) * 255 / 31;
                g = ((v >> 5) & 0x3F) * 255 / 63;
                b = (v & 0x1F) * 255 / 31;
                a = 255;
            } else if (v & 0x8000) {                  // RGB555
                r = ((v >> 10) & 0x1F) * 255 / 31;
                g = ((v >> 5) & 0x1F) * 255 / 31;
                b = (v & 0x1F) * 255 / 31;
                a = 255;
            } else {                                  // ARGB3444
                a = ((v >> 12) & 0x7) * 255 / 7;
                r = ((v >> 8) & 0xF) * 255 / 15;
                g = ((v >> 4) & 0xF) * 255 / 15;
                b = (v & 0xF) * 255 / 15;
            }
            if (px < w && py < h) PutRGBA(out, py * w + px, r, g, b, a);
        }
    }
    // Other formats (I4/I8/IA*/C4/C8) fall through as white for now.
}

static unsigned int GetOrCreateTexture(const MyTexObj* obj) {
    if (!obj || !obj->data) return 0;
    for (int i = 0; i < s_texCacheN; i++)
        if (s_texCache[i].key == obj->data) return s_texCache[i].tex;

    int w = obj->w, h = obj->h;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return 0;
    static unsigned char buf[1024 * 1024 * 4];
    DecodeGCTexture(obj->fmt, (const unsigned char*)obj->data, w, h, buf);
    unsigned int tex = GfxCreateTexture(w, h, buf);
    if (s_texCacheN < TEXCACHE_MAX) {
        s_texCache[s_texCacheN].key = obj->data;
        s_texCache[s_texCacheN].tex = tex;
        s_texCacheN++;
    }
    return tex;
}

void GXInitTexObj(GXTexObj* obj, void* image_ptr, u16 width, u16 height,
                  GXTexFmt format, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t, u8 mip) {
    (void)wrap_s; (void)wrap_t; (void)mip;
    MyTexObj* t = (MyTexObj*)obj;
    t->data = image_ptr; t->w = width; t->h = height; t->fmt = (int)format;
}

void GXInitTexObjCI(GXTexObj* obj, void* image_ptr, u16 width, u16 height,
                    GXCITexFmt format, GXTexWrapMode s, GXTexWrapMode t, u8 mip, u32 tlut) {
    (void)format; (void)s; (void)t; (void)mip; (void)tlut;
    MyTexObj* to = (MyTexObj*)obj;
    to->data = image_ptr; to->w = width; to->h = height; to->fmt = -1;  // palette -> white
}

void GXLoadTexObj(GXTexObj* obj, GXTexMapID id) {
    if (id != GX_TEXMAP0) return;      // only map0 feeds our single sampler
    s_curTex = GetOrCreateTexture((const MyTexObj*)obj);
}

// ---------------------------------------------------------------------------
// Immediate-mode vertex capture
// ---------------------------------------------------------------------------
#define MAXV 64
static float s_vClip[MAXV][2];
static float s_vUV[MAXV][2];
static int s_vCount = 0;
static int s_prim = 0;

void GXBegin(GXPrimitive type, GXVtxFmt fmt, u16 nverts) {
    (void)fmt; (void)nverts;
    s_prim = type;
    s_vCount = 0;
}

void GXPosition3f32(f32 x, f32 y, f32 z) {
    if (s_vCount >= MAXV) return;
    // world = posMtx * (x,y,z,1)
    float ox = s_pos[0][0]*x + s_pos[0][1]*y + s_pos[0][2]*z + s_pos[0][3];
    float oy = s_pos[1][0]*x + s_pos[1][1]*y + s_pos[1][2]*z + s_pos[1][3];
    float oz = s_pos[2][0]*x + s_pos[2][1]*y + s_pos[2][2]*z + s_pos[2][3];
    // clip = proj * world
    float cx = s_proj[0][0]*ox + s_proj[0][1]*oy + s_proj[0][2]*oz + s_proj[0][3];
    float cy = s_proj[1][0]*ox + s_proj[1][1]*oy + s_proj[1][2]*oz + s_proj[1][3];
    float cw = s_proj[3][0]*ox + s_proj[3][1]*oy + s_proj[3][2]*oz + s_proj[3][3];
    if (cw == 0.0f) cw = 1.0f;
    s_vClip[s_vCount][0] = cx / cw;
    s_vClip[s_vCount][1] = cy / cw;
    s_vUV[s_vCount][0] = 0.0f;
    s_vUV[s_vCount][1] = 0.0f;
    s_vCount++;
}

void GXTexCoord2f32(f32 s, f32 t) {
    if (s_vCount == 0 || s_vCount > MAXV) return;
    s_vUV[s_vCount - 1][0] = s;
    s_vUV[s_vCount - 1][1] = t;
}

void GXEnd(void) {
    if (s_vCount < 3) { s_vCount = 0; return; }
    // Expand quads (0,1,2,3) into two triangles (0,1,2)(0,2,3).
    float clip[MAXV * 3 * 2];
    float uv[MAXV * 3 * 2];
    int n = 0;
    if (s_prim == GX_QUADS) {
        for (int q = 0; q + 3 < s_vCount; q += 4) {
            int idx[6] = { q, q+1, q+2, q, q+2, q+3 };
            for (int j = 0; j < 6; j++) {
                clip[n*2+0] = s_vClip[idx[j]][0]; clip[n*2+1] = s_vClip[idx[j]][1];
                uv[n*2+0] = s_vUV[idx[j]][0];     uv[n*2+1] = s_vUV[idx[j]][1];
                n++;
            }
        }
    } else {  // treat as triangle strip/fan-ish fallback: fan from vertex 0
        for (int j = 1; j + 1 < s_vCount; j++) {
            int idx[3] = { 0, j, j+1 };
            for (int k = 0; k < 3; k++) {
                clip[n*2+0] = s_vClip[idx[k]][0]; clip[n*2+1] = s_vClip[idx[k]][1];
                uv[n*2+0] = s_vUV[idx[k]][0];     uv[n*2+1] = s_vUV[idx[k]][1];
                n++;
            }
        }
    }
    Gfx2D_DrawTexTris(clip, uv, n, s_curTex, s_tint[0], s_tint[1], s_tint[2], s_tint[3]);
    s_vCount = 0;
}

// Capture the material color as our draw tint (texture is modulated by it).
void GXSetChanMatColor(GXChannelID id, GXColor c) {
    (void)id;
    s_tint[0] = c.r / 255.0f; s_tint[1] = c.g / 255.0f;
    s_tint[2] = c.b / 255.0f; s_tint[3] = c.a / 255.0f;
}
// (All other GX state functions are kept as loose no-ops in sys_switch.c.)

#endif // __SWITCH__
