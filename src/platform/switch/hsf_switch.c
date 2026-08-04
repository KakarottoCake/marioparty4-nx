#ifdef __SWITCH__

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "game/hu3d.h"
#include "game/hsfformat.h"
#include "game/memory.h"
#include "dolphin/gx/GXVert.h"
#include "dolphin/os.h"

extern void GXSet3DMode(u8 enable);
extern HU3DMOTION Hu3DMotion[HU3D_MOTION_MAX];
extern float GetCurve(HSFTRACK *track, float time);

/*
 * HSF files are PPC data.  Their pointer fields are 32-bit offsets and all
 * numeric fields are big-endian.  The Switch ABI has 64-bit pointers, so the
 * original loader cannot be used in place.  This loader copies the small
 * static-model subset into native structures and leaves the source block
 * freeable by the original model manager.
 */

#define SWITCH_HSF_HEADER_SIZE 0xB0
#define SWITCH_HSF_SECTION_COUNT 21
#define SWITCH_HSF_MAX_COUNT 8192
#define SWITCH_HSF_MAX_CHILDREN 4096
#define SWITCH_HSF_FACE_SIZE 0x30
#define SWITCH_HSF_OBJECT_SIZE 0x144
#define SWITCH_HSF_MATERIAL_SIZE 0x3C
#define SWITCH_HSF_ATTRIBUTE_SIZE 0x84
#define SWITCH_HSF_BITMAP_SIZE 0x20
#define SWITCH_HSF_PALETTE_SIZE 0x10
#define SWITCH_HSF_MOTION_SIZE 0x10
#define SWITCH_HSF_TRACK_SIZE 0x10
#define SWITCH_HSF_CENV_SIZE 0x24
#define SWITCH_HSF_CENV_SINGLE_SIZE 0x0C
#define SWITCH_HSF_CENV_DUAL_SIZE 0x10
#define SWITCH_HSF_CENV_MULTI_SIZE 0x10
#define SWITCH_HSF_CENV_DUAL_WEIGHT_SIZE 0x0C
#define SWITCH_HSF_CENV_MULTI_WEIGHT_SIZE 0x08
#define SWITCH_HSF_SKELETON_SIZE 0x28
#define SWITCH_HSF_PART_SIZE 0x0C
#define SWITCH_HSF_CLUSTER_SIZE 0xA0
#define SWITCH_HSF_SHAPE_SIZE 0x0C
#define SWITCH_HSF_BUFFER_SIZE 12
#define SWITCH_HSF_MAX_DEPTH 128

enum {
    SWITCH_HSF_SCENE = 0,
    SWITCH_HSF_COLOR,
    SWITCH_HSF_MATERIAL,
    SWITCH_HSF_ATTRIBUTE,
    SWITCH_HSF_VERTEX,
    SWITCH_HSF_NORMAL,
    SWITCH_HSF_ST,
    SWITCH_HSF_FACE,
    SWITCH_HSF_OBJECT,
    SWITCH_HSF_BITMAP,
    SWITCH_HSF_PALETTE,
    SWITCH_HSF_MOTION,
    SWITCH_HSF_CENV,
    SWITCH_HSF_SKELETON,
    SWITCH_HSF_PART,
    SWITCH_HSF_CLUSTER,
    SWITCH_HSF_SHAPE,
    SWITCH_HSF_MAP_ATTR,
    SWITCH_HSF_MATRIX,
    SWITCH_HSF_SYMBOL,
    SWITCH_HSF_STRING
};

typedef struct SwitchHsfSection_s {
    u32 ofs;
    u32 count;
} SwitchHsfSection;

typedef struct SwitchHsfHeader_s {
    SwitchHsfSection section[SWITCH_HSF_SECTION_COUNT];
} SwitchHsfHeader;

typedef struct SwitchHsfArena_s {
    u8 *base;
    u32 size;
    u32 used;
} SwitchHsfArena;

typedef struct SwitchHsfContext_s {
    const u8 *raw;
    u32 rawSize;
    SwitchHsfHeader header;
    SwitchHsfArena arena;
    char *strings;
    u32 stringSize;
    HSFBUFFER *vertex;
    HSFBUFFER *normal;
    HSFBUFFER *st;
    HSFBUFFER *color;
    HSFBUFFER *face;
    HSFMATERIAL *material;
    HSFATTRIBUTE *attribute;
    HSFBITMAP *bitmap;
    HSFPALETTE *palette;
    HSFSKELETON *skeleton;
    HSFCENV *cenv;
    HSFMOTION *motion;
    HSFPART *part;
    HSFCLUSTER *cluster;
    HSFSHAPE *shape;
} SwitchHsfContext;

static BOOL SwitchHsfEstimateAdd(u32 *total, u32 count, u32 size);
const char *SwitchHsfMotionTargetName(const HSFMOTION *motion, u16 offset);

static u16 SwitchHsfBE16(const u8 *p) {
    return (u16)(((u16)p[0] << 8) | p[1]);
}

static s16 SwitchHsfS16(const u8 *p) {
    return (s16)SwitchHsfBE16(p);
}

static u32 SwitchHsfBE32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | p[3];
}

static s32 SwitchHsfS32(const u8 *p) {
    return (s32)SwitchHsfBE32(p);
}

static float SwitchHsfF32(const u8 *p) {
    u32 bits = SwitchHsfBE32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static BOOL SwitchHsfRange(const SwitchHsfContext *ctx, u32 ofs, u32 size) {
    if (ofs > ctx->rawSize || size > ctx->rawSize - ofs) {
        return FALSE;
    }
    return TRUE;
}

static BOOL SwitchHsfCountOK(u32 count) {
    return count <= SWITCH_HSF_MAX_COUNT;
}

static void *SwitchHsfArenaAlloc(SwitchHsfArena *arena, u32 size) {
    u32 aligned;
    if (!arena || size > arena->size) {
        return NULL;
    }
    aligned = (arena->used + 7U) & ~7U;
    if (aligned > arena->size || size > arena->size - aligned) {
        return NULL;
    }
    arena->used = aligned + size;
    memset(arena->base + aligned, 0, size);
    return arena->base + aligned;
}

static char *SwitchHsfString(SwitchHsfContext *ctx, u32 offset) {
    if (offset == 0xFFFFFFFFU || !ctx->strings || offset >= ctx->stringSize) {
        return NULL;
    }
    return ctx->strings + offset;
}

static BOOL SwitchHsfHeaderRead(SwitchHsfContext *ctx, const void *data) {
    const u8 *raw = (const u8 *)data;
    s32 i;

    if (!ctx || !raw || ctx->rawSize < SWITCH_HSF_HEADER_SIZE ||
        memcmp(raw, "HSFV", 4) != 0) {
        return FALSE;
    }
    ctx->raw = raw;
    for (i = 0; i < SWITCH_HSF_SECTION_COUNT; i++) {
        ctx->header.section[i].ofs = SwitchHsfBE32(raw + 8 + i * 8);
        ctx->header.section[i].count = SwitchHsfBE32(raw + 12 + i * 8);
        if (!SwitchHsfCountOK(ctx->header.section[i].count) ||
            ctx->header.section[i].ofs > ctx->rawSize) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL SwitchHsfTableRange(const SwitchHsfContext *ctx,
                                s32 sectionId, u32 elementSize) {
    const SwitchHsfSection *section = &ctx->header.section[sectionId];
    if (!SwitchHsfCountOK(section->count) ||
        section->count > 0xFFFFFFFFU / elementSize) {
        return FALSE;
    }
    return SwitchHsfRange(ctx, section->ofs, section->count * elementSize);
}

static BOOL SwitchHsfSymbol(const SwitchHsfContext *ctx, u32 index, u32 *value) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_SYMBOL];
    u32 ofs;
    if (!value || index >= section->count || index > 0x3FFFFFFFU) {
        return FALSE;
    }
    ofs = section->ofs + index * 4;
    if (!SwitchHsfRange(ctx, ofs, 4)) {
        return FALSE;
    }
    *value = SwitchHsfBE32(ctx->raw + ofs);
    return TRUE;
}

static void SwitchHsfCopyTransform(HSFTRANSFORM *dst, const u8 *raw) {
    dst->pos.x = SwitchHsfF32(raw + 0);
    dst->pos.y = SwitchHsfF32(raw + 4);
    dst->pos.z = SwitchHsfF32(raw + 8);
    dst->rot.x = SwitchHsfF32(raw + 12);
    dst->rot.y = SwitchHsfF32(raw + 16);
    dst->rot.z = SwitchHsfF32(raw + 20);
    dst->scale.x = SwitchHsfF32(raw + 24);
    dst->scale.y = SwitchHsfF32(raw + 28);
    dst->scale.z = SwitchHsfF32(raw + 32);
}

static HSFBUFFER *SwitchHsfParseBuffers(SwitchHsfContext *ctx, s32 sectionId,
                                         u32 elementSize, s32 kind) {
    const SwitchHsfSection *section = &ctx->header.section[sectionId];
    HSFBUFFER *buffers;
    u32 tableSize;
    u32 dataBase;
    u32 i;

    if (section->count == 0) {
        return NULL;
    }
    if (!SwitchHsfTableRange(ctx, sectionId, SWITCH_HSF_BUFFER_SIZE) ||
        section->count > 0x3FFFFFFFU / SWITCH_HSF_BUFFER_SIZE) {
        return NULL;
    }
    tableSize = section->count * SWITCH_HSF_BUFFER_SIZE;
    dataBase = section->ofs + tableSize;
    if (!SwitchHsfRange(ctx, dataBase, 0)) {
        return NULL;
    }
    buffers = (HSFBUFFER *)SwitchHsfArenaAlloc(&ctx->arena,
                                                section->count * sizeof(HSFBUFFER));
    if (!buffers) {
        return NULL;
    }

    for (i = 0; i < section->count; i++) {
        const u8 *record = ctx->raw + section->ofs + i * SWITCH_HSF_BUFFER_SIZE;
        u32 count = SwitchHsfBE32(record + 4);
        u32 dataOfs = SwitchHsfBE32(record + 8);
        u32 byteCount;
        const u8 *source;
        void *destination;
        u32 j;

        if (count > 0x3FFFFFFFU / elementSize) {
            return NULL;
        }
        byteCount = count * elementSize;
        if (dataOfs > ctx->rawSize - dataBase ||
            !SwitchHsfRange(ctx, dataBase + dataOfs, byteCount)) {
            return NULL;
        }
        source = ctx->raw + dataBase + dataOfs;
        destination = SwitchHsfArenaAlloc(&ctx->arena, byteCount ? byteCount : 1);
        if (!destination) {
            return NULL;
        }
        buffers[i].name = SwitchHsfString(ctx, SwitchHsfBE32(record));
        buffers[i].count = (s32)count;
        buffers[i].data = destination;

        if (kind == 0) {
            HuVecF *out = (HuVecF *)destination;
            for (j = 0; j < count; j++) {
                out[j].x = SwitchHsfF32(source + j * 12 + 0);
                out[j].y = SwitchHsfF32(source + j * 12 + 4);
                out[j].z = SwitchHsfF32(source + j * 12 + 8);
            }
        } else if (kind == 1) {
            Vec *out = (Vec *)destination;
            for (j = 0; j < count; j++) {
                out[j].x = SwitchHsfF32(source + j * 12 + 0);
                out[j].y = SwitchHsfF32(source + j * 12 + 4);
                out[j].z = SwitchHsfF32(source + j * 12 + 8);
            }
        } else if (kind == 2) {
            HuVec2f *out = (HuVec2f *)destination;
            for (j = 0; j < count; j++) {
                out[j].x = SwitchHsfF32(source + j * 8 + 0);
                out[j].y = SwitchHsfF32(source + j * 8 + 4);
            }
        } else {
            memcpy(destination, source, byteCount);
        }
    }
    return buffers;
}

static HSFBUFFER *SwitchHsfParseNormals(SwitchHsfContext *ctx) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_NORMAL];
    u32 stride = ctx->header.section[SWITCH_HSF_CENV].count ? 12 : 3;
    HSFBUFFER *buffers;
    u32 tableSize;
    u32 dataBase;
    u32 i;

    if (section->count == 0) {
        return NULL;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_NORMAL, SWITCH_HSF_BUFFER_SIZE)) {
        return NULL;
    }
    tableSize = section->count * SWITCH_HSF_BUFFER_SIZE;
    dataBase = section->ofs + tableSize;
    if (!SwitchHsfRange(ctx, dataBase, 0)) {
        return NULL;
    }
    buffers = (HSFBUFFER *)SwitchHsfArenaAlloc(&ctx->arena,
                                                section->count * sizeof(HSFBUFFER));
    if (!buffers) {
        return NULL;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *record = ctx->raw + section->ofs + i * SWITCH_HSF_BUFFER_SIZE;
        u32 count = SwitchHsfBE32(record + 4);
        u32 dataOfs = SwitchHsfBE32(record + 8);
        u32 byteCount;
        const u8 *source;
        void *destination;
        u32 j;

        if (count > 0x3FFFFFFFU / stride) {
            return NULL;
        }
        byteCount = count * stride;
        if (dataOfs > ctx->rawSize - dataBase ||
            !SwitchHsfRange(ctx, dataBase + dataOfs, byteCount)) {
            return NULL;
        }
        source = ctx->raw + dataBase + dataOfs;
        destination = SwitchHsfArenaAlloc(&ctx->arena, byteCount ? byteCount : 1);
        if (!destination) {
            return NULL;
        }
        buffers[i].name = SwitchHsfString(ctx, SwitchHsfBE32(record));
        buffers[i].count = (s32)count;
        buffers[i].data = destination;
        if (stride == 12) {
            Vec *out = (Vec *)destination;
            for (j = 0; j < count; j++) {
                out[j].x = SwitchHsfF32(source + j * 12 + 0);
                out[j].y = SwitchHsfF32(source + j * 12 + 4);
                out[j].z = SwitchHsfF32(source + j * 12 + 8);
            }
        } else {
            memcpy(destination, source, byteCount);
        }
    }
    return buffers;
}

static HSFBUFFER *SwitchHsfParseFaces(SwitchHsfContext *ctx) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_FACE];
    HSFBUFFER *buffers;
    u32 tableSize;
    u32 dataBase;
    u32 i;

    if (section->count == 0) {
        return NULL;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_FACE, SWITCH_HSF_BUFFER_SIZE)) {
        return NULL;
    }
    tableSize = section->count * SWITCH_HSF_BUFFER_SIZE;
    dataBase = section->ofs + tableSize;
    if (!SwitchHsfRange(ctx, dataBase, 0)) {
        return NULL;
    }
    buffers = (HSFBUFFER *)SwitchHsfArenaAlloc(&ctx->arena,
                                                section->count * sizeof(HSFBUFFER));
    if (!buffers) {
        return NULL;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *record = ctx->raw + section->ofs + i * SWITCH_HSF_BUFFER_SIZE;
        u32 count = SwitchHsfBE32(record + 4);
        u32 dataOfs = SwitchHsfBE32(record + 8);
        u32 byteCount;
        const u8 *source;
        HSFFACE *destination;
        u32 j;

        if (count > 0x3FFFFFFFU / SWITCH_HSF_FACE_SIZE) {
            return NULL;
        }
        byteCount = count * SWITCH_HSF_FACE_SIZE;
        if (dataOfs > ctx->rawSize - dataBase ||
            !SwitchHsfRange(ctx, dataBase + dataOfs, byteCount)) {
            return NULL;
        }
        source = ctx->raw + dataBase + dataOfs;
        destination = (HSFFACE *)SwitchHsfArenaAlloc(
            &ctx->arena, count ? count * sizeof(HSFFACE) : 1);
        if (!destination) {
            return NULL;
        }
        buffers[i].name = SwitchHsfString(ctx, SwitchHsfBE32(record));
        buffers[i].count = (s32)count;
        buffers[i].data = destination;
        for (j = 0; j < count; j++) {
            const u8 *rawFace = source + j * SWITCH_HSF_FACE_SIZE;
            HSFFACE *face = &destination[j];
            s32 k;
            face->type = SwitchHsfS16(rawFace + 0);
            face->mat = SwitchHsfS16(rawFace + 2);
            for (k = 0; k < 4; k++) {
                face->indices[k][0] = SwitchHsfS16(rawFace + 4 + k * 8);
                face->indices[k][1] = SwitchHsfS16(rawFace + 6 + k * 8);
                face->indices[k][2] = SwitchHsfS16(rawFace + 8 + k * 8);
                face->indices[k][3] = SwitchHsfS16(rawFace + 10 + k * 8);
            }
            face->nbt.x = SwitchHsfF32(rawFace + 0x24);
            face->nbt.y = SwitchHsfF32(rawFace + 0x28);
            face->nbt.z = SwitchHsfF32(rawFace + 0x2C);
            if ((face->type & HSF_FACE_MASK) == HSF_FACE_TRISTRIP) {
                u32 stripCount = SwitchHsfBE32(rawFace + 0x1C);
                u32 stripOfs = SwitchHsfBE32(rawFace + 0x20);
                u32 stripBytes;
                u8 *stripData;
                u32 stripIndex;
                if (stripCount > SWITCH_HSF_MAX_COUNT ||
                    stripCount > 0x3FFFFFFFU / 8U ||
                    stripOfs > 0x3FFFFFFFU / 8U) {
                    face->strip.count = 0;
                    face->strip.data = NULL;
                    continue;
                }
                stripBytes = stripCount * 8U;
                if (stripOfs > (ctx->rawSize - dataBase) / 8U ||
                    !SwitchHsfRange(ctx, dataBase + stripOfs * 8U, stripBytes)) {
                    face->strip.count = 0;
                    face->strip.data = NULL;
                    continue;
                }
                stripData = (u8 *)SwitchHsfArenaAlloc(
                    &ctx->arena, stripBytes ? stripBytes : 1);
                if (!stripData) {
                    return NULL;
                }
                for (stripIndex = 0; stripIndex < stripCount; stripIndex++) {
                    s32 corner;
                    for (corner = 0; corner < 4; corner++) {
                        ((s16 *)stripData)[stripIndex * 4 + corner] =
                            SwitchHsfS16(ctx->raw + dataBase + stripOfs * 8U +
                                         stripIndex * 8U + corner * 2U);
                    }
                }
                face->strip.count = stripCount;
                face->strip.data = (s16 *)stripData;
            }
        }
    }
    return buffers;
}

static HSFSCENE *SwitchHsfParseScene(SwitchHsfContext *ctx) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_SCENE];
    HSFSCENE *scene;
    u32 i;
    if (section->count == 0) {
        return NULL;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_SCENE, 16)) {
        return NULL;
    }
    scene = (HSFSCENE *)SwitchHsfArenaAlloc(&ctx->arena,
                                             section->count * sizeof(HSFSCENE));
    if (!scene) {
        return NULL;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs + i * 16;
        scene[i].fogType = (GXFogType)SwitchHsfBE32(raw + 0);
        scene[i].fogStart = SwitchHsfF32(raw + 4);
        scene[i].fogEnd = SwitchHsfF32(raw + 8);
        scene[i].color.r = raw[12];
        scene[i].color.g = raw[13];
        scene[i].color.b = raw[14];
        scene[i].color.a = raw[15];
    }
    return scene;
}

static HSFMATERIAL *SwitchHsfParseMaterials(SwitchHsfContext *ctx) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_MATERIAL];
    HSFMATERIAL *materials;
    u32 i;
    if (section->count == 0) {
        return NULL;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_MATERIAL, SWITCH_HSF_MATERIAL_SIZE)) {
        return NULL;
    }
    materials = (HSFMATERIAL *)SwitchHsfArenaAlloc(
        &ctx->arena, section->count * sizeof(HSFMATERIAL));
    if (!materials) {
        return NULL;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs + i * SWITCH_HSF_MATERIAL_SIZE;
        HSFMATERIAL *material = &materials[i];
        material->name = SwitchHsfString(ctx, SwitchHsfBE32(raw + 0));
        material->pass = SwitchHsfBE16(raw + 8);
        material->vtxMode = raw[10];
        memcpy(material->litColor, raw + 11, 3);
        memcpy(material->color, raw + 14, 3);
        memcpy(material->shadowColor, raw + 17, 3);
        material->hiliteScale = SwitchHsfF32(raw + 0x14);
        material->unk18 = SwitchHsfF32(raw + 0x18);
        material->invAlpha = SwitchHsfF32(raw + 0x1C);
        material->unk20[0] = SwitchHsfF32(raw + 0x20);
        material->unk20[1] = SwitchHsfF32(raw + 0x24);
        material->refAlpha = SwitchHsfF32(raw + 0x28);
        material->unk2C = SwitchHsfF32(raw + 0x2C);
        material->flags = SwitchHsfBE32(raw + 0x30);
        material->attrNum = SwitchHsfBE32(raw + 0x34);
        material->attr = NULL;
        if (material->attrNum != 0) {
            u32 attrSymbol = SwitchHsfBE32(raw + 0x38);
            u32 j;
            if (material->attrNum > 256 ||
                attrSymbol > ctx->header.section[SWITCH_HSF_SYMBOL].count ||
                material->attrNum > ctx->header.section[SWITCH_HSF_SYMBOL].count - attrSymbol) {
                return NULL;
            }
            material->attr = (s32 *)SwitchHsfArenaAlloc(
                &ctx->arena, material->attrNum * sizeof(s32));
            if (!material->attr) {
                return NULL;
            }
            for (j = 0; j < material->attrNum; j++) {
                u32 attrIndex;
                if (!SwitchHsfSymbol(ctx, attrSymbol + j, &attrIndex) ||
                    attrIndex >= ctx->header.section[SWITCH_HSF_ATTRIBUTE].count) {
                    material->attr[j] = -1;
                } else {
                    material->attr[j] = (s32)attrIndex;
                }
            }
        }
    }
    return materials;
}

static u32 SwitchHsfRoundUp(u32 value, u32 alignment) {
    u32 remainder;
    if (alignment == 0) {
        return value;
    }
    remainder = value % alignment;
    if (remainder != 0 && value > 0xFFFFFFFFU - (alignment - remainder)) {
        return 0;
    }
    return value + (alignment - remainder) % alignment;
}

static u32 SwitchHsfBitmapBytes(u8 format, u8 pixSize, u32 width, u32 height) {
    u32 tileWidth;
    u32 tileHeight;
    u32 bytesPerTile;

    if (width == 0 || height == 0 || width > 4096 || height > 4096) {
        return 0;
    }
    switch (format) {
        case HSF_BMPFMT_RGBA8:
            tileWidth = 4; tileHeight = 4; bytesPerTile = 64; break;
        case HSF_BMPFMT_RGB565:
        case HSF_BMPFMT_RGB5A3:
        case HSF_BMPFMT_IA8:
            tileWidth = 4; tileHeight = 4; bytesPerTile = 32; break;
        case HSF_BMPFMT_I4:
            tileWidth = 8; tileHeight = 8; bytesPerTile = 32; break;
        case HSF_BMPFMT_I8:
        case HSF_BMPFMT_IA4:
            tileWidth = 8; tileHeight = 4; bytesPerTile = 32; break;
        case HSF_BMPFMT_CMPR:
            tileWidth = 8; tileHeight = 8; bytesPerTile = 32; break;
        case HSF_BMPFMT_CI_RGB565:
        case HSF_BMPFMT_CI_RGB5A3:
        case HSF_BMPFMT_CI_IA8:
            if (pixSize < 8) {
                tileWidth = 8; tileHeight = 8; bytesPerTile = 32;
            } else {
                tileWidth = 8; tileHeight = 4; bytesPerTile = 32;
            }
            break;
        default:
            return 0;
    }
    width = SwitchHsfRoundUp(width, tileWidth);
    height = SwitchHsfRoundUp(height, tileHeight);
    if (width == 0 || height == 0 || height > 0xFFFFFFFFU / width ||
        width * height > 0xFFFFFFFFU / bytesPerTile) {
        return 0;
    }
    return (width / tileWidth) * (height / tileHeight) * bytesPerTile;
}

static HSFPALETTE *SwitchHsfParsePalettes(SwitchHsfContext *ctx) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_PALETTE];
    HSFPALETTE *palettes;
    u32 tableSize;
    u32 dataBase;
    u32 i;

    if (section->count == 0) {
        return NULL;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_PALETTE, SWITCH_HSF_PALETTE_SIZE) ||
        section->count > 0xFFFFFFFFU / SWITCH_HSF_PALETTE_SIZE) {
        return NULL;
    }
    tableSize = section->count * SWITCH_HSF_PALETTE_SIZE;
    if (section->ofs > 0xFFFFFFFFU - tableSize) {
        return NULL;
    }
    dataBase = section->ofs + tableSize;
    if (!SwitchHsfRange(ctx, dataBase, 0)) {
        return NULL;
    }
    palettes = (HSFPALETTE *)SwitchHsfArenaAlloc(
        &ctx->arena, section->count * sizeof(HSFPALETTE));
    if (!palettes) {
        return NULL;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs + i * SWITCH_HSF_PALETTE_SIZE;
        HSFPALETTE *palette = &palettes[i];
        u32 entries = SwitchHsfBE32(raw + 8);
        u32 dataOfs = SwitchHsfBE32(raw + 12);
        u32 byteCount;
        void *destination;
        if (entries > 0x10000U || entries > 0xFFFFFFFFU / 2U) {
            return NULL;
        }
        byteCount = entries * 2;
        if (dataOfs > ctx->rawSize - dataBase ||
            !SwitchHsfRange(ctx, dataBase + dataOfs, byteCount)) {
            return NULL;
        }
        destination = SwitchHsfArenaAlloc(&ctx->arena, byteCount ? byteCount : 1);
        if (!destination) {
            return NULL;
        }
        palette->name = SwitchHsfString(ctx, SwitchHsfBE32(raw));
        palette->unk = SwitchHsfS32(raw + 4);
        palette->palSize = entries;
        palette->data = (u16 *)destination;
        memcpy(destination, ctx->raw + dataBase + dataOfs, byteCount);
    }
    return palettes;
}

static HSFBITMAP *SwitchHsfParseBitmaps(SwitchHsfContext *ctx) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_BITMAP];
    HSFBITMAP *bitmaps;
    u32 tableSize;
    u32 dataBase;
    u32 i;
    if (section->count == 0) {
        return NULL;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_BITMAP, SWITCH_HSF_BITMAP_SIZE)) {
        return NULL;
    }
    tableSize = section->count * SWITCH_HSF_BITMAP_SIZE;
    if (section->ofs > 0xFFFFFFFFU - tableSize) {
        return NULL;
    }
    dataBase = section->ofs + tableSize;
    if (!SwitchHsfRange(ctx, dataBase, 0)) {
        return NULL;
    }
    bitmaps = (HSFBITMAP *)SwitchHsfArenaAlloc(
        &ctx->arena, section->count * sizeof(HSFBITMAP));
    if (!bitmaps) {
        return NULL;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs + i * SWITCH_HSF_BITMAP_SIZE;
        HSFBITMAP *bitmap = &bitmaps[i];
        bitmap->name = SwitchHsfString(ctx, SwitchHsfBE32(raw + 0));
        bitmap->maxLod = SwitchHsfBE32(raw + 4);
        bitmap->dataFmt = raw[8];
        bitmap->pixSize = raw[9];
        bitmap->sizeX = SwitchHsfS16(raw + 10);
        bitmap->sizeY = SwitchHsfS16(raw + 12);
        bitmap->palSize = SwitchHsfS16(raw + 14);
        bitmap->tint.r = raw[16];
        bitmap->tint.g = raw[17];
        bitmap->tint.b = raw[18];
        bitmap->tint.a = raw[19];
        bitmap->palData = NULL;
        bitmap->unk = SwitchHsfBE32(raw + 0x18);
        if (ctx->palette) {
            u32 paletteIndex = SwitchHsfBE32(raw + 0x14);
            if (paletteIndex != 0xFFFFFFFFU &&
                paletteIndex < ctx->header.section[SWITCH_HSF_PALETTE].count) {
                bitmap->palData = ctx->palette[paletteIndex].data;
            }
        }
        {
            u32 byteCount = SwitchHsfBitmapBytes(
                bitmap->dataFmt, bitmap->pixSize,
                (u32)(bitmap->sizeX < 0 ? 0 : bitmap->sizeX),
                (u32)(bitmap->sizeY < 0 ? 0 : bitmap->sizeY));
            u32 dataOfs = SwitchHsfBE32(raw + 0x1C);
            void *destination;
            if (byteCount == 0) {
                bitmap->data = NULL;
            } else {
                if (dataOfs > ctx->rawSize - dataBase ||
                    !SwitchHsfRange(ctx, dataBase + dataOfs, byteCount)) {
                    return NULL;
                }
                destination = SwitchHsfArenaAlloc(&ctx->arena, byteCount);
                if (!destination) {
                    return NULL;
                }
                memcpy(destination, ctx->raw + dataBase + dataOfs, byteCount);
                bitmap->data = destination;
            }
        }
    }
    return bitmaps;
}

static HSFATTRIBUTE *SwitchHsfParseAttributes(SwitchHsfContext *ctx) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_ATTRIBUTE];
    HSFATTRIBUTE *attributes;
    u32 i;
    if (section->count == 0) {
        return NULL;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_ATTRIBUTE, SWITCH_HSF_ATTRIBUTE_SIZE)) {
        return NULL;
    }
    attributes = (HSFATTRIBUTE *)SwitchHsfArenaAlloc(
        &ctx->arena, section->count * sizeof(HSFATTRIBUTE));
    if (!attributes) {
        return NULL;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs + i * SWITCH_HSF_ATTRIBUTE_SIZE;
        HSFATTRIBUTE *attribute = &attributes[i];
        u32 bitmap = SwitchHsfBE32(raw + 0x80);
        attribute->name = SwitchHsfString(ctx, SwitchHsfBE32(raw + 0));
        attribute->kColor = SwitchHsfF32(raw + 0x0C);
        attribute->nbtTpLvl = SwitchHsfF32(raw + 0x14);
        attribute->unk20 = SwitchHsfF32(raw + 0x20);
        attribute->scale.x = SwitchHsfF32(raw + 0x28);
        attribute->scale.y = SwitchHsfF32(raw + 0x2C);
        attribute->trans.x = SwitchHsfF32(raw + 0x30);
        attribute->trans.y = SwitchHsfF32(raw + 0x34);
        attribute->wrapS = SwitchHsfBE32(raw + 0x64);
        attribute->wrapT = SwitchHsfBE32(raw + 0x68);
        attribute->maxLod = SwitchHsfBE32(raw + 0x78);
        attribute->flag = SwitchHsfBE32(raw + 0x7C);
        if (bitmap != 0xFFFFFFFFU && bitmap < ctx->header.section[SWITCH_HSF_BITMAP].count) {
            attribute->bitmap = &ctx->bitmap[bitmap];
        }
    }
    return attributes;
}

static HSFCONSTDATA *SwitchHsfMakeConst(SwitchHsfContext *ctx) {
    HSFCONSTDATA *constant = (HSFCONSTDATA *)SwitchHsfArenaAlloc(
        &ctx->arena, sizeof(HSFCONSTDATA));
    if (!constant) {
        return NULL;
    }
    constant->attr = HU3D_CONST_NONE;
    constant->hookMdlId = HU3D_MODELID_NONE;
    constant->drawData = NULL;
    constant->dlBuf = NULL;
    PSMTXIdentity(constant->matrix);
    constant->hiliteMap = NULL;
    return constant;
}

static BOOL SwitchHsfMeshIndex(u32 rawIndex, u32 count, u32 *index) {
    if (!index || rawIndex == 0xFFFFFFFFU || rawIndex >= count) {
        return FALSE;
    }
    *index = rawIndex;
    return TRUE;
}

static BOOL SwitchHsfRelativeRange(const SwitchHsfContext *ctx, u32 base,
                                   u32 offset, u32 size) {
    if (!ctx || base > ctx->rawSize || offset > ctx->rawSize - base ||
        size > ctx->rawSize - (base + offset)) {
        return FALSE;
    }
    return TRUE;
}

static HSFSKELETON *SwitchHsfParseSkeleton(SwitchHsfContext *ctx) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_SKELETON];
    HSFSKELETON *skeleton;
    u32 i;

    if (section->count == 0) {
        return NULL;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_SKELETON,
                             SWITCH_HSF_SKELETON_SIZE)) {
        return NULL;
    }
    skeleton = (HSFSKELETON *)SwitchHsfArenaAlloc(
        &ctx->arena, section->count * sizeof(HSFSKELETON));
    if (!skeleton) {
        return NULL;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs +
                        i * SWITCH_HSF_SKELETON_SIZE;
        skeleton[i].name = SwitchHsfString(ctx, SwitchHsfBE32(raw));
        SwitchHsfCopyTransform(&skeleton[i].transform, raw + 4);
    }
    return skeleton;
}

static BOOL SwitchHsfCenvDescriptorSizes(const SwitchHsfContext *ctx,
                                         u32 *descriptorBytes,
                                         u32 *weightBase) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_CENV];
    u32 dataBase;
    u32 descriptors = 0;
    u32 i;

    if (!descriptorBytes || !weightBase || section->count == 0 ||
        !SwitchHsfTableRange(ctx, SWITCH_HSF_CENV, SWITCH_HSF_CENV_SIZE) ||
        section->ofs > 0xFFFFFFFFU - section->count * SWITCH_HSF_CENV_SIZE) {
        return FALSE;
    }
    dataBase = section->ofs + section->count * SWITCH_HSF_CENV_SIZE;
    if (!SwitchHsfRange(ctx, dataBase, 0)) {
        return FALSE;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs +
                        i * SWITCH_HSF_CENV_SIZE;
        u32 singleCount = SwitchHsfBE32(raw + 0x10);
        u32 dualCount = SwitchHsfBE32(raw + 0x14);
        u32 multiCount = SwitchHsfBE32(raw + 0x18);
        if (!SwitchHsfCountOK(singleCount) || !SwitchHsfCountOK(dualCount) ||
            !SwitchHsfCountOK(multiCount) ||
            !SwitchHsfEstimateAdd(&descriptors, singleCount,
                                   SWITCH_HSF_CENV_SINGLE_SIZE) ||
            !SwitchHsfEstimateAdd(&descriptors, dualCount,
                                   SWITCH_HSF_CENV_DUAL_SIZE) ||
            !SwitchHsfEstimateAdd(&descriptors, multiCount,
                                   SWITCH_HSF_CENV_MULTI_SIZE)) {
            return FALSE;
        }
    }
    if (!SwitchHsfRelativeRange(ctx, dataBase, 0, descriptors)) {
        return FALSE;
    }
    if (dataBase > 0xFFFFFFFFU - descriptors) {
        return FALSE;
    }
    *descriptorBytes = descriptors;
    *weightBase = dataBase + descriptors;
    return TRUE;
}

static BOOL SwitchHsfCenvPointerRange(const SwitchHsfContext *ctx, u32 base,
                                      u32 offset, u32 count, u32 elementSize) {
    if (count == 0) {
        return TRUE;
    }
    if (elementSize != 0 && count > 0x3FFFFFFFU / elementSize) {
        return FALSE;
    }
    return SwitchHsfRelativeRange(ctx, base, offset, count * elementSize);
}

static HSFCENV *SwitchHsfParseCenv(SwitchHsfContext *ctx) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_CENV];
    HSFCENV *cenv;
    u32 descriptorBytes;
    u32 dataBase;
    u32 weightBase;
    u32 i;

    if (section->count == 0 || !SwitchHsfCenvDescriptorSizes(
            ctx, &descriptorBytes, &weightBase)) {
        return NULL;
    }
    dataBase = section->ofs + section->count * SWITCH_HSF_CENV_SIZE;
    cenv = (HSFCENV *)SwitchHsfArenaAlloc(
        &ctx->arena, section->count * sizeof(HSFCENV));
    if (!cenv) {
        return NULL;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs +
                        i * SWITCH_HSF_CENV_SIZE;
        HSFCENV *dst = &cenv[i];
        u32 singleCount = SwitchHsfBE32(raw + 0x10);
        u32 dualCount = SwitchHsfBE32(raw + 0x14);
        u32 multiCount = SwitchHsfBE32(raw + 0x18);
        u32 singleOfs = SwitchHsfBE32(raw + 4);
        u32 dualOfs = SwitchHsfBE32(raw + 8);
        u32 multiOfs = SwitchHsfBE32(raw + 0x0C);
        u32 j;

        memset(dst, 0, sizeof(*dst));
        dst->name = SwitchHsfString(ctx, SwitchHsfBE32(raw));
        dst->singleCount = singleCount;
        dst->dualCount = dualCount;
        dst->multiCount = multiCount;
        dst->vtxCount = SwitchHsfBE32(raw + 0x1C);
        dst->copyCount = SwitchHsfBE32(raw + 0x20);

        if (singleCount != 0) {
            if (!SwitchHsfCenvPointerRange(ctx, dataBase, singleOfs,
                                           singleCount,
                                           SWITCH_HSF_CENV_SINGLE_SIZE)) {
                return NULL;
            }
            dst->singleData = (HSFCENVSINGLE *)SwitchHsfArenaAlloc(
                &ctx->arena, singleCount * sizeof(HSFCENVSINGLE));
            if (!dst->singleData) {
                return NULL;
            }
            for (j = 0; j < singleCount; j++) {
                const u8 *src = ctx->raw + dataBase + singleOfs +
                                j * SWITCH_HSF_CENV_SINGLE_SIZE;
                dst->singleData[j].target = SwitchHsfBE32(src);
                dst->singleData[j].pos = SwitchHsfBE16(src + 4);
                dst->singleData[j].posNum = SwitchHsfBE16(src + 6);
                dst->singleData[j].normal = SwitchHsfBE16(src + 8);
                dst->singleData[j].normalNum = SwitchHsfBE16(src + 10);
            }
        }

        if (dualCount != 0) {
            if (!SwitchHsfCenvPointerRange(ctx, dataBase, dualOfs,
                                           dualCount,
                                           SWITCH_HSF_CENV_DUAL_SIZE)) {
                return NULL;
            }
            dst->dualData = (HSFCENVDUAL *)SwitchHsfArenaAlloc(
                &ctx->arena, dualCount * sizeof(HSFCENVDUAL));
            if (!dst->dualData) {
                return NULL;
            }
            for (j = 0; j < dualCount; j++) {
                const u8 *src = ctx->raw + dataBase + dualOfs +
                                j * SWITCH_HSF_CENV_DUAL_SIZE;
                HSFCENVDUAL *dual = &dst->dualData[j];
                u32 weightCount = SwitchHsfBE32(src + 8);
                u32 weightOfs = SwitchHsfBE32(src + 0x0C);
                u32 k;
                dual->target1 = SwitchHsfBE32(src);
                dual->target2 = SwitchHsfBE32(src + 4);
                dual->weightNum = weightCount;
                dual->weight = NULL;
                if (!SwitchHsfCountOK(weightCount) ||
                    !SwitchHsfCenvPointerRange(
                        ctx, weightBase, weightOfs, weightCount,
                        SWITCH_HSF_CENV_DUAL_WEIGHT_SIZE)) {
                    return NULL;
                }
                if (weightCount != 0) {
                    dual->weight = (HSFCENVDUALWEIGHT *)SwitchHsfArenaAlloc(
                        &ctx->arena,
                        weightCount * sizeof(HSFCENVDUALWEIGHT));
                    if (!dual->weight) {
                        return NULL;
                    }
                    for (k = 0; k < weightCount; k++) {
                        const u8 *weight = ctx->raw + weightBase + weightOfs +
                                           k * SWITCH_HSF_CENV_DUAL_WEIGHT_SIZE;
                        dual->weight[k].weight = SwitchHsfF32(weight);
                        dual->weight[k].pos = SwitchHsfBE16(weight + 4);
                        dual->weight[k].posNum = SwitchHsfBE16(weight + 6);
                        dual->weight[k].normal = SwitchHsfBE16(weight + 8);
                        dual->weight[k].normalNum = SwitchHsfBE16(weight + 10);
                    }
                }
            }
        }

        if (multiCount != 0) {
            if (!SwitchHsfCenvPointerRange(ctx, dataBase, multiOfs,
                                           multiCount,
                                           SWITCH_HSF_CENV_MULTI_SIZE)) {
                return NULL;
            }
            dst->multiData = (HSFCENVMULTI *)SwitchHsfArenaAlloc(
                &ctx->arena, multiCount * sizeof(HSFCENVMULTI));
            if (!dst->multiData) {
                return NULL;
            }
            for (j = 0; j < multiCount; j++) {
                const u8 *src = ctx->raw + dataBase + multiOfs +
                                j * SWITCH_HSF_CENV_MULTI_SIZE;
                HSFCENVMULTI *multi = &dst->multiData[j];
                u32 weightCount = SwitchHsfBE32(src);
                u32 weightOfs = SwitchHsfBE32(src + 0x0C);
                u32 k;
                multi->weightNum = weightCount;
                multi->pos = SwitchHsfBE16(src + 4);
                multi->posNum = SwitchHsfBE16(src + 6);
                multi->normal = SwitchHsfBE16(src + 8);
                multi->normalNum = SwitchHsfBE16(src + 10);
                multi->weight = NULL;
                if (!SwitchHsfCountOK(weightCount) ||
                    !SwitchHsfCenvPointerRange(
                        ctx, weightBase, weightOfs, weightCount,
                        SWITCH_HSF_CENV_MULTI_WEIGHT_SIZE)) {
                    return NULL;
                }
                if (weightCount != 0) {
                    multi->weight = (HSFCENVMULTIWEIGHT *)SwitchHsfArenaAlloc(
                        &ctx->arena,
                        weightCount * sizeof(HSFCENVMULTIWEIGHT));
                    if (!multi->weight) {
                        return NULL;
                    }
                    for (k = 0; k < weightCount; k++) {
                        const u8 *weight = ctx->raw + weightBase + weightOfs +
                                           k * SWITCH_HSF_CENV_MULTI_WEIGHT_SIZE;
                        multi->weight[k].target = SwitchHsfBE32(weight);
                        multi->weight[k].value = SwitchHsfF32(weight + 4);
                    }
                }
            }
        }
    }
    (void)descriptorBytes;
    return cenv;
}

/* Part/cluster/shape records contain PPC pointers in the source file.  The
 * records are kept in their original compact sizes here and all pointer
 * fields are rebuilt into the native arena. */
static HSFPART *SwitchHsfParseParts(SwitchHsfContext *ctx) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_PART];
    HSFPART *parts;
    u32 dataBase;
    u32 i;

    if (section->count == 0) {
        return NULL;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_PART, SWITCH_HSF_PART_SIZE) ||
        section->count > 0xFFFFFFFFU / SWITCH_HSF_PART_SIZE) {
        return NULL;
    }
    dataBase = section->ofs + section->count * SWITCH_HSF_PART_SIZE;
    if (dataBase < section->ofs || !SwitchHsfRange(ctx, dataBase, 0)) {
        return NULL;
    }
    parts = (HSFPART *)SwitchHsfArenaAlloc(&ctx->arena,
                                           section->count * sizeof(HSFPART));
    if (!parts) {
        return NULL;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs + i * SWITCH_HSF_PART_SIZE;
        HSFPART *part = &parts[i];
        u32 count = SwitchHsfBE32(raw + 4);
        u32 vertexOfs = SwitchHsfBE32(raw + 8);
        u32 j;

        if (count > SWITCH_HSF_MAX_COUNT ||
            vertexOfs > (ctx->rawSize - dataBase) / sizeof(u16) ||
            !SwitchHsfRange(ctx, dataBase + vertexOfs * sizeof(u16),
                            count * sizeof(u16))) {
            return NULL;
        }
        part->name = SwitchHsfString(ctx, SwitchHsfBE32(raw));
        part->num = count;
        part->vertex = (u16 *)SwitchHsfArenaAlloc(
            &ctx->arena, count ? count * sizeof(u16) : sizeof(u16));
        if (!part->vertex) {
            return NULL;
        }
        for (j = 0; j < count; j++) {
            part->vertex[j] = SwitchHsfBE16(
                ctx->raw + dataBase + vertexOfs * sizeof(u16) + j * sizeof(u16));
        }
    }
    return parts;
}

static HSFCLUSTER *SwitchHsfParseClusters(SwitchHsfContext *ctx) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_CLUSTER];
    HSFCLUSTER *clusters;
    u32 i;

    if (section->count == 0) {
        return NULL;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_CLUSTER,
                             SWITCH_HSF_CLUSTER_SIZE)) {
        return NULL;
    }
    clusters = (HSFCLUSTER *)SwitchHsfArenaAlloc(
        &ctx->arena, section->count * sizeof(HSFCLUSTER));
    if (!clusters) {
        return NULL;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs + i * SWITCH_HSF_CLUSTER_SIZE;
        HSFCLUSTER *cluster = &clusters[i];
        u32 vertexNum = SwitchHsfBE32(raw + 0x98);
        u32 vertexSymbol = SwitchHsfBE32(raw + 0x9C);
        u32 j;

        if (vertexNum > SWITCH_HSF_MAX_CHILDREN) {
            return NULL;
        }
        memset(cluster, 0, sizeof(*cluster));
        cluster->name[0] = SwitchHsfString(ctx, SwitchHsfBE32(raw + 0));
        cluster->name[1] = SwitchHsfString(ctx, SwitchHsfBE32(raw + 4));
        cluster->targetName = SwitchHsfString(ctx, SwitchHsfBE32(raw + 8));
        cluster->target = -1;
        if (ctx->part) {
            u32 partIndex = SwitchHsfBE32(raw + 0x0C);
            if (partIndex != 0xFFFFFFFFU &&
                partIndex < ctx->header.section[SWITCH_HSF_PART].count) {
                cluster->part = &ctx->part[partIndex];
            }
        }
        cluster->index = SwitchHsfF32(raw + 0x10);
        for (j = 0; j < 32; j++) {
            cluster->weight[j] = SwitchHsfF32(raw + 0x14 + j * 4);
        }
        cluster->adjusted = raw[0x94];
        cluster->unk95 = raw[0x95];
        cluster->type = SwitchHsfBE16(raw + 0x96);
        cluster->vertexNum = vertexNum;
        if (vertexNum != 0) {
            cluster->vertex = (HSFBUFFER **)SwitchHsfArenaAlloc(
                &ctx->arena, vertexNum * sizeof(HSFBUFFER *));
            if (!cluster->vertex) {
                return NULL;
            }
            for (j = 0; j < vertexNum; j++) {
                u32 vertexIndex;
                if (!SwitchHsfSymbol(ctx, vertexSymbol + j, &vertexIndex) ||
                    vertexIndex >= ctx->header.section[SWITCH_HSF_VERTEX].count) {
                    return NULL;
                }
                cluster->vertex[j] = &ctx->vertex[vertexIndex];
            }
        }
    }
    return clusters;
}

static HSFSHAPE *SwitchHsfParseShapes(SwitchHsfContext *ctx) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_SHAPE];
    HSFSHAPE *shapes;
    u32 i;

    if (section->count == 0) {
        return NULL;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_SHAPE, SWITCH_HSF_SHAPE_SIZE)) {
        return NULL;
    }
    shapes = (HSFSHAPE *)SwitchHsfArenaAlloc(
        &ctx->arena, section->count * sizeof(HSFSHAPE));
    if (!shapes) {
        return NULL;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs + i * SWITCH_HSF_SHAPE_SIZE;
        HSFSHAPE *shape = &shapes[i];
        u32 vertexNum = SwitchHsfBE16(raw + 6);
        u32 vertexSymbol = SwitchHsfBE32(raw + 8);
        u32 j;

        if (vertexNum > SWITCH_HSF_MAX_CHILDREN) {
            return NULL;
        }
        memset(shape, 0, sizeof(*shape));
        shape->name = SwitchHsfString(ctx, SwitchHsfBE32(raw));
        shape->num16[0] = SwitchHsfBE16(raw + 4);
        shape->num16[1] = (u16)vertexNum;
        if (vertexNum != 0) {
            shape->vertex = (HSFBUFFER **)SwitchHsfArenaAlloc(
                &ctx->arena, vertexNum * sizeof(HSFBUFFER *));
            if (!shape->vertex) {
                return NULL;
            }
            for (j = 0; j < vertexNum; j++) {
                u32 vertexIndex;
                if (!SwitchHsfSymbol(ctx, vertexSymbol + j, &vertexIndex) ||
                    vertexIndex >= ctx->header.section[SWITCH_HSF_VERTEX].count) {
                    return NULL;
                }
                shape->vertex[j] = &ctx->vertex[vertexIndex];
            }
        }
    }
    return shapes;
}

static HSFOBJECT *SwitchHsfParseObjects(SwitchHsfContext *ctx, HSFDATA *model) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_OBJECT];
    HSFOBJECT *objects;
    u32 i;
    s32 rootIndex = -1;

    if (section->count == 0) {
        return NULL;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_OBJECT, SWITCH_HSF_OBJECT_SIZE)) {
        return NULL;
    }
    objects = (HSFOBJECT *)SwitchHsfArenaAlloc(
        &ctx->arena, section->count * sizeof(HSFOBJECT));
    if (!objects) {
        return NULL;
    }
    memset(objects, 0, section->count * sizeof(HSFOBJECT));

    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs + i * SWITCH_HSF_OBJECT_SIZE;
        HSFOBJECT *object = &objects[i];
        u32 parent;
        u32 childCount = SwitchHsfBE32(raw + 0x14);
        u32 childSymbol = SwitchHsfBE32(raw + 0x18);
        u32 index;

        object->name = SwitchHsfString(ctx, SwitchHsfBE32(raw + 0));
        object->type = SwitchHsfBE32(raw + 4);
        object->flags = SwitchHsfBE32(raw + 0x0C);
        object->constData = SwitchHsfMakeConst(ctx);
        if (!object->constData) {
            return NULL;
        }
        if (SwitchHsfS32(raw + 0x10) < 0) {
            rootIndex = (s32)i;
            parent = 0xFFFFFFFFU;
        } else {
            parent = SwitchHsfBE32(raw + 0x10);
        }
        if (parent != 0xFFFFFFFFU && parent < section->count) {
            object->mesh.parent = &objects[parent];
        }

        /* Skeleton records carry the bind pose for joints and null objects.
         * Copy the raw object transforms first; the skeleton pass below can
         * replace them where a matching bind-pose name exists. */
        SwitchHsfCopyTransform(&object->mesh.base, raw + 0x1C);
        SwitchHsfCopyTransform(&object->mesh.curr, raw + 0x40);

        if (object->type == HSF_OBJ_CAMERA) {
            float rawFov;
            float rawNear;
            float rawFar;

            object->camera.pos.x = SwitchHsfF32(raw + 0x10);
            object->camera.pos.y = SwitchHsfF32(raw + 0x14);
            object->camera.pos.z = SwitchHsfF32(raw + 0x18);
            object->camera.target.x = SwitchHsfF32(raw + 0x1C);
            object->camera.target.y = SwitchHsfF32(raw + 0x20);
            object->camera.target.z = SwitchHsfF32(raw + 0x24);
            object->camera.upRot = SwitchHsfF32(raw + 0x28);
            rawFov = SwitchHsfF32(raw + 0x2C);
            rawNear = SwitchHsfF32(raw + 0x30);
            rawFar = SwitchHsfF32(raw + 0x34);

            /* Some title HSF camera records leave these optional fields as
             * 0xCCCCCCCC.  Keep the valid position/target, but use the same
             * safe perspective as the boot camera when those fields are bad. */
            if (object->camera.upRot != object->camera.upRot ||
                object->camera.upRot < -360.0f ||
                object->camera.upRot > 360.0f) {
                object->camera.upRot = 0.0f;
            }
            object->camera.fov = rawFov;
            if (rawFov != rawFov || rawFov <= 1.0f || rawFov >= 179.0f) {
                object->camera.fov = 30.0f;
            }
            object->camera.near = rawNear;
            if (rawNear != rawNear || rawNear <= 0.01f) {
                object->camera.near = 20.0f;
            }
            object->camera.far = rawFar;
            if (rawFar != rawFar || rawFar <= object->camera.near) {
                object->camera.far = 15000.0f;
            }
            object->type = HSF_OBJ_CAMERA;
            continue;
        }
        if (object->type == 8) {
            object->type = HSF_OBJ_LIGHT;
            continue;
        }

        object->mesh.mesh.min.x = SwitchHsfF32(raw + 0x64);
        object->mesh.mesh.min.y = SwitchHsfF32(raw + 0x68);
        object->mesh.mesh.min.z = SwitchHsfF32(raw + 0x6C);
        object->mesh.mesh.max.x = SwitchHsfF32(raw + 0x70);
        object->mesh.mesh.max.y = SwitchHsfF32(raw + 0x74);
        object->mesh.mesh.max.z = SwitchHsfF32(raw + 0x78);
        object->mesh.mesh.baseMorph = SwitchHsfF32(raw + 0x7C);
        for (index = 0; index < 33; index++) {
            object->mesh.mesh.morphWeight[index] = SwitchHsfF32(raw + 0x80 + index * 4);
        }
        object->mesh.face = SwitchHsfMeshIndex(SwitchHsfBE32(raw + 0x104),
                                               ctx->header.section[SWITCH_HSF_FACE].count,
                                               &index) ? &ctx->face[index] : NULL;
        object->mesh.vertex = SwitchHsfMeshIndex(SwitchHsfBE32(raw + 0x108),
                                                 ctx->header.section[SWITCH_HSF_VERTEX].count,
                                                 &index) ? &ctx->vertex[index] : NULL;
        object->mesh.normal = SwitchHsfMeshIndex(SwitchHsfBE32(raw + 0x10C),
                                                 ctx->header.section[SWITCH_HSF_NORMAL].count,
                                                 &index) ? &ctx->normal[index] : NULL;
        object->mesh.color = SwitchHsfMeshIndex(SwitchHsfBE32(raw + 0x110),
                                                ctx->header.section[SWITCH_HSF_COLOR].count,
                                                &index) ? &ctx->color[index] : NULL;
        object->mesh.st = SwitchHsfMeshIndex(SwitchHsfBE32(raw + 0x114),
                                             ctx->header.section[SWITCH_HSF_ST].count,
                                             &index) ? &ctx->st[index] : NULL;
        object->mesh.material = ctx->material;
        object->mesh.attribute = ctx->attribute;
        object->mesh.writeNum = raw[0x120];
        object->mesh.unk121 = raw[0x121];
        object->mesh.shapeType = raw[0x122];
        object->mesh.matPass = raw[0x123];
        object->mesh.shapeNum = SwitchHsfBE32(raw + 0x124);
        object->mesh.shape = NULL;
        object->mesh.clusterNum = SwitchHsfBE32(raw + 0x12C);
        object->mesh.cluster = NULL;
        object->mesh.cenvNum = 0;
        object->mesh.cenv = NULL;
        object->mesh.file[0] = NULL;
        object->mesh.file[1] = NULL;

        if (object->mesh.shapeNum > SWITCH_HSF_MAX_CHILDREN ||
            object->mesh.clusterNum > SWITCH_HSF_MAX_CHILDREN) {
            return NULL;
        }
        if (object->mesh.shapeNum != 0) {
            u32 shapeSymbol = SwitchHsfBE32(raw + 0x128);
            object->mesh.shape = (HSFBUFFER **)SwitchHsfArenaAlloc(
                &ctx->arena, object->mesh.shapeNum * sizeof(HSFBUFFER *));
            if (!object->mesh.shape) {
                return NULL;
            }
            for (index = 0; index < object->mesh.shapeNum; index++) {
                u32 vertexIndex;
                if (!SwitchHsfSymbol(ctx, shapeSymbol + index, &vertexIndex) ||
                    vertexIndex >= ctx->header.section[SWITCH_HSF_VERTEX].count) {
                    return NULL;
                }
                object->mesh.shape[index] = &ctx->vertex[vertexIndex];
            }
        }
        if (object->mesh.clusterNum != 0) {
            u32 clusterSymbol = SwitchHsfBE32(raw + 0x130);
            if (!ctx->cluster) {
                return NULL;
            }
            object->mesh.cluster = (HSFCLUSTER **)SwitchHsfArenaAlloc(
                &ctx->arena, object->mesh.clusterNum * sizeof(HSFCLUSTER *));
            if (!object->mesh.cluster) {
                return NULL;
            }
            for (index = 0; index < object->mesh.clusterNum; index++) {
                u32 clusterIndex;
                if (!SwitchHsfSymbol(ctx, clusterSymbol + index,
                                     &clusterIndex) ||
                    clusterIndex >= ctx->header.section[SWITCH_HSF_CLUSTER].count) {
                    return NULL;
                }
                object->mesh.cluster[index] = &ctx->cluster[clusterIndex];
            }
        }

        if (ctx->cenv) {
            u32 cenvIndex = SwitchHsfBE32(raw + 0x138);
            u32 cenvCount = SwitchHsfBE32(raw + 0x134);
            if (cenvCount != 0 && cenvIndex != 0xFFFFFFFFU &&
                cenvIndex < ctx->header.section[SWITCH_HSF_CENV].count &&
                cenvCount <= ctx->header.section[SWITCH_HSF_CENV].count -
                              cenvIndex) {
                object->mesh.cenvNum = cenvCount;
                object->mesh.cenv = &ctx->cenv[cenvIndex];
                if (object->mesh.vertex && object->mesh.vertex->count > 0) {
                    u32 bytes = (u32)object->mesh.vertex->count * sizeof(HuVecF);
                    object->mesh.file[0] = SwitchHsfArenaAlloc(&ctx->arena,
                                                                bytes);
                    if (!object->mesh.file[0]) {
                        return NULL;
                    }
                    memcpy(object->mesh.file[0], object->mesh.vertex->data,
                           bytes);
                }
                if (object->mesh.normal && object->mesh.normal->count > 0) {
                    u32 bytes = (u32)object->mesh.normal->count * sizeof(Vec);
                    object->mesh.file[1] = SwitchHsfArenaAlloc(&ctx->arena,
                                                                bytes);
                    if (!object->mesh.file[1]) {
                        return NULL;
                    }
                    memcpy(object->mesh.file[1], object->mesh.normal->data,
                           bytes);
                }
            }
        }

        if (childCount > SWITCH_HSF_MAX_CHILDREN) {
            return NULL;
        }
        object->mesh.childrenCount = childCount;
        if (childCount != 0) {
            u32 child;
            object->mesh.children = (HSFOBJECT **)SwitchHsfArenaAlloc(
                &ctx->arena, childCount * sizeof(HSFOBJECT *));
            if (!object->mesh.children) {
                return NULL;
            }
            for (child = 0; child < childCount; child++) {
                u32 childIndex;
                if (!SwitchHsfSymbol(ctx, childSymbol + child, &childIndex) ||
                    childIndex >= section->count) {
                    return NULL;
                }
                object->mesh.children[child] = &objects[childIndex];
            }
        }
    }
    if (rootIndex < 0) {
        rootIndex = 0;
    }
    model->root = &objects[rootIndex];

    if (ctx->skeleton) {
        for (i = 0; i < section->count; i++) {
            u32 j;
            if (!objects[i].name) {
                continue;
            }
            for (j = 0; j < ctx->header.section[SWITCH_HSF_SKELETON].count;
                 j++) {
                if (ctx->skeleton[j].name &&
                    strcmp(objects[i].name, ctx->skeleton[j].name) == 0) {
                    objects[i].mesh.base = ctx->skeleton[j].transform;
                    objects[i].mesh.curr = ctx->skeleton[j].transform;
                    break;
                }
            }
        }
    }
    return objects;
}

static s32 SwitchHsfFindObject(const HSFDATA *model, const char *name) {
    s32 i;
    if (!model || !model->object || !name) {
        return -1;
    }
    for (i = 0; i < model->objectNum; i++) {
        if (model->object[i].name && strcmp(model->object[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void SwitchHsfResolveReferences(HSFDATA *model, u32 stringSize) {
    s32 i;
    if (!model) {
        return;
    }
    for (i = 0; i < model->clusterNum; i++) {
        HSFCLUSTER *cluster = &model->cluster[i];
        cluster->target = SwitchHsfFindObject(model, cluster->targetName);
    }
    if (!model->motion || !model->motion->track) {
        return;
    }
    for (i = 0; i < model->motion->numTracks; i++) {
        HSFTRACK *track = &model->motion->track[i];
        const char *name = NULL;
        if (model->motion->name && track->target < stringSize) {
            const char *candidate = model->motion->name + track->target;
            size_t nameBytes = stringSize - track->target;
            if (nameBytes > 256) {
                nameBytes = 256;
            }
            if (memchr(candidate, '\0', nameBytes) != NULL) {
                name = candidate;
            }
        }
        s32 resolved = -1;
        s32 j;
        if (!name) {
            track->target = 0xFFFF;
            continue;
        }
        if (track->type == HSF_TRACK_TRANSFORM ||
            track->type == HSF_TRACK_MORPH) {
            resolved = SwitchHsfFindObject(model, name);
        } else if (track->type == HSF_TRACK_CLUSTER ||
                   track->type == HSF_TRACK_CLUSTER_WEIGHT) {
            for (j = 0; j < model->clusterNum; j++) {
                if (model->cluster[j].name[0] &&
                    strcmp(model->cluster[j].name[0], name) == 0) {
                    resolved = j;
                    break;
                }
            }
        } else if (track->type == HSF_TRACK_ATTRIBUTE) {
            for (j = 0; j < model->attributeNum; j++) {
                if (model->attribute[j].name &&
                    strcmp(model->attribute[j].name, name) == 0) {
                    resolved = j;
                    break;
                }
            }
        } else if (track->type == HSF_TRACK_MATERIAL) {
            for (j = 0; j < model->materialNum; j++) {
                if (model->material[j].name &&
                    strcmp(model->material[j].name, name) == 0) {
                    resolved = j;
                    break;
                }
            }
        }
        track->target = resolved < 0 ? 0xFFFF : (u16)resolved;
    }
}

/*
 * Motion records use the same 16-byte PPC layout as HSFTRACK.  The file
 * stores one motion record followed by its tracks and curve data.  Only the
 * transform tracks are copied here; envelope, cluster, material, and bitmap
 * animation still need their own validated native representations.
 */
static HSFMOTION *SwitchHsfParseMotion(SwitchHsfContext *ctx) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_MOTION];
    const u8 *rawMotion;
    HSFMOTION *motion;
    HSFTRACK *tracks;
    u32 trackBase;
    u32 dataBase;
    u32 trackCount;
    u32 i;

    if (section->count == 0) {
        return NULL;
    }
    if (section->count != 1 ||
        !SwitchHsfTableRange(ctx, SWITCH_HSF_MOTION, SWITCH_HSF_MOTION_SIZE)) {
        OSReport("HSF Switch: motion table variant skipped\n");
        return NULL;
    }
    rawMotion = ctx->raw + section->ofs;
    trackCount = SwitchHsfBE32(rawMotion + 4);
    if (!SwitchHsfCountOK(trackCount) ||
        trackCount > 0xFFFFFFFFU / SWITCH_HSF_TRACK_SIZE ||
        section->ofs > 0xFFFFFFFFU - SWITCH_HSF_MOTION_SIZE) {
        return NULL;
    }
    trackBase = section->ofs + SWITCH_HSF_MOTION_SIZE;
    if (!SwitchHsfRange(ctx, trackBase,
                        trackCount * SWITCH_HSF_TRACK_SIZE) ||
        trackBase > 0xFFFFFFFFU - trackCount * SWITCH_HSF_TRACK_SIZE) {
        return NULL;
    }
    dataBase = trackBase + trackCount * SWITCH_HSF_TRACK_SIZE;
    if (!SwitchHsfRange(ctx, dataBase, 0)) {
        return NULL;
    }

    motion = (HSFMOTION *)SwitchHsfArenaAlloc(&ctx->arena, sizeof(*motion));
    tracks = (HSFTRACK *)SwitchHsfArenaAlloc(
        &ctx->arena, trackCount * sizeof(*tracks));
    if (!motion || (trackCount != 0 && !tracks)) {
        return NULL;
    }
    motion->name = ctx->strings;
    motion->numTracks = (s32)trackCount;
    motion->track = tracks;
    motion->maxTime = SwitchHsfF32(rawMotion + 12);

    for (i = 0; i < trackCount; i++) {
        const u8 *raw = ctx->raw + trackBase + i * SWITCH_HSF_TRACK_SIZE;
        HSFTRACK *track = &tracks[i];
        u32 curve = SwitchHsfBE16(raw + 8);
        u32 keyframes = SwitchHsfBE16(raw + 10);
        u32 dataOfs = SwitchHsfBE32(raw + 12);
        u32 valueCount;
        u32 j;

        memset(track, 0, sizeof(*track));
        track->type = raw[0];
        track->start = raw[1];
        track->target = SwitchHsfBE16(raw + 2);
        track->clusterWeight = SwitchHsfS32(raw + 4);
        track->curveType = (u16)curve;
        track->numKeyframes = (u16)keyframes;

        if (track->type == HSF_TRACK_TRANSFORM) {
            track->channel = SwitchHsfBE16(raw + 6);
        } else if (track->type == HSF_TRACK_MORPH) {
            track->morphWeight = SwitchHsfS16(raw + 6);
        } else {
            track->attrIdx = SwitchHsfS16(raw + 4);
        }

        if (curve == HSF_CURVE_CONST) {
            track->value = SwitchHsfF32(raw + 12);
            continue;
        }
        if ((track->type != HSF_TRACK_TRANSFORM &&
             track->type != HSF_TRACK_MORPH &&
             track->type != HSF_TRACK_CLUSTER &&
             track->type != HSF_TRACK_CLUSTER_WEIGHT &&
             track->type != HSF_TRACK_MATERIAL &&
             track->type != HSF_TRACK_ATTRIBUTE) ||
            (curve != HSF_CURVE_STEP && curve != HSF_CURVE_LINEAR &&
             curve != HSF_CURVE_BEZIER) || keyframes == 0) {
            continue;
        }
        valueCount = keyframes * (curve == HSF_CURVE_BEZIER ? 4U : 2U);
        if (valueCount > 0x3FFFFFFFU / sizeof(float) ||
            dataOfs > ctx->rawSize - dataBase ||
            !SwitchHsfRange(ctx, dataBase + dataOfs,
                            valueCount * sizeof(float))) {
            return NULL;
        }
        track->data = SwitchHsfArenaAlloc(&ctx->arena,
                                          valueCount * sizeof(float));
        if (!track->data) {
            return NULL;
        }
        for (j = 0; j < valueCount; j++) {
            ((float *)track->data)[j] = SwitchHsfF32(
                ctx->raw + dataBase + dataOfs + j * sizeof(float));
        }
    }
    return motion;
}

const char *SwitchHsfMotionTargetName(const HSFMOTION *motion, u16 offset) {
    if (!motion || !motion->name || offset == 0xFFFF) {
        return NULL;
    }
    return motion->name + offset;
}

static u32 SwitchHsfRawSize(void *data) {
    uintptr_t address = (uintptr_t)data;
    s32 i;
    for (i = 0; i < HEAP_MAX; i++) {
        uintptr_t start = (uintptr_t)HuMemHeapPtrGet((HeapID)i);
        uintptr_t end = start + HuMemHeapSizeGet((HeapID)i);
        if (start != 0 && address >= start && address < end) {
            return (u32)HuMemMemorySizeGet(data);
        }
    }
    return 0;
}

static void SwitchModelRawFree(void *data) {
    uintptr_t address;
    s32 i;
    if (!data) {
        return;
    }
    address = (uintptr_t)data;
    for (i = 0; i < HEAP_MAX; i++) {
        uintptr_t start = (uintptr_t)HuMemHeapPtrGet((HeapID)i);
        uintptr_t end = start + HuMemHeapSizeGet((HeapID)i);
        if (start != 0 && address >= start && address < end) {
            HuMemDirectFree(data);
            return;
        }
    }
}

static BOOL SwitchHsfEstimateAdd(u32 *total, u32 count, u32 size) {
    u32 bytes;
    if (!total || (size != 0 && count > 0xFFFFFFFFU / size)) {
        return FALSE;
    }
    bytes = count * size;
    if (bytes > 0xFFFFFFFFU - *total) {
        return FALSE;
    }
    *total += bytes;
    return TRUE;
}

static BOOL SwitchHsfEstimateMaterials(const SwitchHsfContext *ctx, u32 *total) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_MATERIAL];
    u32 i;
    if (section->count == 0) {
        return TRUE;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_MATERIAL, SWITCH_HSF_MATERIAL_SIZE) ||
        !SwitchHsfEstimateAdd(total, section->count, sizeof(HSFMATERIAL))) {
        return FALSE;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs + i * SWITCH_HSF_MATERIAL_SIZE;
        u32 attrNum = SwitchHsfBE32(raw + 0x34);
        u32 attrSymbol = SwitchHsfBE32(raw + 0x38);
        if (attrNum > 256 ||
            (attrNum != 0 &&
             (attrSymbol > ctx->header.section[SWITCH_HSF_SYMBOL].count ||
              attrNum > ctx->header.section[SWITCH_HSF_SYMBOL].count - attrSymbol)) ||
            !SwitchHsfEstimateAdd(total, attrNum, sizeof(s32))) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL SwitchHsfEstimatePalettes(const SwitchHsfContext *ctx, u32 *total) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_PALETTE];
    u32 tableSize;
    u32 dataBase;
    u32 i;
    if (section->count == 0) {
        return TRUE;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_PALETTE, SWITCH_HSF_PALETTE_SIZE) ||
        section->count > 0xFFFFFFFFU / SWITCH_HSF_PALETTE_SIZE) {
        return FALSE;
    }
    tableSize = section->count * SWITCH_HSF_PALETTE_SIZE;
    if (section->ofs > 0xFFFFFFFFU - tableSize) {
        return FALSE;
    }
    dataBase = section->ofs + tableSize;
    if (!SwitchHsfRange(ctx, dataBase, 0) ||
        !SwitchHsfEstimateAdd(total, section->count, sizeof(HSFPALETTE))) {
        return FALSE;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs + i * SWITCH_HSF_PALETTE_SIZE;
        u32 entries = SwitchHsfBE32(raw + 8);
        u32 dataOfs = SwitchHsfBE32(raw + 12);
        u32 bytes;
        if (entries > 0x10000U || entries > 0xFFFFFFFFU / 2U) {
            return FALSE;
        }
        bytes = entries * 2;
        if (dataOfs > ctx->rawSize - dataBase ||
            !SwitchHsfRange(ctx, dataBase + dataOfs, bytes) ||
            !SwitchHsfEstimateAdd(total, 1, bytes ? bytes : 1)) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL SwitchHsfEstimateBitmaps(const SwitchHsfContext *ctx, u32 *total) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_BITMAP];
    u32 tableSize;
    u32 dataBase;
    u32 i;
    if (section->count == 0) {
        return TRUE;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_BITMAP, SWITCH_HSF_BITMAP_SIZE) ||
        section->count > 0xFFFFFFFFU / SWITCH_HSF_BITMAP_SIZE) {
        return FALSE;
    }
    tableSize = section->count * SWITCH_HSF_BITMAP_SIZE;
    if (section->ofs > 0xFFFFFFFFU - tableSize) {
        return FALSE;
    }
    dataBase = section->ofs + tableSize;
    if (!SwitchHsfRange(ctx, dataBase, 0) ||
        !SwitchHsfEstimateAdd(total, section->count, sizeof(HSFBITMAP))) {
        return FALSE;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs + i * SWITCH_HSF_BITMAP_SIZE;
        u32 bytes = SwitchHsfBitmapBytes(raw[8], raw[9],
                                         SwitchHsfS16(raw + 10) < 0 ? 0 : SwitchHsfS16(raw + 10),
                                         SwitchHsfS16(raw + 12) < 0 ? 0 : SwitchHsfS16(raw + 12));
        u32 dataOfs = SwitchHsfBE32(raw + 0x1C);
        if (bytes != 0 && (dataOfs > ctx->rawSize - dataBase ||
                           !SwitchHsfRange(ctx, dataBase + dataOfs, bytes) ||
                           !SwitchHsfEstimateAdd(total, 1, bytes))) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL SwitchHsfEstimateBuffers(const SwitchHsfContext *ctx, s32 sectionId,
                                     u32 elementSize, u32 *total) {
    const SwitchHsfSection *section = &ctx->header.section[sectionId];
    u32 tableSize;
    u32 dataBase;
    u32 i;
    if (section->count == 0) {
        return TRUE;
    }
    if (!SwitchHsfTableRange(ctx, sectionId, SWITCH_HSF_BUFFER_SIZE)) {
        return FALSE;
    }
    tableSize = section->count * SWITCH_HSF_BUFFER_SIZE;
    dataBase = section->ofs + tableSize;
    if (!SwitchHsfRange(ctx, dataBase, 0) ||
        !SwitchHsfEstimateAdd(total, section->count, sizeof(HSFBUFFER))) {
        return FALSE;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *record = ctx->raw + section->ofs + i * SWITCH_HSF_BUFFER_SIZE;
        u32 count = SwitchHsfBE32(record + 4);
        u32 dataOfs = SwitchHsfBE32(record + 8);
        u32 dataBytes;
        if (elementSize != 0 && count > 0x3FFFFFFFU / elementSize) {
            return FALSE;
        }
        dataBytes = count * elementSize;
        if (dataOfs > ctx->rawSize - dataBase ||
            !SwitchHsfRange(ctx, dataBase + dataOfs, dataBytes) ||
            !SwitchHsfEstimateAdd(total, 1, dataBytes ? dataBytes : 1)) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL SwitchHsfEstimateNormals(const SwitchHsfContext *ctx, u32 *total) {
    u32 stride = ctx->header.section[SWITCH_HSF_CENV].count ? 12 : 3;
    return SwitchHsfEstimateBuffers(ctx, SWITCH_HSF_NORMAL, stride, total);
}

static BOOL SwitchHsfEstimateFaces(const SwitchHsfContext *ctx, u32 *total) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_FACE];
    u32 tableSize;
    u32 dataBase;
    u32 i;
    if (section->count == 0) {
        return TRUE;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_FACE, SWITCH_HSF_BUFFER_SIZE)) {
        return FALSE;
    }
    tableSize = section->count * SWITCH_HSF_BUFFER_SIZE;
    dataBase = section->ofs + tableSize;
    if (!SwitchHsfRange(ctx, dataBase, 0) ||
        !SwitchHsfEstimateAdd(total, section->count, sizeof(HSFBUFFER))) {
        return FALSE;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *record = ctx->raw + section->ofs + i * SWITCH_HSF_BUFFER_SIZE;
        u32 count = SwitchHsfBE32(record + 4);
        u32 dataOfs = SwitchHsfBE32(record + 8);
        u32 rawBytes;
        if (count > 0x3FFFFFFFU / SWITCH_HSF_FACE_SIZE ||
            count > 0x3FFFFFFFU / sizeof(HSFFACE)) {
            return FALSE;
        }
        rawBytes = count * SWITCH_HSF_FACE_SIZE;
        if (dataOfs > ctx->rawSize - dataBase ||
            !SwitchHsfRange(ctx, dataBase + dataOfs, rawBytes) ||
            !SwitchHsfEstimateAdd(total, 1,
                                   count ? count * sizeof(HSFFACE) : 1)) {
            return FALSE;
        }
        for (u32 j = 0; j < count; j++) {
            const u8 *rawFace = ctx->raw + dataBase + dataOfs +
                                j * SWITCH_HSF_FACE_SIZE;
            if ((SwitchHsfBE16(rawFace) & HSF_FACE_MASK) == HSF_FACE_TRISTRIP) {
                u32 stripCount = SwitchHsfBE32(rawFace + 0x1C);
                u32 stripOfs = SwitchHsfBE32(rawFace + 0x20);
                u32 stripBytes;
                if (stripCount > SWITCH_HSF_MAX_COUNT ||
                    stripCount > 0x3FFFFFFFU / 8U ||
                    stripOfs > 0x3FFFFFFFU / 8U) {
                    return FALSE;
                }
                stripBytes = stripCount * 8U;
                if (stripOfs > (ctx->rawSize - dataBase) / 8U ||
                    !SwitchHsfRange(ctx, dataBase + stripOfs * 8U, stripBytes) ||
                    !SwitchHsfEstimateAdd(total, 1, stripBytes ? stripBytes : 1)) {
                    return FALSE;
                }
            }
        }
    }
    return TRUE;
}

static BOOL SwitchHsfEstimateMotion(const SwitchHsfContext *ctx, u32 *total) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_MOTION];
    const u8 *rawMotion;
    u32 trackCount;
    u32 trackBase;
    u32 dataBase;
    u32 i;

    if (section->count == 0) {
        return TRUE;
    }
    if (section->count != 1 ||
        !SwitchHsfTableRange(ctx, SWITCH_HSF_MOTION, SWITCH_HSF_MOTION_SIZE)) {
        return FALSE;
    }
    rawMotion = ctx->raw + section->ofs;
    trackCount = SwitchHsfBE32(rawMotion + 4);
    if (!SwitchHsfCountOK(trackCount) ||
        !SwitchHsfEstimateAdd(total, 1, sizeof(HSFMOTION)) ||
        !SwitchHsfEstimateAdd(total, trackCount, sizeof(HSFTRACK)) ||
        trackCount > 0xFFFFFFFFU / SWITCH_HSF_TRACK_SIZE) {
        return FALSE;
    }
    trackBase = section->ofs + SWITCH_HSF_MOTION_SIZE;
    if (!SwitchHsfRange(ctx, trackBase,
                        trackCount * SWITCH_HSF_TRACK_SIZE)) {
        return FALSE;
    }
    dataBase = trackBase + trackCount * SWITCH_HSF_TRACK_SIZE;
    if (!SwitchHsfRange(ctx, dataBase, 0)) {
        return FALSE;
    }
    for (i = 0; i < trackCount; i++) {
        const u8 *raw = ctx->raw + trackBase + i * SWITCH_HSF_TRACK_SIZE;
        u32 curve = SwitchHsfBE16(raw + 8);
        u32 keyframes = SwitchHsfBE16(raw + 10);
        u32 dataOfs = SwitchHsfBE32(raw + 12);
        u32 valueCount;
        if (raw[0] != HSF_TRACK_TRANSFORM ||
            curve == HSF_CURVE_CONST || keyframes == 0 ||
            (curve != HSF_CURVE_STEP && curve != HSF_CURVE_LINEAR &&
             curve != HSF_CURVE_BEZIER)) {
            continue;
        }
        valueCount = keyframes * (curve == HSF_CURVE_BEZIER ? 4U : 2U);
        if (valueCount > 0x3FFFFFFFU / sizeof(float) ||
            dataOfs > ctx->rawSize - dataBase ||
            !SwitchHsfRange(ctx, dataBase + dataOfs,
                            valueCount * sizeof(float)) ||
            !SwitchHsfEstimateAdd(total, valueCount, sizeof(float))) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL SwitchHsfEstimateSkeleton(const SwitchHsfContext *ctx,
                                      u32 *total) {
    const SwitchHsfSection *section =
        &ctx->header.section[SWITCH_HSF_SKELETON];
    if (section->count == 0) {
        return TRUE;
    }
    return SwitchHsfTableRange(ctx, SWITCH_HSF_SKELETON,
                               SWITCH_HSF_SKELETON_SIZE) &&
           SwitchHsfEstimateAdd(total, section->count,
                                 sizeof(HSFSKELETON));
}

static BOOL SwitchHsfEstimateCenv(const SwitchHsfContext *ctx, u32 *total) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_CENV];
    u32 descriptorBytes;
    u32 dataBase;
    u32 weightBase;
    u32 i;

    if (section->count == 0) {
        return TRUE;
    }
    if (!SwitchHsfCenvDescriptorSizes(ctx, &descriptorBytes, &weightBase) ||
        !SwitchHsfEstimateAdd(total, section->count, sizeof(HSFCENV))) {
        return FALSE;
    }
    dataBase = section->ofs + section->count * SWITCH_HSF_CENV_SIZE;
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs +
                        i * SWITCH_HSF_CENV_SIZE;
        u32 singleCount = SwitchHsfBE32(raw + 0x10);
        u32 dualCount = SwitchHsfBE32(raw + 0x14);
        u32 multiCount = SwitchHsfBE32(raw + 0x18);
        u32 singleOfs = SwitchHsfBE32(raw + 4);
        u32 dualOfs = SwitchHsfBE32(raw + 8);
        u32 multiOfs = SwitchHsfBE32(raw + 0x0C);
        u32 j;

        if (!SwitchHsfCenvPointerRange(ctx, dataBase, singleOfs,
                                       singleCount,
                                       SWITCH_HSF_CENV_SINGLE_SIZE) ||
            !SwitchHsfCenvPointerRange(ctx, dataBase, dualOfs, dualCount,
                                       SWITCH_HSF_CENV_DUAL_SIZE) ||
            !SwitchHsfCenvPointerRange(ctx, dataBase, multiOfs, multiCount,
                                       SWITCH_HSF_CENV_MULTI_SIZE) ||
            !SwitchHsfEstimateAdd(total, singleCount,
                                  sizeof(HSFCENVSINGLE)) ||
            !SwitchHsfEstimateAdd(total, dualCount, sizeof(HSFCENVDUAL)) ||
            !SwitchHsfEstimateAdd(total, multiCount, sizeof(HSFCENVMULTI))) {
            return FALSE;
        }
        for (j = 0; j < dualCount; j++) {
            const u8 *dual = ctx->raw + dataBase + dualOfs +
                             j * SWITCH_HSF_CENV_DUAL_SIZE;
            u32 weightCount = SwitchHsfBE32(dual + 8);
            u32 weightOfs = SwitchHsfBE32(dual + 0x0C);
            if (!SwitchHsfCountOK(weightCount) ||
                !SwitchHsfCenvPointerRange(ctx, weightBase, weightOfs,
                                            weightCount,
                                            SWITCH_HSF_CENV_DUAL_WEIGHT_SIZE) ||
                !SwitchHsfEstimateAdd(total, weightCount,
                                      sizeof(HSFCENVDUALWEIGHT))) {
                return FALSE;
            }
        }
        for (j = 0; j < multiCount; j++) {
            const u8 *multi = ctx->raw + dataBase + multiOfs +
                              j * SWITCH_HSF_CENV_MULTI_SIZE;
            u32 weightCount = SwitchHsfBE32(multi);
            u32 weightOfs = SwitchHsfBE32(multi + 0x0C);
            if (!SwitchHsfCountOK(weightCount) ||
                !SwitchHsfCenvPointerRange(ctx, weightBase, weightOfs,
                                            weightCount,
                                            SWITCH_HSF_CENV_MULTI_WEIGHT_SIZE) ||
                !SwitchHsfEstimateAdd(total, weightCount,
                                      sizeof(HSFCENVMULTIWEIGHT))) {
                return FALSE;
            }
        }
    }
    (void)descriptorBytes;
    return TRUE;
}

static BOOL SwitchHsfEstimateEnvelopeBuffers(const SwitchHsfContext *ctx,
                                             u32 *total) {
    const SwitchHsfSection *objects = &ctx->header.section[SWITCH_HSF_OBJECT];
    const SwitchHsfSection *vertices = &ctx->header.section[SWITCH_HSF_VERTEX];
    const SwitchHsfSection *normals = &ctx->header.section[SWITCH_HSF_NORMAL];
    u32 i;

    if (ctx->header.section[SWITCH_HSF_CENV].count == 0 ||
        objects->count == 0) {
        return TRUE;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_VERTEX, SWITCH_HSF_BUFFER_SIZE) ||
        !SwitchHsfTableRange(ctx, SWITCH_HSF_NORMAL, SWITCH_HSF_BUFFER_SIZE) ||
        objects->count > 0xFFFFFFFFU / SWITCH_HSF_OBJECT_SIZE) {
        return FALSE;
    }
    for (i = 0; i < objects->count; i++) {
        const u8 *raw = ctx->raw + objects->ofs + i * SWITCH_HSF_OBJECT_SIZE;
        u32 cenvCount = SwitchHsfBE32(raw + 0x134);
        u32 vertexIndex = SwitchHsfBE32(raw + 0x108);
        u32 normalIndex = SwitchHsfBE32(raw + 0x10C);
        u32 vertexCount;
        u32 normalCount;
        if (cenvCount == 0 || vertexIndex >= vertices->count ||
            normalIndex >= normals->count) {
            continue;
        }
        vertexCount = SwitchHsfBE32(ctx->raw + vertices->ofs +
                                    vertexIndex * SWITCH_HSF_BUFFER_SIZE + 4);
        normalCount = SwitchHsfBE32(ctx->raw + normals->ofs +
                                    normalIndex * SWITCH_HSF_BUFFER_SIZE + 4);
        if (!SwitchHsfEstimateAdd(total, vertexCount, sizeof(HuVecF)) ||
            !SwitchHsfEstimateAdd(total, normalCount, sizeof(Vec))) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL SwitchHsfEstimateParts(const SwitchHsfContext *ctx, u32 *total) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_PART];
    u32 dataBase;
    u32 i;
    if (section->count == 0) return TRUE;
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_PART, SWITCH_HSF_PART_SIZE) ||
        section->ofs > 0xFFFFFFFFU - section->count * SWITCH_HSF_PART_SIZE ||
        !SwitchHsfEstimateAdd(total, section->count, sizeof(HSFPART))) {
        return FALSE;
    }
    dataBase = section->ofs + section->count * SWITCH_HSF_PART_SIZE;
    if (!SwitchHsfRange(ctx, dataBase, 0)) return FALSE;
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs + i * SWITCH_HSF_PART_SIZE;
        u32 count = SwitchHsfBE32(raw + 4);
        u32 vertexOfs = SwitchHsfBE32(raw + 8);
        if (count > SWITCH_HSF_MAX_COUNT ||
            vertexOfs > (ctx->rawSize - dataBase) / sizeof(u16) ||
            !SwitchHsfRange(ctx, dataBase + vertexOfs * sizeof(u16),
                            count * sizeof(u16)) ||
            !SwitchHsfEstimateAdd(total, 1,
                                  count ? count * sizeof(u16) : sizeof(u16))) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL SwitchHsfEstimateClusters(const SwitchHsfContext *ctx,
                                       u32 *total) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_CLUSTER];
    u32 i;
    if (section->count == 0) return TRUE;
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_CLUSTER,
                             SWITCH_HSF_CLUSTER_SIZE) ||
        !SwitchHsfEstimateAdd(total, section->count, sizeof(HSFCLUSTER))) {
        return FALSE;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs + i * SWITCH_HSF_CLUSTER_SIZE;
        u32 vertexNum = SwitchHsfBE32(raw + 0x98);
        if (vertexNum > SWITCH_HSF_MAX_CHILDREN ||
            !SwitchHsfEstimateAdd(total, vertexNum, sizeof(HSFBUFFER *))) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL SwitchHsfEstimateShapes(const SwitchHsfContext *ctx, u32 *total) {
    const SwitchHsfSection *section = &ctx->header.section[SWITCH_HSF_SHAPE];
    u32 i;
    if (section->count == 0) return TRUE;
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_SHAPE, SWITCH_HSF_SHAPE_SIZE) ||
        !SwitchHsfEstimateAdd(total, section->count, sizeof(HSFSHAPE))) {
        return FALSE;
    }
    for (i = 0; i < section->count; i++) {
        const u8 *raw = ctx->raw + section->ofs + i * SWITCH_HSF_SHAPE_SIZE;
        u32 vertexNum = SwitchHsfBE16(raw + 6);
        if (vertexNum > SWITCH_HSF_MAX_CHILDREN ||
            !SwitchHsfEstimateAdd(total, vertexNum, sizeof(HSFBUFFER *))) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL SwitchHsfEstimateArena(const SwitchHsfContext *ctx, u32 *total) {
    const SwitchHsfSection *objectSection = &ctx->header.section[SWITCH_HSF_OBJECT];
    u32 i;
    *total = sizeof(HSFDATA) + 8;
    if (ctx->header.section[SWITCH_HSF_STRING].count >
        0xFFFFFFFFU - *total) {
        return FALSE;
    }
    *total += ctx->header.section[SWITCH_HSF_STRING].count;
    if (!SwitchHsfEstimateAdd(total, ctx->header.section[SWITCH_HSF_SCENE].count,
                               sizeof(HSFSCENE)) ||
        !SwitchHsfEstimateMaterials(ctx, total) ||
        !SwitchHsfEstimateAdd(total, ctx->header.section[SWITCH_HSF_ATTRIBUTE].count,
                               sizeof(HSFATTRIBUTE)) ||
        !SwitchHsfEstimatePalettes(ctx, total) ||
        !SwitchHsfEstimateBitmaps(ctx, total) ||
        !SwitchHsfEstimateMotion(ctx, total) ||
        !SwitchHsfEstimateBuffers(ctx, SWITCH_HSF_VERTEX, 12, total) ||
        !SwitchHsfEstimateNormals(ctx, total) ||
        !SwitchHsfEstimateBuffers(ctx, SWITCH_HSF_ST, 8, total) ||
        !SwitchHsfEstimateBuffers(ctx, SWITCH_HSF_COLOR, 4, total) ||
        !SwitchHsfEstimateFaces(ctx, total) ||
        !SwitchHsfEstimateSkeleton(ctx, total) ||
        !SwitchHsfEstimateCenv(ctx, total) ||
        !SwitchHsfEstimateParts(ctx, total) ||
        !SwitchHsfEstimateClusters(ctx, total) ||
        !SwitchHsfEstimateShapes(ctx, total) ||
        !SwitchHsfEstimateEnvelopeBuffers(ctx, total)) {
        return FALSE;
    }
    if (objectSection->count == 0) {
        return TRUE;
    }
    if (!SwitchHsfTableRange(ctx, SWITCH_HSF_OBJECT, SWITCH_HSF_OBJECT_SIZE) ||
        !SwitchHsfEstimateAdd(total, objectSection->count,
                               sizeof(HSFOBJECT) + sizeof(HSFCONSTDATA))) {
        return FALSE;
    }
    for (i = 0; i < objectSection->count; i++) {
        const u8 *raw = ctx->raw + objectSection->ofs + i * SWITCH_HSF_OBJECT_SIZE;
        u32 children = SwitchHsfBE32(raw + 0x14);
        u32 shapes = SwitchHsfBE32(raw + 0x124);
        u32 clusters = SwitchHsfBE32(raw + 0x12C);
        if (children > SWITCH_HSF_MAX_CHILDREN ||
            shapes > SWITCH_HSF_MAX_CHILDREN ||
            clusters > SWITCH_HSF_MAX_CHILDREN ||
            !SwitchHsfEstimateAdd(total, children, sizeof(HSFOBJECT *))) {
            return FALSE;
        }
        if (!SwitchHsfEstimateAdd(total, shapes, sizeof(HSFBUFFER *)) ||
            !SwitchHsfEstimateAdd(total, clusters, sizeof(HSFCLUSTER *))) {
            return FALSE;
        }
    }
    return TRUE;
}

void *LoadHSF(void *data) {
    SwitchHsfContext ctx;
    HSFDATA *model;
    HSFSCENE *scene;
    u32 rawSize;
    u32 estimatedSize = 0;
    u32 arenaSize;

    if (!data) {
        return NULL;
    }
    memset(&ctx, 0, sizeof(ctx));
    rawSize = SwitchHsfRawSize(data);
    if (rawSize < SWITCH_HSF_HEADER_SIZE || rawSize > 0x4000000U) {
        OSReport("HSF Switch: invalid source size %u\n", rawSize);
        SwitchModelRawFree(data);
        return NULL;
    }
    ctx.rawSize = rawSize;
    if (!SwitchHsfHeaderRead(&ctx, data)) {
        OSReport("HSF Switch: invalid header\n");
        SwitchModelRawFree(data);
        return NULL;
    }

    if (SwitchHsfEstimateArena(&ctx, &estimatedSize)) {
        /* The estimate covers all copied arrays.  Keep a modest cushion for
         * alignment and future small metadata, instead of reserving the
         * whole source block again. */
        arenaSize = estimatedSize + 0x20000U;
    } else {
        /* A few HSF variants have legacy buffer layouts that the estimator
         * does not recognize yet.  The parser still validates them while
         * loading, so use a larger fallback arena for those files. */
        arenaSize = rawSize + rawSize / 2U + 0x10000U;
    }
    if (arenaSize < 0x40000U) {
        arenaSize = 0x40000U;
    }
    if (arenaSize > 0x900000U || arenaSize < estimatedSize) {
        OSReport("HSF Switch: native arena too large (%u)\n", arenaSize);
        SwitchModelRawFree(data);
        return NULL;
    }
    model = (HSFDATA *)HuMemDirectMalloc(HEAP_MODEL, (s32)arenaSize);
    if (!model) {
        OSReport("HSF Switch: native arena allocation failed (%u)\n", arenaSize);
        SwitchModelRawFree(data);
        return NULL;
    }
    memset(model, 0, sizeof(*model));
    ctx.arena.base = (u8 *)model;
    ctx.arena.size = arenaSize;
    ctx.arena.used = sizeof(*model);
    memcpy(model->magic, ctx.raw, sizeof(model->magic));

    if (ctx.header.section[SWITCH_HSF_STRING].count != 0) {
        const SwitchHsfSection *strings = &ctx.header.section[SWITCH_HSF_STRING];
        if (!SwitchHsfRange(&ctx, strings->ofs, strings->count)) {
            goto fail;
        }
        ctx.stringSize = strings->count;
        ctx.strings = (char *)SwitchHsfArenaAlloc(&ctx.arena, ctx.stringSize + 1);
        if (!ctx.strings) {
            goto fail;
        }
        memcpy(ctx.strings, ctx.raw + strings->ofs, ctx.stringSize);
        ctx.strings[ctx.stringSize] = '\0';
    }

    scene = SwitchHsfParseScene(&ctx);
    ctx.palette = SwitchHsfParsePalettes(&ctx);
    ctx.material = SwitchHsfParseMaterials(&ctx);
    ctx.bitmap = SwitchHsfParseBitmaps(&ctx);
    ctx.attribute = SwitchHsfParseAttributes(&ctx);
    ctx.vertex = SwitchHsfParseBuffers(&ctx, SWITCH_HSF_VERTEX, 12, 0);
    ctx.normal = SwitchHsfParseNormals(&ctx);
    ctx.st = SwitchHsfParseBuffers(&ctx, SWITCH_HSF_ST, 8, 2);
    ctx.color = SwitchHsfParseBuffers(&ctx, SWITCH_HSF_COLOR, 4, 3);
    ctx.face = SwitchHsfParseFaces(&ctx);
    if ((ctx.header.section[SWITCH_HSF_SCENE].count && !scene) ||
        (ctx.header.section[SWITCH_HSF_MATERIAL].count && !ctx.material) ||
        (ctx.header.section[SWITCH_HSF_PALETTE].count && !ctx.palette) ||
        (ctx.header.section[SWITCH_HSF_BITMAP].count && !ctx.bitmap) ||
        (ctx.header.section[SWITCH_HSF_ATTRIBUTE].count && !ctx.attribute) ||
        (ctx.header.section[SWITCH_HSF_VERTEX].count && !ctx.vertex) ||
        (ctx.header.section[SWITCH_HSF_NORMAL].count && !ctx.normal) ||
        (ctx.header.section[SWITCH_HSF_ST].count && !ctx.st) ||
        (ctx.header.section[SWITCH_HSF_FACE].count && !ctx.face)) {
        goto fail;
    }

    model->scene = scene;
    model->sceneNum = (s16)ctx.header.section[SWITCH_HSF_SCENE].count;
    model->material = ctx.material;
    model->materialNum = (s16)ctx.header.section[SWITCH_HSF_MATERIAL].count;
    model->bitmap = ctx.bitmap;
    model->bitmapNum = (s16)ctx.header.section[SWITCH_HSF_BITMAP].count;
    model->palette = ctx.palette;
    model->paletteNum = (s16)ctx.header.section[SWITCH_HSF_PALETTE].count;
    model->attribute = ctx.attribute;
    model->attributeNum = (s16)ctx.header.section[SWITCH_HSF_ATTRIBUTE].count;
    model->vertex = ctx.vertex;
    model->vertexNum = (s16)ctx.header.section[SWITCH_HSF_VERTEX].count;
    model->normal = ctx.normal;
    model->normalNum = (s16)ctx.header.section[SWITCH_HSF_NORMAL].count;
    model->st = ctx.st;
    model->stNum = (s16)ctx.header.section[SWITCH_HSF_ST].count;
    model->color = ctx.color;
    model->colorNum = (s16)ctx.header.section[SWITCH_HSF_COLOR].count;
    model->face = ctx.face;
    model->faceNum = (s16)ctx.header.section[SWITCH_HSF_FACE].count;
    model->objectNum = (s16)ctx.header.section[SWITCH_HSF_OBJECT].count;
    ctx.part = SwitchHsfParseParts(&ctx);
    ctx.cluster = SwitchHsfParseClusters(&ctx);
    ctx.shape = SwitchHsfParseShapes(&ctx);
    if ((ctx.header.section[SWITCH_HSF_PART].count && !ctx.part) ||
        (ctx.header.section[SWITCH_HSF_CLUSTER].count && !ctx.cluster) ||
        (ctx.header.section[SWITCH_HSF_SHAPE].count && !ctx.shape)) {
        goto fail;
    }
    model->part = ctx.part;
    model->partNum = (s16)ctx.header.section[SWITCH_HSF_PART].count;
    model->cluster = ctx.cluster;
    model->clusterNum = (s16)ctx.header.section[SWITCH_HSF_CLUSTER].count;
    model->shape = ctx.shape;
    model->shapeNum = (s16)ctx.header.section[SWITCH_HSF_SHAPE].count;
    ctx.skeleton = SwitchHsfParseSkeleton(&ctx);
    ctx.cenv = SwitchHsfParseCenv(&ctx);
    model->cenv = ctx.cenv;
    model->cenvNum = ctx.cenv ? (s16)ctx.header.section[SWITCH_HSF_CENV].count : 0;
    model->skeleton = ctx.skeleton;
    model->skeletonNum = ctx.skeleton ?
        (s16)ctx.header.section[SWITCH_HSF_SKELETON].count : 0;
    model->mapAttrNum = 0;
    ctx.motion = SwitchHsfParseMotion(&ctx);
    model->motion = ctx.motion;
    model->motionNum = ctx.motion ? (s16)ctx.motion->numTracks : 0;
    model->matrixNum = 0;
    model->object = SwitchHsfParseObjects(&ctx, model);
    if (ctx.header.section[SWITCH_HSF_OBJECT].count && !model->object) {
        goto fail;
    }
    SwitchHsfResolveReferences(model, ctx.stringSize);

    SwitchModelRawFree(data);
    return model;

fail:
    OSReport("HSF Switch: conversion failed\n");
    HuMemDirectFree(model);
    SwitchModelRawFree(data);
    return NULL;
}

void MakeDisplayList(HU3DMODELID modelId, u32 no) {
    (void)modelId;
    (void)no;
}

static const HSFOBJECT *s_hsfObjectBase;
static u32 s_hsfObjectCount;

static BOOL SwitchHsfObjectInModel(const HSFOBJECT *object) {
    uintptr_t address;
    uintptr_t base;
    uintptr_t end;
    if (!object || !s_hsfObjectBase || s_hsfObjectCount == 0) {
        return FALSE;
    }
    address = (uintptr_t)object;
    base = (uintptr_t)s_hsfObjectBase;
    end = base + (uintptr_t)s_hsfObjectCount * sizeof(HSFOBJECT);
    return address >= base && address < end &&
           ((address - base) % sizeof(HSFOBJECT)) == 0;
}

static void SwitchHsfObjectLocalMode(const HSFOBJECT *object, BOOL base,
                                     Mtx out) {
    const HSFTRANSFORM *transform = base ? &object->mesh.base :
                                             &object->mesh.curr;
    PSMTXIdentity(out);
    mtxRot(out, transform->rot.x, transform->rot.y, transform->rot.z);
    mtxScaleCat(out, transform->scale.x, transform->scale.y,
                transform->scale.z);
    mtxTransCat(out, transform->pos.x, transform->pos.y, transform->pos.z);
}

static void SwitchHsfObjectMatrixMode(const HSFOBJECT *object, BOOL base,
                                      Mtx model, Mtx out) {
    const HSFOBJECT *chain[SWITCH_HSF_MAX_DEPTH];
    const HSFOBJECT *current = object;
    s32 count = 0;
    s32 i;
    if (model) {
        PSMTXCopy(model, out);
    } else {
        PSMTXIdentity(out);
    }
    while (current && count < SWITCH_HSF_MAX_DEPTH) {
        if (!SwitchHsfObjectInModel(current)) {
            break;
        }
        chain[count++] = current;
        current = current->mesh.parent;
    }
    for (i = count - 1; i >= 0; i--) {
        Mtx local;
        SwitchHsfObjectLocalMode(chain[i], base, local);
        PSMTXConcat(out, local, out);
    }
}

static void SwitchHsfObjectMatrix(const HSFOBJECT *object, Mtx model, Mtx out) {
    SwitchHsfObjectMatrixMode(object, FALSE, model, out);
}

static u32 SwitchHsfMtxInverse(const Mtx src, Mtx dst) {
    float a = src[0][0], b = src[0][1], c = src[0][2];
    float d = src[1][0], e = src[1][1], f = src[1][2];
    float g = src[2][0], h = src[2][1], i = src[2][2];
    float det = a * (e * i - f * h) - b * (d * i - f * g) +
                c * (d * h - e * g);
    float inv;

    if (fabsf(det) < 0.000001f) {
        PSMTXIdentity(dst);
        return 0;
    }
    inv = 1.0f / det;
    dst[0][0] = (e * i - f * h) * inv;
    dst[0][1] = (c * h - b * i) * inv;
    dst[0][2] = (b * f - c * e) * inv;
    dst[1][0] = (f * g - d * i) * inv;
    dst[1][1] = (a * i - c * g) * inv;
    dst[1][2] = (c * d - a * f) * inv;
    dst[2][0] = (d * h - e * g) * inv;
    dst[2][1] = (b * g - a * h) * inv;
    dst[2][2] = (a * e - b * d) * inv;
    dst[0][3] = -(dst[0][0] * src[0][3] + dst[0][1] * src[1][3] +
                  dst[0][2] * src[2][3]);
    dst[1][3] = -(dst[1][0] * src[0][3] + dst[1][1] * src[1][3] +
                  dst[1][2] * src[2][3]);
    dst[2][3] = -(dst[2][0] * src[0][3] + dst[2][1] * src[1][3] +
                  dst[2][2] * src[2][3]);
    return 1;
}

static void SwitchHsfMtxMultVec(const Mtx matrix, const HuVecF *source,
                                HuVecF *destination) {
    HuVecF value;
    value.x = matrix[0][0] * source->x + matrix[0][1] * source->y +
              matrix[0][2] * source->z + matrix[0][3];
    value.y = matrix[1][0] * source->x + matrix[1][1] * source->y +
              matrix[1][2] * source->z + matrix[1][3];
    value.z = matrix[2][0] * source->x + matrix[2][1] * source->y +
              matrix[2][2] * source->z + matrix[2][3];
    *destination = value;
}

static void SwitchHsfMtxMultVecArray(const Mtx matrix,
                                     const HuVecF *source,
                                     HuVecF *destination, u32 count) {
    u32 i;
    for (i = 0; i < count; i++) {
        SwitchHsfMtxMultVec(matrix, &source[i], &destination[i]);
    }
}

static BOOL SwitchHsfSkinMatrix(const HSFOBJECT *mesh,
                                const HSFOBJECT *target,
                                const Mtx meshCurrentInverse,
                                const Mtx meshBase, Mtx out) {
    Mtx targetCurrent;
    Mtx targetBase;
    Mtx targetBaseInverse;
    Mtx temp;
    Mtx temp2;

    SwitchHsfObjectMatrixMode(target, FALSE, NULL, targetCurrent);
    SwitchHsfObjectMatrixMode(target, TRUE, NULL, targetBase);
    if (!SwitchHsfMtxInverse(targetBase, targetBaseInverse)) {
        return FALSE;
    }
    PSMTXConcat(targetCurrent, targetBaseInverse, temp);
    PSMTXConcat(temp, meshBase, temp2);
    PSMTXConcat(meshCurrentInverse, temp2, out);
    (void)mesh;
    return TRUE;
}

static void SwitchHsfMatrixBlend(const Mtx first, const Mtx second,
                                 float firstWeight, Mtx out) {
    float secondWeight = 1.0f - firstWeight;
    s32 row;
    s32 column;
    for (row = 0; row < 3; row++) {
        for (column = 0; column < 4; column++) {
            out[row][column] = first[row][column] * firstWeight +
                               second[row][column] * secondWeight;
        }
    }
}

static BOOL SwitchHsfEnvelopeRange(u32 start, u32 count, s32 available) {
    return start <= (u32)available && count <= (u32)available - start;
}

static const HSFOBJECT *SwitchHsfEnvelopeTarget(const HSFDATA *model,
                                                u32 target) {
    if (!model || !model->object || target >= (u32)model->objectNum) {
        return NULL;
    }
    return &model->object[target];
}

static void SwitchHsfUpdateEnvelope(const HSFDATA *model, HSFOBJECT *object) {
    const HuVecF *sourceVertex;
    HuVecF *destinationVertex;
    const Vec *sourceNormal;
    Vec *destinationNormal;
    Mtx meshCurrent;
    Mtx meshCurrentInverse;
    Mtx meshBase;
    u32 cenvIndex;
    u32 i;

    if (!model || !object || object->mesh.cenvNum == 0 ||
        !object->mesh.vertex || !object->mesh.vertex->data ||
        !object->mesh.file[0]) {
        return;
    }
    sourceVertex = (const HuVecF *)object->mesh.vertex->data;
    destinationVertex = (HuVecF *)object->mesh.file[0];
    sourceNormal = object->mesh.normal && object->mesh.normal->data ?
        (const Vec *)object->mesh.normal->data : NULL;
    destinationNormal = object->mesh.file[1] ? (Vec *)object->mesh.file[1] : NULL;

    SwitchHsfObjectMatrixMode(object, FALSE, NULL, meshCurrent);
    SwitchHsfObjectMatrixMode(object, TRUE, NULL, meshBase);
    if (!SwitchHsfMtxInverse(meshCurrent, meshCurrentInverse)) {
        return;
    }
    memcpy(destinationVertex, sourceVertex,
           (u32)object->mesh.vertex->count * sizeof(HuVecF));
    if (sourceNormal && destinationNormal) {
        memcpy(destinationNormal, sourceNormal,
               (u32)object->mesh.normal->count * sizeof(Vec));
    }

    for (cenvIndex = 0; cenvIndex < object->mesh.cenvNum; cenvIndex++) {
        const HSFCENV *cenv = &object->mesh.cenv[cenvIndex];
        for (i = 0; i < cenv->singleCount; i++) {
            const HSFCENVSINGLE *single = &cenv->singleData[i];
            const HSFOBJECT *target = SwitchHsfEnvelopeTarget(
                model, single->target);
            Mtx matrix;
            Mtx normalMatrix;
            if (!target ||
                !SwitchHsfEnvelopeRange(single->pos, single->posNum,
                                        object->mesh.vertex->count) ||
                (single->normalNum != 0 &&
                 (!sourceNormal || !destinationNormal ||
                  !SwitchHsfEnvelopeRange(single->normal, single->normalNum,
                                          object->mesh.normal->count))) ||
                !SwitchHsfSkinMatrix(object, target, meshCurrentInverse,
                                      meshBase, matrix)) {
                continue;
            }
            SwitchHsfMtxMultVecArray(matrix, sourceVertex + single->pos,
                                     destinationVertex + single->pos,
                                     single->posNum);
            if (sourceNormal && destinationNormal && single->normalNum != 0 &&
                PSMTXInvXpose(matrix, normalMatrix)) {
                SwitchHsfMtxMultVecArray(
                    normalMatrix, (const HuVecF *)(sourceNormal + single->normal),
                    (HuVecF *)(destinationNormal + single->normal),
                    single->normalNum);
            }
        }

        for (i = 0; i < cenv->dualCount; i++) {
            const HSFCENVDUAL *dual = &cenv->dualData[i];
            const HSFOBJECT *target1 = SwitchHsfEnvelopeTarget(
                model, dual->target1);
            const HSFOBJECT *target2 = SwitchHsfEnvelopeTarget(
                model, dual->target2);
            Mtx matrix1;
            Mtx matrix2;
            if (!target1 || !target2 ||
                !SwitchHsfSkinMatrix(object, target1, meshCurrentInverse,
                                      meshBase, matrix1) ||
                !SwitchHsfSkinMatrix(object, target2, meshCurrentInverse,
                                      meshBase, matrix2)) {
                continue;
            }
            {
                u32 j;
                for (j = 0; j < dual->weightNum; j++) {
                    const HSFCENVDUALWEIGHT *weight = &dual->weight[j];
                    float value = weight->weight;
                    Mtx matrix;
                    Mtx normalMatrix;
                    if (value < 0.0f) value = 0.0f;
                    if (value > 1.0f) value = 1.0f;
                    if (!SwitchHsfEnvelopeRange(weight->pos, weight->posNum,
                                                object->mesh.vertex->count) ||
                        (weight->normalNum != 0 &&
                         (!sourceNormal || !destinationNormal ||
                          !SwitchHsfEnvelopeRange(
                              weight->normal, weight->normalNum,
                              object->mesh.normal->count)))) {
                        continue;
                    }
                    SwitchHsfMatrixBlend(matrix1, matrix2, value, matrix);
                    SwitchHsfMtxMultVecArray(matrix, sourceVertex + weight->pos,
                                             destinationVertex + weight->pos,
                                             weight->posNum);
                    if (sourceNormal && destinationNormal &&
                        weight->normalNum != 0 &&
                        PSMTXInvXpose(matrix, normalMatrix)) {
                        SwitchHsfMtxMultVecArray(
                            normalMatrix,
                            (const HuVecF *)(sourceNormal + weight->normal),
                            (HuVecF *)(destinationNormal + weight->normal),
                            weight->normalNum);
                    }
                }
            }
        }

        for (i = 0; i < cenv->multiCount; i++) {
            const HSFCENVMULTI *multi = &cenv->multiData[i];
            u32 j;
            if (!SwitchHsfEnvelopeRange(multi->pos, multi->posNum,
                                        object->mesh.vertex->count) ||
                (multi->normalNum != 0 &&
                 (!sourceNormal || !destinationNormal ||
                  !SwitchHsfEnvelopeRange(multi->normal, multi->normalNum,
                                          object->mesh.normal->count)))) {
                continue;
            }
            for (j = 0; j < multi->weightNum; j++) {
                const HSFCENVMULTIWEIGHT *weight = &multi->weight[j];
                const HSFOBJECT *target = SwitchHsfEnvelopeTarget(
                    model, weight->target);
                Mtx matrix;
                Mtx normalMatrix;
                HuVecF transformed;
                Vec transformedNormal;
                u32 k;
                if (!target || !SwitchHsfSkinMatrix(
                        object, target, meshCurrentInverse, meshBase, matrix)) {
                    continue;
                }
                for (k = 0; k < multi->posNum; k++) {
                    SwitchHsfMtxMultVec(matrix, &sourceVertex[multi->pos + k],
                                        &transformed);
                    destinationVertex[multi->pos + k].x += weight->value *
                        (transformed.x - sourceVertex[multi->pos + k].x);
                    destinationVertex[multi->pos + k].y += weight->value *
                        (transformed.y - sourceVertex[multi->pos + k].y);
                    destinationVertex[multi->pos + k].z += weight->value *
                        (transformed.z - sourceVertex[multi->pos + k].z);
                }
                if (sourceNormal && destinationNormal && multi->normalNum != 0 &&
                    PSMTXInvXpose(matrix, normalMatrix)) {
                    for (k = 0; k < multi->normalNum; k++) {
                        SwitchHsfMtxMultVec(normalMatrix,
                                            &sourceNormal[multi->normal + k],
                                            &transformedNormal);
                        destinationNormal[multi->normal + k].x += weight->value *
                            (transformedNormal.x -
                             sourceNormal[multi->normal + k].x);
                        destinationNormal[multi->normal + k].y += weight->value *
                            (transformedNormal.y -
                             sourceNormal[multi->normal + k].y);
                        destinationNormal[multi->normal + k].z += weight->value *
                            (transformedNormal.z -
                             sourceNormal[multi->normal + k].z);
                    }
                }
            }
        }

        if (cenv->copyCount != 0 &&
            SwitchHsfEnvelopeRange(cenv->vtxCount, cenv->copyCount,
                                   object->mesh.vertex->count)) {
            memcpy(destinationVertex + cenv->vtxCount,
                   sourceVertex + cenv->vtxCount,
                   cenv->copyCount * sizeof(HuVecF));
        }
    }
}

void SwitchHsfClusterAdjustObject(HSFDATA *model, HSFDATA *motionModel) {
    s32 i;
    if (!model || !motionModel || !motionModel->cluster) {
        return;
    }
    for (i = 0; i < motionModel->clusterNum; i++) {
        HSFCLUSTER *cluster = &motionModel->cluster[i];
        cluster->target = SwitchHsfFindObject(model, cluster->targetName);
        cluster->adjusted = TRUE;
    }
}

static void SwitchHsfApplyShape(HSFOBJECT *object) {
    HuVecF *destination;
    HSFBUFFER *base;
    u32 i;

    if (!object || object->type != HSF_OBJ_MESH ||
        object->mesh.shapeNum == 0 || !object->mesh.shape ||
        !object->mesh.vertex || !object->mesh.vertex->data) {
        return;
    }
    base = object->mesh.shape[0];
    destination = (HuVecF *)object->mesh.vertex->data;
    if (!base || !base->data || base->count <= 0 ||
        base->count > object->mesh.vertex->count) {
        return;
    }

    if (object->mesh.shapeType == 2) {
        float total = 0.0f;
        for (i = 0; i < object->mesh.shapeNum && i < 33; i++) {
            total += object->mesh.mesh.morphWeight[i];
        }
        for (i = 0; i < (u32)base->count; i++) {
            destination[i] = ((const HuVecF *)base->data)[i];
        }
        for (i = 0; i < object->mesh.shapeNum && i < 33; i++) {
            HSFBUFFER *shape = object->mesh.shape[i];
            float weight = object->mesh.mesh.morphWeight[i];
            u32 j;
            if (!shape || !shape->data || shape->count < base->count) {
                continue;
            }
            if (weight < 0.0f) weight = 0.0f;
            if (total > 1.0f) weight /= total;
            for (j = 0; j < (u32)base->count; j++) {
                destination[j].x += weight *
                    (((const HuVecF *)shape->data)[j].x - destination[j].x);
                destination[j].y += weight *
                    (((const HuVecF *)shape->data)[j].y - destination[j].y);
                destination[j].z += weight *
                    (((const HuVecF *)shape->data)[j].z - destination[j].z);
            }
        }
    } else {
        float morph = object->mesh.mesh.baseMorph;
        s32 first = (s32)floorf(morph);
        float fraction = morph - (float)first;
        HSFBUFFER *firstBuffer;
        HSFBUFFER *secondBuffer;
        u32 j;
        if (first < 0) first = 0;
        if ((u32)first >= object->mesh.shapeNum) {
            first = (s32)object->mesh.shapeNum - 1;
        }
        if (first < 0) return;
        if (fraction < 0.0f) fraction = 0.0f;
        if (fraction > 1.0f) fraction = 1.0f;
        secondBuffer = object->mesh.shape[first + 1 < (s32)object->mesh.shapeNum ?
                                           first + 1 : first];
        firstBuffer = object->mesh.shape[first];
        if (!firstBuffer || !secondBuffer || !firstBuffer->data ||
            !secondBuffer->data || firstBuffer->count < base->count ||
            secondBuffer->count < base->count) {
            return;
        }
        for (j = 0; j < (u32)base->count; j++) {
            const HuVecF *a = &((const HuVecF *)firstBuffer->data)[j];
            const HuVecF *b = &((const HuVecF *)secondBuffer->data)[j];
            destination[j].x = a->x + fraction * (b->x - a->x);
            destination[j].y = a->y + fraction * (b->y - a->y);
            destination[j].z = a->z + fraction * (b->z - a->z);
        }
    }
    object->mesh.writeNum++;
}

void SwitchHsfShapeProc(HSFDATA *model) {
    s32 i;
    if (!model || !model->object) {
        return;
    }
    for (i = 0; i < model->objectNum; i++) {
        SwitchHsfApplyShape(&model->object[i]);
    }
}

static void SwitchHsfApplyCluster(HSFOBJECT *object, HSFCLUSTER *cluster) {
    HuVecF *destination;
    HSFPART *part;
    u32 i;
    if (!object || !cluster || !cluster->part || !object->mesh.vertex ||
        !object->mesh.vertex->data || cluster->vertexNum == 0 ||
        !cluster->vertex) {
        return;
    }
    destination = (HuVecF *)object->mesh.vertex->data;
    part = cluster->part;
    if (cluster->type == 2) {
        float total = 0.0f;
        for (i = 0; i < cluster->vertexNum && i < 32; i++) {
            total += cluster->weight[i];
        }
        for (i = 0; i < cluster->vertexNum; i++) {
            HSFBUFFER *source = cluster->vertex[i];
            float weight = i < 32 ? cluster->weight[i] : 0.0f;
            u32 j;
            if (!source || !source->data || source->count < part->num) {
                continue;
            }
            if (weight < 0.0f) weight = 0.0f;
            if (total > 1.0f) weight /= total;
            for (j = 0; j < part->num; j++) {
                u16 vertex = part->vertex[j];
                if (vertex >= (u32)object->mesh.vertex->count) continue;
                destination[vertex].x += weight *
                    (((const HuVecF *)source->data)[j].x - destination[vertex].x);
                destination[vertex].y += weight *
                    (((const HuVecF *)source->data)[j].y - destination[vertex].y);
                destination[vertex].z += weight *
                    (((const HuVecF *)source->data)[j].z - destination[vertex].z);
            }
        }
        return;
    }

    {
        s32 first = (s32)floorf(cluster->index);
        float fraction = cluster->index - (float)first;
        HSFBUFFER *a;
        HSFBUFFER *b;
        if (first < 0) first = 0;
        if ((u32)first >= cluster->vertexNum) first = cluster->vertexNum - 1;
        if (fraction < 0.0f) fraction = 0.0f;
        if (fraction > 1.0f) fraction = 1.0f;
        a = cluster->vertex[first];
        b = cluster->vertex[first + 1 < (s32)cluster->vertexNum ?
                            first + 1 : first];
        if (!a || !b || !a->data || !b->data || a->count < part->num ||
            b->count < part->num) {
            return;
        }
        for (i = 0; i < part->num; i++) {
            u16 vertex = part->vertex[i];
            const HuVecF *va = &((const HuVecF *)a->data)[i];
            const HuVecF *vb = &((const HuVecF *)b->data)[i];
            if (vertex >= (u32)object->mesh.vertex->count) continue;
            destination[vertex].x = va->x + fraction * (vb->x - va->x);
            destination[vertex].y = va->y + fraction * (vb->y - va->y);
            destination[vertex].z = va->z + fraction * (vb->z - va->z);
        }
    }
    object->mesh.writeNum++;
}

void SwitchHsfClusterProc(HU3DMODEL *modelP) {
    s32 slot;
    s32 i;
    if (!modelP || !modelP->hsf || !(modelP->attr & HU3D_ATTR_CLUSTER_ON)) {
        return;
    }
    for (slot = 0; slot < 4; slot++) {
        s16 motionId = modelP->motIdCluster[slot];
        HSFDATA *motionModel;
        if (motionId < 0 || motionId >= HU3D_MOTION_MAX ||
            !Hu3DMotion[motionId].hsf) {
            continue;
        }
        motionModel = Hu3DMotion[motionId].hsf;
        SwitchHsfClusterAdjustObject(modelP->hsf, motionModel);
        for (i = 0; i < motionModel->clusterNum; i++) {
            HSFCLUSTER *cluster = &motionModel->cluster[i];
            if (cluster->target >= 0 && cluster->target < modelP->hsf->objectNum) {
                SwitchHsfApplyCluster(&modelP->hsf->object[cluster->target],
                                      cluster);
            }
        }
    }
}

void SwitchHsfClusterMotionExec(HU3DMODEL *modelP) {
    s32 slot;
    if (!modelP) return;
    for (slot = 0; slot < 4; slot++) {
        s16 motionId = modelP->motIdCluster[slot];
        HSFDATA *motionModel;
        HSFMOTION *motion;
        s32 i;
        if (motionId < 0 || motionId >= HU3D_MOTION_MAX ||
            !Hu3DMotion[motionId].hsf ||
            !(motionModel = Hu3DMotion[motionId].hsf) ||
            !(motion = motionModel->motion)) {
            continue;
        }
        for (i = 0; i < motion->numTracks; i++) {
            HSFTRACK *track = &motion->track[i];
            if (track->target == 0xFFFF ||
                track->target >= (u16)motionModel->clusterNum) {
                continue;
            }
            if (track->type == HSF_TRACK_CLUSTER) {
                motionModel->cluster[track->target].index =
                    GetCurve(track, modelP->clusterTime[slot]);
            } else if (track->type == HSF_TRACK_CLUSTER_WEIGHT &&
                       track->clusterWeight >= 0 &&
                       track->clusterWeight < 32) {
                motionModel->cluster[track->target].weight[track->clusterWeight] =
                    GetCurve(track, modelP->clusterTime[slot]);
            }
        }
    }
}

static void SwitchHsfSetMaterial(const HSFOBJECT *object, s16 matIndex,
                                 s16 materialCount, u32 modelAttr) {
    GXColor color = {255, 255, 255, 255};
    const HSFMATERIAL *material = NULL;
    u32 flags = object->flags;
    if (object->mesh.material && matIndex >= 0 &&
        matIndex < materialCount && matIndex < 0x1000) {
        material = &object->mesh.material[matIndex];
        flags |= material->flags;
        color.r = material->color[0];
        color.g = material->color[1];
        color.b = material->color[2];
        if (material->invAlpha > 0.0f) {
            float alpha = 1.0f - material->invAlpha;
            if (alpha < 0.0f) alpha = 0.0f;
            if (alpha > 1.0f) alpha = 1.0f;
            color.a = (u8)(alpha * 255.0f + 0.5f);
        }
    }

    if (modelAttr & HU3D_ATTR_CULL_FRONT) {
        GXSetCullMode(GX_CULL_FRONT);
    } else if (flags & HSF_MATERIAL_NOCULL) {
        GXSetCullMode(GX_CULL_NONE);
    } else {
        GXSetCullMode(GX_CULL_BACK);
    }

    if (modelAttr & HU3D_ATTR_ZCMP_OFF) {
        GXSetZMode(GX_FALSE, GX_LEQUAL, GX_FALSE);
    } else {
        BOOL zWrite = TRUE;
        if (modelAttr & HU3D_ATTR_ZWRITE_OFF) {
            zWrite = FALSE;
        } else if (flags & (HSF_MATERIAL_DISABLE_ZWRITE | HSF_MATERIAL_NEAR)) {
            zWrite = FALSE;
        } else if (material && (material->invAlpha != 0.0f ||
                                (material->pass & 0xF) ||
                                (flags & (HSF_MATERIAL_ADDCOL |
                                          HSF_MATERIAL_INVCOL)))) {
            zWrite = FALSE;
        }
        GXSetZMode(GX_TRUE, GX_LEQUAL, zWrite);
    }

    if (flags & (HSF_MATERIAL_DISABLE_ZWRITE | HSF_MATERIAL_NEAR)) {
        GXSetAlphaCompare(GX_GEQUAL, 0x80, GX_AOP_OR,
                          GX_GEQUAL, 0x80);
    } else {
        GXSetAlphaCompare(GX_GEQUAL, 1, GX_AOP_AND,
                          GX_GEQUAL, 1);
    }

    if (flags & HSF_MATERIAL_ADDCOL) {
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE,
                       GX_LO_NOOP);
    } else if (flags & HSF_MATERIAL_INVCOL) {
        GXSetBlendMode(GX_BM_BLEND, GX_BL_ZERO, GX_BL_INVDSTCLR,
                       GX_LO_NOOP);
    } else {
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA,
                       GX_LO_NOOP);
    }
#ifdef __SWITCH__
    /* The temporary solid-material path has no decoded texture alpha.  Do
     * not discard the whole title because the original alpha test expects it. */
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetCullMode(GX_CULL_NONE);
#endif
    GXSetChanMatColor(GX_COLOR0A0, color);
}

static const HSFATTRIBUTE *SwitchHsfMaterialAttribute(
    const HSFOBJECT *object, s16 matIndex, s16 materialCount) {
    const HSFMATERIAL *material;
    u32 attrIndex;
    if (!object || !object->mesh.material || !object->mesh.attribute ||
        matIndex < 0 || matIndex >= materialCount || matIndex >= 0x1000) {
        return NULL;
    }
    material = &object->mesh.material[matIndex];
    if (material->attrNum == 0 || !material->attr ||
        material->attr[0] < 0) {
        return NULL;
    }
    attrIndex = (u32)material->attr[0];
    if (attrIndex >= 0x1000) {
        return NULL;
    }
    return &object->mesh.attribute[attrIndex];
}

unsigned int g_hsfTexCalls, g_hsfTexReflect, g_hsfTexNoAttr, g_hsfTexNoBmp,
             g_hsfTexBadFmt, g_hsfTexLoaded;

static void SwitchHsfSetTexture(const HSFOBJECT *object, s16 matIndex,
                                s16 materialCount) {
    GXTexObj texObj;
    GXTlutObj tlutObj;
    const HSFATTRIBUTE *attribute;
    HSFBITMAP *bitmap;
    GXTlutFmt tlutFormat;
    GXCITexFmt ciFormat;
    BOOL indexed = FALSE;
    u32 materialFlags = object ? object->flags : 0;

    if (object && object->mesh.material && matIndex >= 0 &&
        matIndex < materialCount && matIndex < 0x1000) {
        materialFlags |= object->mesh.material[matIndex].flags;
    }
    /* Reflection materials use a GameCube environment/cubemap path that the
     * GLES2 compatibility layer does not expose yet.  Keep their geometry
     * visible with the material tint until that path is implemented. */
    g_hsfTexCalls++;
    if (materialFlags & HSF_MATERIAL_REFLECTMODEL) {
        g_hsfTexReflect++;
        return;
    }

    GXInvalidateTexAll();
    attribute = SwitchHsfMaterialAttribute(object, matIndex, materialCount);
    if (!attribute) {
        g_hsfTexNoAttr++;
        return;
    }
    bitmap = attribute->bitmap;
    if (!bitmap || !bitmap->data || bitmap->sizeX <= 0 || bitmap->sizeY <= 0) {
        g_hsfTexNoBmp++;
        return;
    }
    switch (bitmap->dataFmt) {
        case HSF_BMPFMT_RGBA8:
            GXInitTexObj(&texObj, bitmap->data, bitmap->sizeX, bitmap->sizeY,
                         GX_TF_RGBA8, attribute->wrapS ? GX_REPEAT : GX_CLAMP,
                         attribute->wrapT ? GX_REPEAT : GX_CLAMP, GX_FALSE);
            break;
        case HSF_BMPFMT_RGB565:
            GXInitTexObj(&texObj, bitmap->data, bitmap->sizeX, bitmap->sizeY,
                         GX_TF_RGB565, attribute->wrapS ? GX_REPEAT : GX_CLAMP,
                         attribute->wrapT ? GX_REPEAT : GX_CLAMP, GX_FALSE);
            break;
        case HSF_BMPFMT_RGB5A3:
            GXInitTexObj(&texObj, bitmap->data, bitmap->sizeX, bitmap->sizeY,
                         GX_TF_RGB5A3, attribute->wrapS ? GX_REPEAT : GX_CLAMP,
                         attribute->wrapT ? GX_REPEAT : GX_CLAMP, GX_FALSE);
            break;
        case HSF_BMPFMT_I4:
            GXInitTexObj(&texObj, bitmap->data, bitmap->sizeX, bitmap->sizeY,
                         GX_TF_I4, attribute->wrapS ? GX_REPEAT : GX_CLAMP,
                         attribute->wrapT ? GX_REPEAT : GX_CLAMP, GX_FALSE);
            break;
        case HSF_BMPFMT_I8:
            GXInitTexObj(&texObj, bitmap->data, bitmap->sizeX, bitmap->sizeY,
                         GX_TF_I8, attribute->wrapS ? GX_REPEAT : GX_CLAMP,
                         attribute->wrapT ? GX_REPEAT : GX_CLAMP, GX_FALSE);
            break;
        case HSF_BMPFMT_IA4:
            GXInitTexObj(&texObj, bitmap->data, bitmap->sizeX, bitmap->sizeY,
                         GX_TF_IA4, attribute->wrapS ? GX_REPEAT : GX_CLAMP,
                         attribute->wrapT ? GX_REPEAT : GX_CLAMP, GX_FALSE);
            break;
        case HSF_BMPFMT_IA8:
            GXInitTexObj(&texObj, bitmap->data, bitmap->sizeX, bitmap->sizeY,
                         GX_TF_IA8, attribute->wrapS ? GX_REPEAT : GX_CLAMP,
                         attribute->wrapT ? GX_REPEAT : GX_CLAMP, GX_FALSE);
            break;
        case HSF_BMPFMT_CMPR:
            GXInitTexObj(&texObj, bitmap->data, bitmap->sizeX, bitmap->sizeY,
                         GX_TF_CMPR, attribute->wrapS ? GX_REPEAT : GX_CLAMP,
                         attribute->wrapT ? GX_REPEAT : GX_CLAMP, GX_FALSE);
            break;
        case HSF_BMPFMT_CI_RGB565:
            indexed = TRUE;
            tlutFormat = GX_TL_RGB565;
            ciFormat = bitmap->pixSize < 8 ? GX_TF_C4 : GX_TF_C8;
            break;
        case HSF_BMPFMT_CI_RGB5A3:
            indexed = TRUE;
            tlutFormat = GX_TL_RGB5A3;
            ciFormat = bitmap->pixSize < 8 ? GX_TF_C4 : GX_TF_C8;
            break;
        case HSF_BMPFMT_CI_IA8:
            indexed = TRUE;
            tlutFormat = GX_TL_IA8;
            ciFormat = bitmap->pixSize < 8 ? GX_TF_C4 : GX_TF_C8;
            break;
        default:
            return;
    }
    if (indexed) {
        if (!bitmap->palData || bitmap->palSize <= 0) {
            return;
        }
        GXInitTlutObj(&tlutObj, bitmap->palData, tlutFormat,
                      (u16)bitmap->palSize);
        GXLoadTlut(&tlutObj, 0);
        GXInitTexObjCI(&texObj, bitmap->data, bitmap->sizeX, bitmap->sizeY,
                       ciFormat, attribute->wrapS ? GX_REPEAT : GX_CLAMP,
                       attribute->wrapT ? GX_REPEAT : GX_CLAMP, GX_FALSE, 0);
    }
    GXLoadTexObj(&texObj, GX_TEXMAP0);
    g_hsfTexLoaded++;
}

static void SwitchHsfEmitIndex(const HSFOBJECT *object, const s16 *index,
                               BOOL useVertexColor,
                               const HSFATTRIBUTE *attribute) {
    s32 vertexIndex = index[0];
    s32 normalIndex = index[1];
    if (!object->mesh.vertex || vertexIndex < 0 ||
        vertexIndex >= object->mesh.vertex->count) {
        GXPosition3f32(0.0f, 0.0f, 0.0f);
        return;
    }
    if (object->mesh.normal && object->mesh.normal->data && normalIndex >= 0 &&
        normalIndex < object->mesh.normal->count) {
        if (object->mesh.file[1]) {
            const Vec *normal = (const Vec *)object->mesh.file[1];
            GXNormal3f32(normal[normalIndex].x, normal[normalIndex].y,
                         normal[normalIndex].z);
        } else {
            /* Static HSF normals use the compact signed 8-bit form. */
            const HSFS8VEC *normal = (const HSFS8VEC *)object->mesh.normal->data;
            GXNormal3f32((float)normal[normalIndex].x / 127.0f,
                         (float)normal[normalIndex].y / 127.0f,
                         (float)normal[normalIndex].z / 127.0f);
        }
    } else {
        GXNormal3f32(0.0f, 0.0f, 1.0f);
    }
    {
        const HuVecF *vertex = object->mesh.file[0] ?
            (const HuVecF *)object->mesh.file[0] :
            (const HuVecF *)object->mesh.vertex->data;
        GXPosition3f32(vertex[vertexIndex].x, vertex[vertexIndex].y,
                       vertex[vertexIndex].z);
    }
    if (useVertexColor && object->mesh.color && object->mesh.color->data &&
        index[2] >= 0 && index[2] < object->mesh.color->count) {
        const GXColor *color = (const GXColor *)object->mesh.color->data;
        GXColor4u8(color[index[2]].r, color[index[2]].g,
                   color[index[2]].b, color[index[2]].a);
    }
    if (object->mesh.st && object->mesh.st->data &&
        index[3] >= 0 && index[3] < object->mesh.st->count) {
        const HuVec2f *st = (const HuVec2f *)object->mesh.st->data;
        float u = st[index[3]].x;
        float v = st[index[3]].y;
        if (attribute) {
            if (attribute->scale.x != 0.0f) {
                u /= attribute->scale.x;
            }
            if (attribute->scale.y != 0.0f) {
                v /= attribute->scale.y;
            }
            u -= attribute->trans.x;
            v -= attribute->trans.y;
        }
        GXTexCoord2f32(u, v);
    }
}

static void SwitchHsfEmitVertex(const HSFOBJECT *object, const HSFFACE *face,
                                s32 corner, BOOL useVertexColor,
                                const HSFATTRIBUTE *attribute) {
    SwitchHsfEmitIndex(object, face->indices[corner], useVertexColor,
                       attribute);
}

static void SwitchHsfRenderFaces(const HSFOBJECT *object, s16 materialCount,
                                 u32 modelAttr) {
    HSFBUFFER *faceBuffer = object->mesh.face;
    HSFFACE *faces;
    s32 i;
    s32 currentType = -1;
    s16 currentMat = -1;
    BOOL useVertexColor = FALSE;
    const HSFATTRIBUTE *textureAttribute = NULL;
    s32 vertices = 0;
    BOOL open = FALSE;

    if (!faceBuffer || !faceBuffer->data || faceBuffer->count <= 0) {
        return;
    }
    faces = (HSFFACE *)faceBuffer->data;
    for (i = 0; i < faceBuffer->count; i++) {
        HSFFACE *face = &faces[i];
        s32 type = face->type & HSF_FACE_MASK;
        s32 needed;
        s32 corner;
        if (type == HSF_FACE_QUAD) {
            needed = 4;
        } else if (type == HSF_FACE_TRI) {
            needed = 3;
            type = HSF_FACE_TRI;
        } else if (type == HSF_FACE_TRISTRIP) {
            if (face->strip.count <= 0 || !face->strip.data) {
                continue;
            }
            needed = face->strip.count + 3;
            if (needed > 60) {
                continue;
            }
        } else {
            continue;
        }
        if (open && (type != currentType || face->mat != currentMat ||
                     vertices + needed > 60)) {
            GXEnd();
            open = FALSE;
            vertices = 0;
        }
        if (!open) {
            currentType = type;
            currentMat = face->mat;
            SwitchHsfSetMaterial(object, currentMat & 0x0FFF, materialCount,
                                 modelAttr);
            SwitchHsfSetTexture(object, currentMat & 0x0FFF, materialCount);
            textureAttribute = SwitchHsfMaterialAttribute(
                object, currentMat & 0x0FFF, materialCount);
            useVertexColor = FALSE;
            if (object->mesh.material && currentMat >= 0 &&
                (currentMat & 0x0FFF) < materialCount &&
                (currentMat & 0x0FFF) < 0x1000) {
                useVertexColor =
                    object->mesh.material[currentMat & 0x0FFF].vtxMode == 5;
            }
            GXBegin(type == HSF_FACE_QUAD ? GX_QUADS :
                    (type == HSF_FACE_TRISTRIP ? GX_TRIANGLESTRIP : GX_TRIANGLES),
                    GX_VTXFMT0, 0);
            open = TRUE;
        }
        if (type == HSF_FACE_TRISTRIP) {
            static const s32 firstCorner[3] = {0, 2, 1};
            s16 *strip = face->strip.data;
            for (corner = 0; corner < 3; corner++) {
                SwitchHsfEmitVertex(object, face, firstCorner[corner],
                                    useVertexColor, textureAttribute);
            }
            for (corner = 0; corner < face->strip.count; corner++) {
                SwitchHsfEmitIndex(object, strip + corner * 4,
                                   useVertexColor, textureAttribute);
            }
        } else {
            for (corner = 0; corner < needed; corner++) {
                SwitchHsfEmitVertex(object, face, corner, useVertexColor,
                                    textureAttribute);
            }
        }
        vertices += needed;
    }
    if (open) {
        GXEnd();
    }
}

void Hu3DDraw(HU3DMODEL *modelP, Mtx mtx, Vec *scale) {
    HSFDATA *model;
    s32 i;
    (void)scale;
    if (!modelP || !modelP->hsf) {
        return;
    }
    model = modelP->hsf;
    s_hsfObjectBase = model->object;
    s_hsfObjectCount = model->objectNum > 0 ? (u32)model->objectNum : 0;
    GXSet3DMode(TRUE);
    GXInvalidateTexAll();
    for (i = 0; i < model->objectNum; i++) {
        if (model->object[i].type == HSF_OBJ_MESH) {
            SwitchHsfUpdateEnvelope(model, &model->object[i]);
        }
    }
    for (i = 0; i < model->objectNum; i++) {
        HSFOBJECT *object = &model->object[i];
        Mtx objectMatrix;
        if (object->type != HSF_OBJ_MESH || !object->mesh.vertex ||
            !object->mesh.face) {
            continue;
        }
        SwitchHsfObjectMatrix(object, mtx, objectMatrix);
        GXLoadPosMtxImm(objectMatrix, 0);
        SwitchHsfRenderFaces(object, model->materialNum, modelP->attr);
    }
    GXSet3DMode(FALSE);
}

#endif
