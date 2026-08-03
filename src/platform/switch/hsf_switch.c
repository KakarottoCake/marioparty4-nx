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
} SwitchHsfContext;

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

        if (object->type == 7) {
            object->type = HSF_OBJ_CAMERA;
            continue;
        }
        if (object->type == 8) {
            object->type = HSF_OBJ_LIGHT;
            continue;
        }

        SwitchHsfCopyTransform(&object->mesh.base, raw + 0x1C);
        SwitchHsfCopyTransform(&object->mesh.curr, raw + 0x40);
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
        object->mesh.shapeNum = 0;
        object->mesh.shape = NULL;
        object->mesh.clusterNum = 0;
        object->mesh.cluster = NULL;
        /* Envelope data is not transformed yet.  Marking it empty keeps all
         * legacy callers on their existing no-op Switch stubs. */
        object->mesh.cenvNum = 0;
        object->mesh.cenv = NULL;
        object->mesh.file[0] = NULL;
        object->mesh.file[1] = NULL;

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
    return objects;
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
        !SwitchHsfEstimateBuffers(ctx, SWITCH_HSF_VERTEX, 12, total) ||
        !SwitchHsfEstimateNormals(ctx, total) ||
        !SwitchHsfEstimateBuffers(ctx, SWITCH_HSF_ST, 8, total) ||
        !SwitchHsfEstimateBuffers(ctx, SWITCH_HSF_COLOR, 4, total) ||
        !SwitchHsfEstimateFaces(ctx, total)) {
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
        if (children > SWITCH_HSF_MAX_CHILDREN ||
            !SwitchHsfEstimateAdd(total, children, sizeof(HSFOBJECT *))) {
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
    u32 estimatedSize;
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
        OSReport("HSF Switch: size estimate fallback\n");
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
    model->cenvNum = 0;
    model->skeletonNum = 0;
    model->clusterNum = 0;
    model->partNum = 0;
    model->shapeNum = 0;
    model->mapAttrNum = 0;
    model->motionNum = 0;
    model->matrixNum = 0;
    model->object = SwitchHsfParseObjects(&ctx, model);
    if (ctx.header.section[SWITCH_HSF_OBJECT].count && !model->object) {
        goto fail;
    }

    OSReport("HSF Switch: static model objects=%d arena=%u/%u\n",
             model->objectNum, ctx.arena.used, arenaSize);
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

static void SwitchHsfObjectLocal(const HSFOBJECT *object, Mtx out) {
    PSMTXIdentity(out);
    mtxRot(out, object->mesh.base.rot.x, object->mesh.base.rot.y,
           object->mesh.base.rot.z);
    mtxScaleCat(out, object->mesh.base.scale.x, object->mesh.base.scale.y,
                object->mesh.base.scale.z);
    mtxTransCat(out, object->mesh.base.pos.x, object->mesh.base.pos.y,
                object->mesh.base.pos.z);
}

static void SwitchHsfObjectMatrix(const HSFOBJECT *object, Mtx model, Mtx out) {
    const HSFOBJECT *chain[SWITCH_HSF_MAX_DEPTH];
    const HSFOBJECT *current = object;
    s32 count = 0;
    s32 i;
    PSMTXCopy(model, out);
    while (current && count < SWITCH_HSF_MAX_DEPTH) {
        chain[count++] = current;
        current = current->mesh.parent;
    }
    for (i = count - 1; i >= 0; i--) {
        Mtx local;
        SwitchHsfObjectLocal(chain[i], local);
        PSMTXConcat(out, local, out);
    }
}

static void SwitchHsfSetMaterial(const HSFOBJECT *object, s16 matIndex,
                                 s16 materialCount) {
    GXColor color = {255, 255, 255, 255};
    if (object->mesh.material && matIndex >= 0 &&
        matIndex < materialCount && matIndex < 0x1000 &&
        object->mesh.material[matIndex].color[0] != 0) {
        const HSFMATERIAL *material = &object->mesh.material[matIndex];
        color.r = material->color[0];
        color.g = material->color[1];
        color.b = material->color[2];
    }
    GXSetChanMatColor(GX_COLOR0A0, color);
}

static void SwitchHsfSetTexture(const HSFOBJECT *object, s16 matIndex,
                                s16 materialCount) {
    GXTexObj texObj;
    GXTlutObj tlutObj;
    HSFMATERIAL *material;
    HSFATTRIBUTE *attribute;
    HSFBITMAP *bitmap;
    GXTlutFmt tlutFormat;
    GXCITexFmt ciFormat;
    u32 attrIndex;
    BOOL indexed = FALSE;

    GXInvalidateTexAll();
    if (!object || !object->mesh.material || !object->mesh.attribute ||
        matIndex < 0 || matIndex >= materialCount || matIndex >= 0x1000) {
        return;
    }
    material = &object->mesh.material[matIndex];
    if (material->attrNum == 0 || !material->attr ||
        material->attr[0] < 0) {
        return;
    }
    attrIndex = (u32)material->attr[0];
    if (attrIndex >= 0x1000) {
        return;
    }
    attribute = &object->mesh.attribute[attrIndex];
    bitmap = attribute->bitmap;
    if (!bitmap || !bitmap->data || bitmap->sizeX <= 0 || bitmap->sizeY <= 0) {
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
}

static void SwitchHsfEmitIndex(const HSFOBJECT *object, const s16 *index) {
    s32 vertexIndex = index[0];
    if (!object->mesh.vertex || vertexIndex < 0 ||
        vertexIndex >= object->mesh.vertex->count) {
        GXPosition3f32(0.0f, 0.0f, 0.0f);
        return;
    }
    {
        const HuVecF *vertex = (const HuVecF *)object->mesh.vertex->data;
        GXPosition3f32(vertex[vertexIndex].x, vertex[vertexIndex].y,
                       vertex[vertexIndex].z);
    }
    if (object->mesh.st && object->mesh.st->data &&
        index[2] >= 0 && index[2] < object->mesh.st->count) {
        const HuVec2f *st = (const HuVec2f *)object->mesh.st->data;
        GXTexCoord2f32(st[index[2]].x, st[index[2]].y);
    }
}

static void SwitchHsfEmitVertex(const HSFOBJECT *object, const HSFFACE *face,
                                s32 corner) {
    SwitchHsfEmitIndex(object, face->indices[corner]);
}

static void SwitchHsfRenderFaces(const HSFOBJECT *object, s16 materialCount) {
    HSFBUFFER *faceBuffer = object->mesh.face;
    HSFFACE *faces;
    s32 i;
    s32 currentType = -1;
    s16 currentMat = -1;
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
            SwitchHsfSetMaterial(object, currentMat & 0x0FFF, materialCount);
            SwitchHsfSetTexture(object, currentMat & 0x0FFF, materialCount);
            GXBegin(type == HSF_FACE_QUAD ? GX_QUADS :
                    (type == HSF_FACE_TRISTRIP ? GX_TRIANGLESTRIP : GX_TRIANGLES),
                    GX_VTXFMT0, 0);
            open = TRUE;
        }
        if (type == HSF_FACE_TRISTRIP) {
            static const s32 firstCorner[3] = {0, 2, 1};
            s16 *strip = face->strip.data;
            for (corner = 0; corner < 3; corner++) {
                SwitchHsfEmitVertex(object, face, firstCorner[corner]);
            }
            for (corner = 0; corner < face->strip.count; corner++) {
                SwitchHsfEmitIndex(object, strip + corner * 4);
            }
        } else {
            for (corner = 0; corner < needed; corner++) {
                SwitchHsfEmitVertex(object, face, corner);
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
    GXSet3DMode(TRUE);
    GXInvalidateTexAll();
    for (i = 0; i < model->objectNum; i++) {
        HSFOBJECT *object = &model->object[i];
        Mtx objectMatrix;
        if (object->type != HSF_OBJ_MESH || !object->mesh.vertex ||
            !object->mesh.face) {
            continue;
        }
        SwitchHsfObjectMatrix(object, mtx, objectMatrix);
        GXLoadPosMtxImm(objectMatrix, 0);
        SwitchHsfRenderFaces(object, model->materialNum);
    }
    GXSet3DMode(FALSE);
}

#endif
