// GX -> OpenGL translation layer (compiled with TARGET_PC so the GX immediate
// mode + texture calls become real functions instead of GameCube FIFO writes).
// Scope: enough of GX to draw 2D sprites and the current static HSF mesh slice.
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

void mtxRot(Mtx m, float x, float y, float z) {
    Vec axis;
    Mtx turn;
    const float radians = 0.017453292519943295f;
    PSMTXIdentity(m);
    if (x != 0.0f) {
        axis.x = 1.0f; axis.y = 0.0f; axis.z = 0.0f;
        PSMTXRotAxisRad(turn, &axis, x * radians);
        PSMTXConcat(turn, m, m);
    }
    if (y != 0.0f) {
        axis.x = 0.0f; axis.y = 1.0f; axis.z = 0.0f;
        PSMTXRotAxisRad(turn, &axis, y * radians);
        PSMTXConcat(turn, m, m);
    }
    if (z != 0.0f) {
        axis.x = 0.0f; axis.y = 0.0f; axis.z = 1.0f;
        PSMTXRotAxisRad(turn, &axis, z * radians);
        PSMTXConcat(turn, m, m);
    }
}

void mtxRotCat(Mtx m, float x, float y, float z) {
    Mtx turn;
    mtxRot(turn, x, y, z);
    PSMTXConcat(turn, m, m);
}

void mtxScaleCat(Mtx m, float x, float y, float z) {
    m[0][0] *= x; m[1][0] *= x; m[2][0] *= x;
    m[0][1] *= y; m[1][1] *= y; m[2][1] *= y;
    m[0][2] *= z; m[1][2] *= z; m[2][2] *= z;
}

void C_MTXLookAt(Mtx m, const Point3d* camPos, const Vec* camUp,
                 const Point3d* target) {
    float fx = target->x - camPos->x;
    float fy = target->y - camPos->y;
    float fz = target->z - camPos->z;
    float fl = sqrtf(fx * fx + fy * fy + fz * fz);
    float rx, ry, rz;
    float rl;
    float ux, uy, uz;

    if (fl < 0.000001f) {
        fx = 0.0f; fy = 0.0f; fz = -1.0f;
        fl = 1.0f;
    }
    fx /= fl; fy /= fl; fz /= fl;
    rx = camUp->y * fz - camUp->z * fy;
    ry = camUp->z * fx - camUp->x * fz;
    rz = camUp->x * fy - camUp->y * fx;
    rl = sqrtf(rx * rx + ry * ry + rz * rz);
    if (rl < 0.000001f) {
        rx = 1.0f; ry = 0.0f; rz = 0.0f;
        rl = 1.0f;
    }
    rx /= rl; ry /= rl; rz /= rl;
    ux = fy * rz - fz * ry;
    uy = fz * rx - fx * rz;
    uz = fx * ry - fy * rx;
    m[0][0] = rx; m[0][1] = ry; m[0][2] = rz;
    m[1][0] = ux; m[1][1] = uy; m[1][2] = uz;
    m[2][0] = -fx; m[2][1] = -fy; m[2][2] = -fz;
    m[0][3] = -(rx * camPos->x + ry * camPos->y + rz * camPos->z);
    m[1][3] = -(ux * camPos->x + uy * camPos->y + uz * camPos->z);
    m[2][3] = fx * camPos->x + fy * camPos->y + fz * camPos->z;
}

void C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 nearZ, f32 farZ) {
    float cot;
    float radians = fovY * 0.017453292519943295f;
    memset(m, 0, sizeof(Mtx44));
    if (aspect == 0.0f) aspect = 4.0f / 3.0f;
    if (nearZ <= 0.0f) nearZ = 0.1f;
    if (farZ <= nearZ) farZ = nearZ + 1.0f;
    cot = 1.0f / tanf(radians * 0.5f);
    m[0][0] = cot / aspect;
    m[1][1] = cot;
    m[2][2] = farZ / (nearZ - farZ);
    m[2][3] = (nearZ * farZ) / (nearZ - farZ);
    m[3][2] = -1.0f;
}

u32 PSMTXInvXpose(const Mtx src, Mtx dst) {
    float a = src[0][0], b = src[0][1], c = src[0][2];
    float d = src[1][0], e = src[1][1], f = src[1][2];
    float g = src[2][0], h = src[2][1], i = src[2][2];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (fabsf(det) < 0.000001f) {
        PSMTXIdentity(dst);
        return 0;
    }
    {
        float inv = 1.0f / det;
        dst[0][0] = (e * i - f * h) * inv;
        dst[0][1] = (d * h - b * i) * inv;
        dst[0][2] = (b * f - d * e) * inv;
        dst[1][0] = (g * f - d * i) * inv;
        dst[1][1] = (a * i - c * g) * inv;
        dst[1][2] = (c * d - a * f) * inv;
        dst[2][0] = (d * h - e * g) * inv;
        dst[2][1] = (b * g - a * h) * inv;
        dst[2][2] = (a * e - b * d) * inv;
        dst[0][3] = dst[1][3] = dst[2][3] = 0.0f;
    }
    return 1;
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
typedef struct {
    const void* data;
    const void* palette;
    int w;
    int h;
    int fmt;
    int palette_fmt;
    int palette_entries;
    u32 tlut;
    int wrap_s;
    int wrap_t;
} MyTexObj;

typedef struct {
    const void* data;
    int fmt;
    int entries;
} MyTlutObj;

/* GXTexObj/GXTlutObj are opaque GameCube-sized structs.  Keep the larger
 * Switch-side state out of those caller-owned buffers; some callers allocate
 * them as 32-byte/12-byte stack objects. */
#define TEXOBJ_STATE_MAX 1024
#define TLUTOBJ_STATE_MAX 64
static struct {
    const GXTexObj* key;
    MyTexObj state;
} s_texObjStates[TEXOBJ_STATE_MAX];
static int s_texObjStateN = 0;
static struct {
    const GXTlutObj* key;
    MyTlutObj state;
} s_tlutObjStates[TLUTOBJ_STATE_MAX];
static int s_tlutObjStateN = 0;

// Cache decoded GL textures by image and palette.  The same image pointer can
// be used with different TLUT slots by the original renderer.
#define TEXCACHE_MAX 512
static struct {
    const void* key;
    const void* palette;
    int fmt;
    int palette_fmt;
    int wrap_s;
    int wrap_t;
    unsigned int tex;
} s_texCache[TEXCACHE_MAX];
static int s_texCacheN = 0;

#define TLUT_MAX 16
static MyTlutObj s_tluts[TLUT_MAX];

static unsigned int s_curTex = 0;   // currently bound GL texture (GX_TEXMAP0)
static float s_tint[4] = {1, 1, 1, 1};

static MyTexObj* GetTexObjState(const GXTexObj* key) {
    int i;
    if (!key) return NULL;
    for (i = 0; i < s_texObjStateN; i++) {
        if (s_texObjStates[i].key == key) return &s_texObjStates[i].state;
    }
    if (s_texObjStateN >= TEXOBJ_STATE_MAX) return NULL;
    s_texObjStates[s_texObjStateN].key = key;
    memset(&s_texObjStates[s_texObjStateN].state, 0,
           sizeof(s_texObjStates[s_texObjStateN].state));
    return &s_texObjStates[s_texObjStateN++].state;
}

static MyTlutObj* GetTlutObjState(const GXTlutObj* key) {
    int i;
    if (!key) return NULL;
    for (i = 0; i < s_tlutObjStateN; i++) {
        if (s_tlutObjStates[i].key == key) return &s_tlutObjStates[i].state;
    }
    if (s_tlutObjStateN >= TLUTOBJ_STATE_MAX) return NULL;
    s_tlutObjStates[s_tlutObjStateN].key = key;
    memset(&s_tlutObjStates[s_tlutObjStateN].state, 0,
           sizeof(s_tlutObjStates[s_tlutObjStateN].state));
    return &s_tlutObjStates[s_tlutObjStateN++].state;
}

static const MyTlutObj* FindTlutObjState(const GXTlutObj* key) {
    int i;
    if (!key) return NULL;
    for (i = 0; i < s_tlutObjStateN; i++) {
        if (s_tlutObjStates[i].key == key) return &s_tlutObjStates[i].state;
    }
    return NULL;
}

static inline void PutRGBA(unsigned char* p, int idx, int r, int g, int b, int a) {
    p[idx*4+0] = (unsigned char)r; p[idx*4+1] = (unsigned char)g;
    p[idx*4+2] = (unsigned char)b; p[idx*4+3] = (unsigned char)a;
}

static int Expand4(int value) { return value * 17; }

static void DecodeRGB565(unsigned value, int* r, int* g, int* b, int* a) {
    *r = ((value >> 11) & 0x1F) * 255 / 31;
    *g = ((value >> 5) & 0x3F) * 255 / 63;
    *b = (value & 0x1F) * 255 / 31;
    *a = 255;
}

static void DecodeRGB5A3(unsigned value, int* r, int* g, int* b, int* a) {
    if (value & 0x8000) {
        *r = ((value >> 10) & 0x1F) * 255 / 31;
        *g = ((value >> 5) & 0x1F) * 255 / 31;
        *b = (value & 0x1F) * 255 / 31;
        *a = 255;
    } else {
        *a = ((value >> 12) & 0x7) * 255 / 7;
        *r = ((value >> 8) & 0xF) * 255 / 15;
        *g = ((value >> 4) & 0xF) * 255 / 15;
        *b = (value & 0xF) * 255 / 15;
    }
}

static void DecodePaletteColor(const MyTexObj* obj, int index,
                               int* r, int* g, int* b, int* a) {
    const unsigned char* p;
    unsigned value;
    if (!obj->palette || index < 0 || index >= obj->palette_entries) {
        *r = *g = *b = 255;
        *a = 0;
        return;
    }
    p = (const unsigned char*)obj->palette + index * 2;
    value = ((unsigned)p[0] << 8) | p[1];
    switch (obj->palette_fmt) {
        case GX_TL_IA8:
            *r = *g = *b = p[0];
            *a = p[1];
            break;
        case GX_TL_RGB565:
            DecodeRGB565(value, r, g, b, a);
            break;
        case GX_TL_RGB5A3:
        default:
            DecodeRGB5A3(value, r, g, b, a);
            break;
    }
}

// Decode a GameCube (tiled, big-endian) texture into a linear RGBA8 buffer.
static void DecodeGCTexture(int fmt, const unsigned char* src, int w, int h,
                            const MyTexObj* obj, unsigned char* out) {
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
            if (fmt == GX_TF_RGB565) DecodeRGB565(v, &r, &g, &b, &a);
            else DecodeRGB5A3(v, &r, &g, &b, &a);
            if (px < w && py < h) PutRGBA(out, py * w + px, r, g, b, a);
        }
    } else if (fmt == GX_TF_I4 || fmt == GX_TF_C4) {
        // I4/C4: 8x8 tiles, two texels per byte.
        int o = 0;
        for (int ty = 0; ty < h; ty += 8)
        for (int tx = 0; tx < w; tx += 8) {
            for (int row = 0; row < 8; row++) {
                for (int col = 0; col < 4; col++) {
                    unsigned value = src[o++];
                    int indexes[2] = {(int)(value >> 4), (int)(value & 0xF)};
                    for (int n = 0; n < 2; n++) {
                        int px = tx + col * 2 + n;
                        int py = ty + row;
                        int r, g, b, a;
                        if (fmt == GX_TF_C4) {
                            DecodePaletteColor(obj, indexes[n], &r, &g, &b, &a);
                        } else {
                            r = g = b = Expand4(indexes[n]);
                            a = 255;
                        }
                        if (px < w && py < h) PutRGBA(out, py * w + px, r, g, b, a);
                    }
                }
            }
        }
    } else if (fmt == GX_TF_I8 || fmt == GX_TF_IA4 || fmt == GX_TF_A8 || fmt == GX_TF_C8) {
        // I8/IA4/A8/C8: 8x4 tiles, one texel per byte.
        int o = 0;
        for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 8)
        for (int row = 0; row < 4; row++)
        for (int col = 0; col < 8; col++) {
            unsigned value = src[o++];
            int px = tx + col;
            int py = ty + row;
            int r, g, b, a;
            if (fmt == GX_TF_C8) {
                DecodePaletteColor(obj, (int)value, &r, &g, &b, &a);
            } else if (fmt == GX_TF_I8) {
                r = g = b = (int)value;
                a = 255;
            } else if (fmt == GX_TF_IA4) {
                r = g = b = Expand4((int)(value >> 4));
                a = Expand4((int)(value & 0xF));
            } else {
                r = g = b = 255;
                a = (int)value;
            }
            if (px < w && py < h) PutRGBA(out, py * w + px, r, g, b, a);
        }
    } else if (fmt == GX_TF_IA8) {
        // IA8: 4x4 tiles, intensity byte followed by alpha byte.
        int o = 0;
        for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 4)
        for (int k = 0; k < 16; k++) {
            int px = tx + (k % 4), py = ty + (k / 4);
            int intensity = src[o++];
            int alpha = src[o++];
            if (px < w && py < h) PutRGBA(out, py * w + px,
                                           intensity, intensity, intensity, alpha);
        }
    } else if (fmt == GX_TF_CMPR) {
        // CMPR is four DXT1 4x4 blocks inside each 8x8 tile.
        int o = 0;
        for (int ty = 0; ty < h; ty += 8)
        for (int tx = 0; tx < w; tx += 8)
        for (int block = 0; block < 4; block++) {
            int bx = tx + (block & 1) * 4;
            int by = ty + (block >> 1) * 4;
            unsigned c0 = ((unsigned)src[o] << 8) | src[o + 1];
            unsigned c1 = ((unsigned)src[o + 2] << 8) | src[o + 3];
            unsigned bits = ((unsigned)src[o + 4] << 24) |
                            ((unsigned)src[o + 5] << 16) |
                            ((unsigned)src[o + 6] << 8) | src[o + 7];
            int r0, g0, b0, a0, r1, g1, b1, a1;
            int colors[4][4];
            DecodeRGB565(c0, &r0, &g0, &b0, &a0);
            DecodeRGB565(c1, &r1, &g1, &b1, &a1);
            colors[0][0] = r0; colors[0][1] = g0; colors[0][2] = b0; colors[0][3] = 255;
            colors[1][0] = r1; colors[1][1] = g1; colors[1][2] = b1; colors[1][3] = 255;
            if (c0 > c1) {
                colors[2][0] = (2*r0 + r1) / 3; colors[2][1] = (2*g0 + g1) / 3;
                colors[2][2] = (2*b0 + b1) / 3; colors[2][3] = 255;
                colors[3][0] = (r0 + 2*r1) / 3; colors[3][1] = (g0 + 2*g1) / 3;
                colors[3][2] = (b0 + 2*b1) / 3; colors[3][3] = 255;
            } else {
                colors[2][0] = (r0 + r1) / 2; colors[2][1] = (g0 + g1) / 2;
                colors[2][2] = (b0 + b1) / 2; colors[2][3] = 255;
                colors[3][0] = colors[3][1] = colors[3][2] = 0; colors[3][3] = 0;
            }
            o += 8;
            for (int row = 0; row < 4; row++)
            for (int col = 0; col < 4; col++) {
                int index = (bits >> (30 - (row * 4 + col) * 2)) & 3;
                int px = bx + col, py = by + row;
                if (px < w && py < h)
                    PutRGBA(out, py * w + px, colors[index][0], colors[index][1],
                            colors[index][2], colors[index][3]);
            }
        }
    }
}

static unsigned int GetOrCreateTexture(const MyTexObj* obj) {
    if (!obj || !obj->data) return 0;
    for (int i = 0; i < s_texCacheN; i++)
        if (s_texCache[i].key == obj->data &&
            s_texCache[i].palette == obj->palette &&
            s_texCache[i].fmt == obj->fmt &&
            s_texCache[i].palette_fmt == obj->palette_fmt &&
            s_texCache[i].wrap_s == obj->wrap_s &&
            s_texCache[i].wrap_t == obj->wrap_t) {
            return s_texCache[i].tex;
        }

    int w = obj->w, h = obj->h;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return 0;
    static unsigned char buf[1024 * 1024 * 4];
    DecodeGCTexture(obj->fmt, (const unsigned char*)obj->data, w, h, obj, buf);
    unsigned int tex = GfxCreateTexture(w, h, buf,
                                        obj->wrap_s, obj->wrap_t);
    if (s_texCacheN < TEXCACHE_MAX) {
        s_texCache[s_texCacheN].key = obj->data;
        s_texCache[s_texCacheN].palette = obj->palette;
        s_texCache[s_texCacheN].fmt = obj->fmt;
        s_texCache[s_texCacheN].palette_fmt = obj->palette_fmt;
        s_texCache[s_texCacheN].wrap_s = obj->wrap_s;
        s_texCache[s_texCacheN].wrap_t = obj->wrap_t;
        s_texCache[s_texCacheN].tex = tex;
        s_texCacheN++;
    }
    return tex;
}

void GXInitTexObj(GXTexObj* obj, void* image_ptr, u16 width, u16 height,
                  GXTexFmt format, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t, u8 mip) {
    (void)mip;
    MyTexObj* t = GetTexObjState(obj);
    if (!t) return;
    t->data = image_ptr;
    t->palette = NULL;
    t->w = width;
    t->h = height;
    t->fmt = (int)format;
    t->palette_fmt = GX_TL_RGB5A3;
    t->palette_entries = 0;
    t->tlut = TLUT_MAX;
    t->wrap_s = (int)wrap_s;
    t->wrap_t = (int)wrap_t;
}

void GXInitTexObjCI(GXTexObj* obj, void* image_ptr, u16 width, u16 height,
                    GXCITexFmt format, GXTexWrapMode s, GXTexWrapMode t, u8 mip, u32 tlut) {
    (void)mip;
    MyTexObj* to = GetTexObjState(obj);
    if (!to) return;
    to->data = image_ptr;
    to->w = width;
    to->h = height;
    to->fmt = (int)format;
    to->tlut = tlut;
    to->wrap_s = (int)s;
    to->wrap_t = (int)t;
    if (tlut < TLUT_MAX) {
        to->palette = s_tluts[tlut].data;
        to->palette_fmt = s_tluts[tlut].fmt;
        to->palette_entries = s_tluts[tlut].entries;
    } else {
        to->palette = NULL;
        to->palette_fmt = GX_TL_RGB5A3;
        to->palette_entries = 0;
    }
}

void GXInitTlutObj(GXTlutObj* obj, void* data, GXTlutFmt fmt, u16 entries) {
    MyTlutObj* tlut = GetTlutObjState(obj);
    if (!tlut) return;
    tlut->data = data;
    tlut->fmt = (int)fmt;
    tlut->entries = entries;
}

void GXLoadTlut(GXTlutObj* obj, u32 tlut_name) {
    const MyTlutObj* tlut = FindTlutObjState(obj);
    if (!obj || tlut_name >= TLUT_MAX) return;
    if (!tlut) return;
    s_tluts[tlut_name] = *tlut;
}

void GXLoadTexObj(GXTexObj* obj, GXTexMapID id) {
    MyTexObj* state;
    if (id != GX_TEXMAP0) return;      // only map0 feeds our single sampler
    state = GetTexObjState(obj);
    if (!state) return;
    if (state->tlut < TLUT_MAX) {
        state->palette = s_tluts[state->tlut].data;
        state->palette_fmt = s_tluts[state->tlut].fmt;
        state->palette_entries = s_tluts[state->tlut].entries;
    }
    s_curTex = GetOrCreateTexture(state);
}

void GXInvalidateTexAll(void) {
    /* The first HSF renderer is solid-material only.  Clearing this state
     * prevents the last sprite texture from tinting a 3D mesh. */
    s_curTex = 0;
}

// ---------------------------------------------------------------------------
// Immediate-mode vertex capture
// ---------------------------------------------------------------------------
#define MAXV 64
static float s_vClip[MAXV][3];
static float s_vUV[MAXV][2];
static float s_vColor[MAXV][4];
static float s_curVertexColor[4] = {1, 1, 1, 1};
static int s_vCount = 0;
static int s_prim = 0;
static BOOL s_3dMode = FALSE;

void GXSet3DMode(u8 enable) {
    s_3dMode = enable ? TRUE : FALSE;
}

// Translate the small set of GX raster states used by HSF materials into the
// GLES2 backend.  The original GX FIFO is not present on Switch, so these
// calls update the backend state consumed by Gfx3D_Draw*.
void GXSetCullMode(GXCullMode mode) {
    Gfx3D_SetCullMode((int)mode);
}

void GXSetZMode(GXBool compare_enable, GXCompare func, GXBool update_enable) {
    Gfx3D_SetDepthMode(compare_enable ? 1 : 0, (int)func,
                       update_enable ? 1 : 0);
}

void GXSetBlendMode(GXBlendMode type, GXBlendFactor src_factor,
                    GXBlendFactor dst_factor, GXLogicOp op) {
    Gfx3D_SetBlendMode((int)type, (int)src_factor, (int)dst_factor,
                       (int)op);
}

void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op,
                       GXCompare comp1, u8 ref1) {
    Gfx3D_SetAlphaCompare((int)comp0, (int)ref0, (int)op,
                          (int)comp1, (int)ref1);
}

void GXBegin(GXPrimitive type, GXVtxFmt fmt, u16 nverts) {
    (void)fmt; (void)nverts;
    s_prim = type;
    s_vCount = 0;
    s_curVertexColor[0] = 1.0f;
    s_curVertexColor[1] = 1.0f;
    s_curVertexColor[2] = 1.0f;
    s_curVertexColor[3] = 1.0f;
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
    float cz = s_proj[2][0]*ox + s_proj[2][1]*oy + s_proj[2][2]*oz + s_proj[2][3];
    float cw = s_proj[3][0]*ox + s_proj[3][1]*oy + s_proj[3][2]*oz + s_proj[3][3];
    if (cw == 0.0f) cw = 1.0f;
    s_vClip[s_vCount][0] = cx / cw;
    s_vClip[s_vCount][1] = cy / cw;
    s_vClip[s_vCount][2] = cz / cw;
    s_vUV[s_vCount][0] = 0.0f;
    s_vUV[s_vCount][1] = 0.0f;
    s_vColor[s_vCount][0] = s_curVertexColor[0];
    s_vColor[s_vCount][1] = s_curVertexColor[1];
    s_vColor[s_vCount][2] = s_curVertexColor[2];
    s_vColor[s_vCount][3] = s_curVertexColor[3];
    s_vCount++;
}

void GXColor4u8(u8 r, u8 g, u8 b, u8 a) {
    if (s_vCount == 0 || s_vCount > MAXV) return;
    s_curVertexColor[0] = r / 255.0f;
    s_curVertexColor[1] = g / 255.0f;
    s_curVertexColor[2] = b / 255.0f;
    s_curVertexColor[3] = a / 255.0f;
    s_vColor[s_vCount - 1][0] = s_curVertexColor[0];
    s_vColor[s_vCount - 1][1] = s_curVertexColor[1];
    s_vColor[s_vCount - 1][2] = s_curVertexColor[2];
    s_vColor[s_vCount - 1][3] = s_curVertexColor[3];
}

void GXTexCoord2f32(f32 s, f32 t) {
    if (s_vCount == 0 || s_vCount > MAXV) return;
    s_vUV[s_vCount - 1][0] = s;
    s_vUV[s_vCount - 1][1] = t;
}

void GXEnd(void) {
    if (s_vCount < 3) { s_vCount = 0; return; }
    // Expand quads (0,1,2,3) into two triangles (0,1,2)(0,2,3).
    float clipXY[MAXV * 3 * 2];
    float clipXYZ[MAXV * 3 * 3];
    float uv[MAXV * 3 * 2];
    float color[MAXV * 3 * 4];
    int n = 0;
    if (s_prim == GX_QUADS) {
        for (int q = 0; q + 3 < s_vCount; q += 4) {
            int idx[6] = { q, q+1, q+2, q, q+2, q+3 };
            for (int j = 0; j < 6; j++) {
                clipXY[n*2+0] = s_vClip[idx[j]][0]; clipXY[n*2+1] = s_vClip[idx[j]][1];
                clipXYZ[n*3+0] = s_vClip[idx[j]][0];
                clipXYZ[n*3+1] = s_vClip[idx[j]][1];
                clipXYZ[n*3+2] = s_vClip[idx[j]][2];
                uv[n*2+0] = s_vUV[idx[j]][0];     uv[n*2+1] = s_vUV[idx[j]][1];
                color[n*4+0] = s_vColor[idx[j]][0];
                color[n*4+1] = s_vColor[idx[j]][1];
                color[n*4+2] = s_vColor[idx[j]][2];
                color[n*4+3] = s_vColor[idx[j]][3];
                n++;
            }
        }
    } else if (s_prim == GX_TRIANGLES) {
        for (int q = 0; q + 2 < s_vCount; q += 3) {
            for (int k = 0; k < 3; k++) {
                clipXY[n*2+0] = s_vClip[q+k][0]; clipXY[n*2+1] = s_vClip[q+k][1];
                clipXYZ[n*3+0] = s_vClip[q+k][0];
                clipXYZ[n*3+1] = s_vClip[q+k][1];
                clipXYZ[n*3+2] = s_vClip[q+k][2];
                uv[n*2+0] = s_vUV[q+k][0];     uv[n*2+1] = s_vUV[q+k][1];
                color[n*4+0] = s_vColor[q+k][0];
                color[n*4+1] = s_vColor[q+k][1];
                color[n*4+2] = s_vColor[q+k][2];
                color[n*4+3] = s_vColor[q+k][3];
                n++;
            }
        }
    } else if (s_prim == GX_TRIANGLESTRIP) {
        for (int j = 0; j + 2 < s_vCount; j++) {
            int idx[3];
            if (j & 1) {
                idx[0] = j + 1; idx[1] = j; idx[2] = j + 2;
            } else {
                idx[0] = j; idx[1] = j + 1; idx[2] = j + 2;
            }
            for (int k = 0; k < 3; k++) {
                clipXY[n*2+0] = s_vClip[idx[k]][0]; clipXY[n*2+1] = s_vClip[idx[k]][1];
                clipXYZ[n*3+0] = s_vClip[idx[k]][0];
                clipXYZ[n*3+1] = s_vClip[idx[k]][1];
                clipXYZ[n*3+2] = s_vClip[idx[k]][2];
                uv[n*2+0] = s_vUV[idx[k]][0];     uv[n*2+1] = s_vUV[idx[k]][1];
                color[n*4+0] = s_vColor[idx[k]][0];
                color[n*4+1] = s_vColor[idx[k]][1];
                color[n*4+2] = s_vColor[idx[k]][2];
                color[n*4+3] = s_vColor[idx[k]][3];
                n++;
            }
        }
    } else {  // treat other unsupported primitives as a fan from vertex 0
        for (int j = 1; j + 1 < s_vCount; j++) {
            int idx[3] = { 0, j, j+1 };
            for (int k = 0; k < 3; k++) {
                clipXY[n*2+0] = s_vClip[idx[k]][0]; clipXY[n*2+1] = s_vClip[idx[k]][1];
                clipXYZ[n*3+0] = s_vClip[idx[k]][0];
                clipXYZ[n*3+1] = s_vClip[idx[k]][1];
                clipXYZ[n*3+2] = s_vClip[idx[k]][2];
                uv[n*2+0] = s_vUV[idx[k]][0];     uv[n*2+1] = s_vUV[idx[k]][1];
                color[n*4+0] = s_vColor[idx[k]][0];
                color[n*4+1] = s_vColor[idx[k]][1];
                color[n*4+2] = s_vColor[idx[k]][2];
                color[n*4+3] = s_vColor[idx[k]][3];
                n++;
            }
        }
    }
    if (s_curTex) {
        if (s_3dMode) {
            Gfx3D_DrawTexTris(clipXYZ, uv, color, n, s_curTex,
                              s_tint[0], s_tint[1], s_tint[2], s_tint[3]);
        } else {
            Gfx2D_DrawTexTris(clipXY, uv, n, s_curTex,
                              s_tint[0], s_tint[1], s_tint[2], s_tint[3]);
        }
    } else {
        if (s_3dMode) {
            Gfx3D_DrawSolidTris(clipXYZ, color, n,
                                s_tint[0], s_tint[1], s_tint[2], s_tint[3]);
        } else {
            Gfx2D_DrawSolidTris(clipXY, n,
                                s_tint[0], s_tint[1], s_tint[2], s_tint[3]);
        }
    }
    s_vCount = 0;
}

// Capture the material color as our draw tint (texture is modulated by it).
void GXSetChanMatColor(GXChannelID id, GXColor c) {
    (void)id;
    s_tint[0] = c.r / 255.0f; s_tint[1] = c.g / 255.0f;
    s_tint[2] = c.b / 255.0f; s_tint[3] = c.a / 255.0f;
}
// Remaining GX state functions are kept as loose no-ops in sys_switch.c.

#endif // __SWITCH__
