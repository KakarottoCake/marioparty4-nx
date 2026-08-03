#include "game/sprite.h"
#include "game/disp.h"
#include "game/memory.h"
#include "game/init.h"

#include "dolphin/mtx.h"

#ifndef __MWERKS__
#include "game/hu3d.h"
#endif

#define SPRITE_DIRTY_ATTR 0x1
#define SPRITE_DIRTY_XFORM 0x2
#define SPRITE_DIRTY_COLOR 0x4

typedef struct sprite_order {
    u16 group;
    u16 sprite;
    u16 prio;
    u16 next;
} SpriteOrder;

HUSPRITE HuSprData[HUSPR_MAX];
HUSPRGRP HuSprGrpData[HUSPR_GRP_MAX];
static SpriteOrder HuSprOrder[HUSPR_MAX*2];

static s16 HuSprOrderNum;
static s16 HuSprOrderNo;
static BOOL HuSprPauseF;

static void HuSprOrderEntry(s16 group, s16 sprite);


void HuSprInit(void)
{
    s16 i;
    HUSPRITE *sprite;
    HUSPRGRP *group;
    for(sprite = &HuSprData[1], i=1; i<HUSPR_MAX; i++, sprite++) {
        sprite->data = NULL;
    }
    for(group = HuSprGrpData, i=0; i<HUSPR_GRP_MAX; i++, group++) {
        group->capacity = 0;
    }
    sprite = &HuSprData[0];
    sprite->prio = 0;
    sprite->data = (void *)1;
    HuSprPauseF = FALSE;
}

void HuSprClose(void)
{
    s16 i;
    HUSPRGRP *group;
    HUSPRITE *sprite;
    
    for(group = HuSprGrpData, i=0; i<HUSPR_GRP_MAX; i++, group++) {
        if(group->capacity != 0) {
            HuSprGrpKill(i);
        }
    }
    for(sprite = &HuSprData[1], i=1; i<HUSPR_MAX; i++, sprite++) {
        if(sprite->data) {
            HuSprKill(i);
        }
    }
    HuSprPauseF = FALSE;
}

void HuSprExec(s16 draw_no)
{
    HUSPRITE *sprite;
    while(sprite = HuSprCall()) {
        if(!(sprite->attr & HUSPR_ATTR_DISPOFF) && sprite->drawNo == draw_no) {
            HuSprDisp(sprite);
        }
    }
}

void HuSprBegin(void)
{
    Mtx temp, rot;
    s16 i, j;
    Vec axis = {0, 0, 1};
    HUSPRGRP *group;
    group = HuSprGrpData;
    HuSprOrderNum = 1;
    HuSprOrder[0].next = 0;
    HuSprOrder[0].prio = -1;
    for(i=0; i<HUSPR_GRP_MAX; i++, group++) {
        if(group->capacity != 0) {
            MTXTrans(temp, group->center.x*group->scale.x, group->center.y*group->scale.y, 0.0f);
            MTXRotAxisDeg(rot, &axis, group->zRot);
            MTXConcat(rot, temp, group->mtx);
            MTXScale(temp, group->scale.x, group->scale.y, 1.0f);
            MTXConcat(group->mtx, temp, group->mtx);
            mtxTransCat(group->mtx, group->pos.x, group->pos.y, 0);
            for(j=0; j<group->capacity; j++) {
                if(group->members[j] != -1) {
                    HuSprOrderEntry(i, group->members[j]);
                }
            }
        }
    }
    HuSprOrderNo = 0;
}

static void HuSprOrderEntry(s16 group, s16 sprite)
{
    SpriteOrder *order = &HuSprOrder[HuSprOrderNum];
    s16 prio = HuSprData[sprite].prio;
    s16 prev, next;
    if(HuSprOrderNum >= HUSPR_MAX*2) {
        OSReport("Order Max Over!\n");
        return;
    }
    next = HuSprOrder[0].next;
    for(prev = 0; next != 0; prev = next, next = HuSprOrder[next].next) {
        if(HuSprOrder[next].prio < prio) {
            break;
        }
    }
    order->next = HuSprOrder[prev].next;
    HuSprOrder[prev].next = HuSprOrderNum;
    order->prio = prio;
    order->group = group;
    order->sprite = sprite;
    HuSprOrderNum++;
}

HUSPRITE *HuSprCall(void)
{
    HuSprOrderNo = HuSprOrder[HuSprOrderNo].next;
    if(HuSprOrderNo != 0) {
        SpriteOrder *order = &HuSprOrder[HuSprOrderNo];
        HUSPRITE *sprite = &HuSprData[order->sprite];
        sprite->groupMtx = &HuSprGrpData[order->group].mtx;
        if(sprite->attr & HUSPR_ATTR_FUNC) {
            return sprite;
        }
        if(!sprite->data || (uintptr_t)sprite->data < 0x1000) {
            sprite->frameP = NULL;
            sprite->patP = NULL;
            return sprite;
        }
        sprite->frameP = &sprite->data->bank[sprite->bank].frame[sprite->animNo];
        sprite->patP = &sprite->data->pat[sprite->frameP->pat];
        return sprite;
    } else {
        return NULL;
    }
}

static inline void SpriteCalcFrame(HUSPRITE *sprite, ANIMBANK *bank, ANIMFRAME **frame, s16 loop)
{
    if(sprite->time >= (*frame)->time) {
        sprite->animNo++;
        sprite->time -= (*frame)->time;
        if(sprite->animNo >= bank->timeNum || (*frame)[1].time == -1) {
            if(loop) {
                sprite->animNo = 0;
            } else {
                sprite->animNo = bank->timeNum-1;
            }
        }
        *frame = &bank->frame[sprite->animNo];
    } else if(sprite->time < 0) {
        sprite->animNo--;
        if(sprite->animNo < 0) {
            if(loop) {
                sprite->animNo = bank->timeNum-1;
            } else {
                sprite->animNo = 0;
            }
        }
        *frame = &bank->frame[sprite->animNo];
        sprite->time += (*frame)->time;
    }
}

void HuSprFinish(void)
{
    ANIMDATA *anim;
    ANIMBANK *bank;
    ANIMFRAME *frame;
    HUSPRITE *sprite;
    s16 i;
    s16 j;
    s16 loop;
    s16 dir;
    
    for(sprite = &HuSprData[1], i=1; i<HUSPR_MAX; i++, sprite++) {
        if(sprite->data && (uintptr_t)sprite->data >= 0x1000 &&
           !(sprite->attr & HUSPR_ATTR_FUNC)) {
            if(!HuSprPauseF || (sprite->attr & HUSPR_ATTR_NOPAUSE)) {
                anim = sprite->data;
                bank = &anim->bank[sprite->bank];
                frame = &bank->frame[sprite->animNo];
                loop = (sprite->attr & HUSPR_ATTR_LOOP) ? 0 : 1;
                if(!(sprite->attr & HUSPR_ATTR_NOANIM)) {
                    dir = (sprite->attr & HUSPR_ATTR_REVERSE) ? -1 : 1;
                    for(j=0; j<(s32)sprite->speed*minimumVcount; j++) {
                        sprite->time += dir;
                        SpriteCalcFrame(sprite, bank, &frame, loop);
                    }
                    sprite->time += (sprite->speed*(float)minimumVcount)-j;
                    SpriteCalcFrame(sprite, bank, &frame, loop);
                }
                sprite->dirty = 0;
            }
        }
    }
}

void HuSprPauseSet(BOOL value)
{
    HuSprPauseF = value;
}

#ifdef __SWITCH__
/*
 * ANIM files were laid out for a 32-bit big-endian GameCube.  The public
 * ANIMDATA structs contain real pointers, so casting an ANIM file directly to
 * those structs on the 64-bit Switch shifts every field after the first
 * pointer.  Rebuild the file into native structs instead.
 */
#define SWITCH_ANIM_MAGIC 0x53414E49u /* "SANI" */
#define SWITCH_ANIM_MAX_ITEMS 4096
#define SWITCH_ANIM_MAX_BYTES (64u * 1024u * 1024u)

typedef struct SwitchAnimHeader_s {
    u32 magic;
    u32 reserved;
} SwitchAnimHeader;

static u16 SwitchAnimBE16(const u8 *p)
{
    return (u16)(((u16)p[0] << 8) | p[1]);
}

static s16 SwitchAnimBES16(const u8 *p)
{
    return (s16)SwitchAnimBE16(p);
}

static u32 SwitchAnimBE32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | p[3];
}

static size_t SwitchAnimAlign(size_t value)
{
    return (value + 7u) & ~7u;
}

static SwitchAnimHeader *SwitchAnimHeaderGet(ANIMDATA *anim)
{
    SwitchAnimHeader *header;
    if (!anim || (uintptr_t)anim < sizeof(SwitchAnimHeader)) {
        return NULL;
    }
    header = (SwitchAnimHeader *)((u8 *)anim - sizeof(SwitchAnimHeader));
    return header->magic == SWITCH_ANIM_MAGIC ? header : NULL;
}

static BOOL SwitchAnimRangeOK(const u8 *base, u32 offset, u32 size)
{
    (void)base;
    return offset != 0 && offset < SWITCH_ANIM_MAX_BYTES &&
           size <= SWITCH_ANIM_MAX_BYTES - offset;
}

static void SwitchAnimRawFree(void *data)
{
    uintptr_t address;
    s32 i;
    if (!data) {
        return;
    }
    address = (uintptr_t)data;
    /* Reflection/toon ANIM data is linked into the executable.  Only release
     * buffers that are inside one of the game's managed heaps. */
    for (i = 0; i < HEAP_MAX; i++) {
        uintptr_t start = (uintptr_t)HuMemHeapPtrGet((HeapID)i);
        uintptr_t end = start + HuMemHeapSizeGet((HeapID)i);
        if (start != 0 && address > start && address < end) {
            HuMemDirectFree(data);
            return;
        }
    }
}

static ANIMDATA *SwitchAnimRead(void *data)
{
    const u8 *raw = (const u8 *)data;
    const u8 *raw_bank;
    const u8 *raw_pat;
    const u8 *raw_bmp;
    ANIMDATA *anim;
    ANIMBANK *bank;
    ANIMPAT *pat;
    ANIMBMP *bmp;
    ANIMFRAME *frames;
    ANIMLAYER *layers;
    SwitchAnimHeader *header;
    s16 bank_num;
    s16 pat_num;
    s16 bmp_num_raw;
    s16 bmp_num;
    s32 frame_count = 0;
    s32 layer_count = 0;
    size_t size;
    size_t cursor;
    s16 i;

    if (!raw) {
        return NULL;
    }

    bank_num = SwitchAnimBES16(raw + 0);
    pat_num = SwitchAnimBES16(raw + 2);
    bmp_num_raw = SwitchAnimBES16(raw + 4);
    bmp_num = bmp_num_raw & ANIM_BMP_NUM_MASK;
    if (bank_num < 0 || bank_num > SWITCH_ANIM_MAX_ITEMS ||
        pat_num < 0 || pat_num > SWITCH_ANIM_MAX_ITEMS ||
        bmp_num < 0 || bmp_num > SWITCH_ANIM_MAX_ITEMS) {
        OSReport("sprman: invalid ANIM counts (%d,%d,%d)\n",
                 bank_num, pat_num, bmp_num);
        SwitchAnimRawFree(data);
        return NULL;
    }

    {
        u32 bank_offset = SwitchAnimBE32(raw + 8);
        u32 pat_offset = SwitchAnimBE32(raw + 12);
        u32 bmp_offset = SwitchAnimBE32(raw + 16);
        if ((bank_num && !SwitchAnimRangeOK(raw, bank_offset,
                                            (u32)bank_num * 8u)) ||
            (pat_num && !SwitchAnimRangeOK(raw, pat_offset,
                                           (u32)pat_num * 16u)) ||
            (bmp_num && !SwitchAnimRangeOK(raw, bmp_offset,
                                           (u32)bmp_num * 20u))) {
            OSReport("sprman: invalid ANIM table offsets\n");
            SwitchAnimRawFree(data);
            return NULL;
        }
        raw_bank = raw + bank_offset;
        raw_pat = raw + pat_offset;
        raw_bmp = raw + bmp_offset;
    }

    for (i = 0; i < bank_num; i++) {
        s16 time_num = SwitchAnimBES16(raw_bank + i * 8);
        if (time_num < 0 || time_num > SWITCH_ANIM_MAX_ITEMS ||
            (time_num && !SwitchAnimRangeOK(raw,
                SwitchAnimBE32(raw_bank + i * 8 + 4), (u32)time_num * 12u))) {
            OSReport("sprman: invalid ANIM bank %d\n", i);
            SwitchAnimRawFree(data);
            return NULL;
        }
        frame_count += time_num;
        if (frame_count > SWITCH_ANIM_MAX_ITEMS * SWITCH_ANIM_MAX_ITEMS) {
            OSReport("sprman: ANIM frame count too large\n");
            SwitchAnimRawFree(data);
            return NULL;
        }
    }
    for (i = 0; i < pat_num; i++) {
        s16 layer_num = SwitchAnimBES16(raw_pat + i * 16);
        if (layer_num < 0 || layer_num > SWITCH_ANIM_MAX_ITEMS ||
            (layer_num && !SwitchAnimRangeOK(raw,
                SwitchAnimBE32(raw_pat + i * 16 + 12), (u32)layer_num * 32u))) {
            OSReport("sprman: invalid ANIM pattern %d\n", i);
            SwitchAnimRawFree(data);
            return NULL;
        }
        layer_count += layer_num;
        if (layer_count > SWITCH_ANIM_MAX_ITEMS * SWITCH_ANIM_MAX_ITEMS) {
            OSReport("sprman: ANIM layer count too large\n");
            SwitchAnimRawFree(data);
            return NULL;
        }
    }

    size = sizeof(SwitchAnimHeader) + sizeof(ANIMDATA);
    size = SwitchAnimAlign(size) + (size_t)bank_num * sizeof(ANIMBANK);
    size = SwitchAnimAlign(size) + (size_t)pat_num * sizeof(ANIMPAT);
    size = SwitchAnimAlign(size) + (size_t)bmp_num * sizeof(ANIMBMP);
    size = SwitchAnimAlign(size) + (size_t)frame_count * sizeof(ANIMFRAME);
    size = SwitchAnimAlign(size) + (size_t)layer_count * sizeof(ANIMLAYER);

    for (i = 0; i < bmp_num; i++) {
        u32 data_size = SwitchAnimBE32(raw_bmp + i * 20 + 8);
        u16 pal_num = SwitchAnimBE16(raw_bmp + i * 20 + 2);
        u32 data_offset = SwitchAnimBE32(raw_bmp + i * 20 + 16);
        u32 pal_offset = SwitchAnimBE32(raw_bmp + i * 20 + 12);
        if (data_size && !SwitchAnimRangeOK(raw, data_offset, data_size)) {
            OSReport("sprman: invalid ANIM bitmap data %d\n", i);
            SwitchAnimRawFree(data);
            return NULL;
        }
        if (pal_num && !SwitchAnimRangeOK(raw, pal_offset, (u32)pal_num * 2u)) {
            OSReport("sprman: invalid ANIM palette %d\n", i);
            SwitchAnimRawFree(data);
            return NULL;
        }
        size = SwitchAnimAlign(size) + data_size;
        size = SwitchAnimAlign(size) + (size_t)pal_num * 2u;
        if (size > SWITCH_ANIM_MAX_BYTES) {
            OSReport("sprman: ANIM allocation too large\n");
            SwitchAnimRawFree(data);
            return NULL;
        }
    }

    header = HuMemDirectMalloc(HEAP_MODEL, (s32)size);
    if (!header) {
        SwitchAnimRawFree(data);
        return NULL;
    }
    memset(header, 0, size);
    header->magic = SWITCH_ANIM_MAGIC;
    anim = (ANIMDATA *)(header + 1);
    anim->bankNum = bank_num;
    anim->patNum = pat_num;
    anim->bmpNum = bmp_num;
    anim->useNum = 0;

    cursor = SwitchAnimAlign((size_t)((u8 *)(anim + 1) - (u8 *)header));
    bank = (ANIMBANK *)((u8 *)header + cursor);
    cursor += (size_t)bank_num * sizeof(ANIMBANK);
    cursor = SwitchAnimAlign(cursor);
    pat = (ANIMPAT *)((u8 *)header + cursor);
    cursor += (size_t)pat_num * sizeof(ANIMPAT);
    cursor = SwitchAnimAlign(cursor);
    bmp = (ANIMBMP *)((u8 *)header + cursor);
    cursor += (size_t)bmp_num * sizeof(ANIMBMP);
    cursor = SwitchAnimAlign(cursor);
    frames = (ANIMFRAME *)((u8 *)header + cursor);
    cursor += (size_t)frame_count * sizeof(ANIMFRAME);
    cursor = SwitchAnimAlign(cursor);
    layers = (ANIMLAYER *)((u8 *)header + cursor);
    cursor += (size_t)layer_count * sizeof(ANIMLAYER);
    cursor = SwitchAnimAlign(cursor);

    anim->bank = bank;
    anim->pat = pat;
    anim->bmp = bmp;

    {
        s32 frame_index = 0;
        for (i = 0; i < bank_num; i++) {
            const u8 *src = raw_bank + i * 8;
            s16 time_num = SwitchAnimBES16(src + 0);
            const u8 *src_frame = raw + SwitchAnimBE32(src + 4);
            s16 j;
            bank[i].timeNum = time_num;
            bank[i].unk = SwitchAnimBES16(src + 2);
            bank[i].frame = frames + frame_index;
            for (j = 0; j < time_num; j++) {
                const u8 *f = src_frame + j * 12;
                frames[frame_index].pat = SwitchAnimBES16(f + 0);
                frames[frame_index].time = SwitchAnimBES16(f + 2);
                frames[frame_index].shiftX = SwitchAnimBES16(f + 4);
                frames[frame_index].shiftY = SwitchAnimBES16(f + 6);
                frames[frame_index].flip = SwitchAnimBES16(f + 8);
                frames[frame_index].pad = SwitchAnimBES16(f + 10);
                frame_index++;
            }
        }
    }

    for (i = 0; i < pat_num; i++) {
        const u8 *src = raw_pat + i * 16;
        s16 layer_num = SwitchAnimBES16(src + 0);
        const u8 *src_layer = raw + SwitchAnimBE32(src + 12);
        s16 j;
        pat[i].layerNum = layer_num;
        pat[i].centerX = SwitchAnimBES16(src + 2);
        pat[i].centerY = SwitchAnimBES16(src + 4);
        pat[i].sizeX = SwitchAnimBES16(src + 6);
        pat[i].sizeY = SwitchAnimBES16(src + 8);
        pat[i].layer = layers;
        for (j = 0; j < layer_num; j++) {
            const u8 *l = src_layer + j * 32;
            s16 k;
            layers->alpha = l[0];
            layers->flip = l[1];
            layers->bmpNo = SwitchAnimBES16(l + 2);
            layers->startX = SwitchAnimBES16(l + 4);
            layers->startY = SwitchAnimBES16(l + 6);
            layers->sizeX = SwitchAnimBES16(l + 8);
            layers->sizeY = SwitchAnimBES16(l + 10);
            layers->shiftX = SwitchAnimBES16(l + 12);
            layers->shiftY = SwitchAnimBES16(l + 14);
            for (k = 0; k < 8; k++) {
                layers->vtx[k] = SwitchAnimBES16(l + 16 + k * 2);
            }
            layers++;
        }
    }

    for (i = 0; i < bmp_num; i++) {
        const u8 *src = raw_bmp + i * 20;
        u16 pal_num = SwitchAnimBE16(src + 2);
        s16 size_x = SwitchAnimBES16(src + 4);
        s16 size_y = SwitchAnimBES16(src + 6);
        u32 data_size = SwitchAnimBE32(src + 8);
        u32 pal_offset = SwitchAnimBE32(src + 12);
        u32 data_offset = SwitchAnimBE32(src + 16);
        bmp[i].pixSize = src[0];
        bmp[i].dataFmt = src[1];
        bmp[i].palNum = (s16)pal_num;
        bmp[i].sizeX = size_x;
        bmp[i].sizeY = size_y;
        bmp[i].dataSize = data_size;
        if (data_size) {
            void *copy = (u8 *)header + cursor;
            cursor = SwitchAnimAlign(cursor + data_size);
            memcpy(copy, raw + data_offset, data_size);
            bmp[i].data = copy;
        }
        if (pal_num) {
            void *copy = (u8 *)header + cursor;
            cursor = SwitchAnimAlign(cursor + (size_t)pal_num * 2u);
            memcpy(copy, raw + pal_offset, (size_t)pal_num * 2u);
            bmp[i].palData = copy;
        }
    }

    SwitchAnimRawFree(data);
    return anim;
}
#endif

ANIMDATA *HuSprAnimRead(void *data)
{
#ifdef __SWITCH__
    ANIMDATA *converted = (ANIMDATA *)data;
    if (SwitchAnimHeaderGet(converted)) {
        converted->useNum++;
        return converted;
    }
    return SwitchAnimRead(data);
#else
    s16 i;
    ANIMBMP *bmp;
    ANIMBANK *bank;
    ANIMPAT *pat;
    
    ANIMDATA *anim = (ANIMDATA *)data;
    if((u32)anim->bank & 0xFFFF0000) {
        anim->useNum++;
        return anim;
    }
    bank = (ANIMBANK *)((u32)anim->bank+(u32)data);
    anim->bank = bank;
    pat = (ANIMPAT *)((u32)anim->pat+(u32)data);
    anim->pat = pat;
    bmp = (ANIMBMP *)((u32)anim->bmp+(u32)data);
    anim->bmp = bmp;
    for(i=0; i<anim->bankNum; i++, bank++) {
        bank->frame = (ANIMFRAME *)((u32)bank->frame+(u32)data);
    }
    for(i=0; i<anim->patNum; i++, pat++) {
        pat->layer = (ANIMLAYER *)((u32)pat->layer+(u32)data);
    }
    for(i=0; i<anim->bmpNum; i++, bmp++) {
        bmp->palData = (void *)((u32)bmp->palData+(u32)data);
        bmp->data = (void *)((u32)bmp->data+(u32)data);
    }
    anim->useNum = 0;
    return anim;
#endif
}

void HuSprAnimLock(ANIMDATA *anim)
{
    anim->useNum++;
}

s16 HuSprCreate(ANIMDATA *anim, s16 prio, s16 bank)
{
    HUSPRITE *sprite;
    s16 i;
    for(sprite = &HuSprData[1], i=1; i<HUSPR_MAX; i++, sprite++) {
        if(!sprite->data) {
            break;
        }
    }
    if(i == HUSPR_MAX) {
        return HUSPR_NONE;
    }
    sprite->data = anim;
    sprite->speed = 1.0f;
    sprite->animNo = 0;
    sprite->bank = bank;
    sprite->time = 0.0f;
    sprite->attr = 0;
    sprite->drawNo = 0;
    sprite->r = sprite->g = sprite->b = sprite->a = 255;
    sprite->pos.x = sprite->pos.y = sprite->zRot = 0.0f;
    sprite->prio = prio;
    sprite->scale.x = sprite->scale.y = 1.0f;
    sprite->wrapS = sprite->wrapT = GX_CLAMP;
    sprite->uvScaleX = sprite->uvScaleY = 1;
    sprite->bg = NULL;
    sprite->scissorX = sprite->scissorY = 0;
    sprite->scissorW = 640;
    sprite->scissorH = 480;
    if(anim) {
        HuSprAnimLock(anim);
    }
    return i;
}

s16 HuSprFuncCreate(HUSPRFUNC func, s16 prio)
{
    HUSPRITE *sprite;
    s16 index = HuSprCreate(NULL, prio, 0);
    if(index == HUSPR_NONE) {
        return HUSPR_NONE;
    }
    sprite = &HuSprData[index];
    sprite->func = func;
    sprite->attr |= HUSPR_ATTR_FUNC;
    return index;
}

s16 HuSprGrpCreate(s16 capacity)
{
    HUSPRGRP *group;
    s16 i, j;
    for(group = HuSprGrpData, i=0; i<HUSPR_GRP_MAX; i++, group++) {
        if(group->capacity == 0) {
            break;
        }
    }
    if(i == HUSPR_GRP_MAX) {
        return HUSPR_GRP_NONE;
    }
    group->members = HuMemDirectMalloc(HEAP_HEAP, sizeof(s16)*capacity);
    for(j=0; j<capacity; j++) {
        group->members[j] = HUSPR_NONE;
    }
    group->capacity = capacity;
    group->pos.x = group->pos.y = group->zRot = group->center.x = group->center.y = 0.0f;
    group->scale.x = group->scale.y = 1.0f;
    return i;
}

s16 HuSprGrpCopy(s16 group)
{
    HUSPRGRP *new_group_ptr;
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 new_group = HuSprGrpCreate(group_ptr->capacity);
    s16 i;
    if(new_group == HUSPR_GRP_NONE) {
        return HUSPR_GRP_NONE;
    }
    new_group_ptr = &HuSprGrpData[new_group];
    new_group_ptr->pos.x = group_ptr->pos.x;
    new_group_ptr->pos.y = group_ptr->pos.y;
    new_group_ptr->zRot = group_ptr->zRot;
    new_group_ptr->scale.x = group_ptr->scale.x;
    new_group_ptr->scale.y = group_ptr->scale.y;
    new_group_ptr->center.x = group_ptr->center.x;
    new_group_ptr->center.y = group_ptr->center.y;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HUSPRITE *old_sprite = &HuSprData[group_ptr->members[i]];
            s16 new_sprite = HuSprCreate(old_sprite->data, old_sprite->prio, old_sprite->bank);
            HuSprData[new_sprite] = *old_sprite;
            HuSprGrpMemberSet(new_group, i, new_sprite);
        }
    }
    return new_group;
}

void HuSprGrpMemberSet(s16 group, s16 member, s16 sprite)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    HUSPRITE *sprite_ptr = &HuSprData[sprite];
    if(group_ptr->capacity == 0 || group_ptr->capacity <= member || group_ptr->members[member] != HUSPR_NONE) {
        return;
    }
    group_ptr->members[member] = sprite;
}

void HuSprGrpMemberKill(s16 group, s16 member)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    if(group_ptr->capacity == 0 || group_ptr->capacity <= member || group_ptr->members[member] == HUSPR_NONE) {
        return;
    }
    HuSprKill(group_ptr->members[member]);
    group_ptr->members[member] = HUSPR_NONE;
}

void HuSprGrpKill(s16 group)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprKill(group_ptr->members[i]);
        }
    }
    group_ptr->capacity = 0;
    HuMemDirectFree(group_ptr->members);
}

void HuSprKill(s16 sprite)
{
    HUSPRITE *sprite_ptr = &HuSprData[sprite];
    if(!sprite_ptr->data) {
        return;
    }
    if(!(sprite_ptr->attr & HUSPR_ATTR_FUNC)) {
        HuSprAnimKill(sprite_ptr->data);
        if(sprite_ptr->bg) {
            HuSprAnimKill(sprite_ptr->bg);
            sprite_ptr->bg = NULL;
        }
    }
    sprite_ptr->data = NULL;
}

void HuSprAnimKill(ANIMDATA *anim)
{
#ifdef __SWITCH__
    if (!anim || (uintptr_t)anim < 0x1000) {
        return;
    }
    SwitchAnimHeader *header = SwitchAnimHeaderGet(anim);
    if (header) {
        if (--anim->useNum <= 0) {
            HuMemDirectFree(header);
        }
        return;
    }
#endif
    if(--anim->useNum <= 0) {
        if(anim->bmpNum & ANIM_BMP_ALLOC) {
            if(anim->bmp->data) {
                HuMemDirectFree(anim->bmp->data);
            }
            if(anim->bmp->palData) {
                HuMemDirectFree(anim->bmp->palData);
            }
        }
        HuMemDirectFree(anim);
    }
}

void HuSprAttrSet(s16 group, s16 member, s32 attr)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    HUSPRITE *sprite_ptr;
    if(group_ptr->capacity == 0 || group_ptr->capacity <= member || group_ptr->members[member] == HUSPR_NONE) {
        return;
    }
    sprite_ptr = &HuSprData[group_ptr->members[member]];
    sprite_ptr->attr |= attr;
    sprite_ptr->dirty |= SPRITE_DIRTY_ATTR;
}

void HuSprAttrReset(s16 group, s16 member, s32 attr)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    HUSPRITE *sprite_ptr;
    if(group_ptr->capacity == 0 || group_ptr->capacity <= member || group_ptr->members[member] == HUSPR_NONE) {
        return;
    }
    sprite_ptr = &HuSprData[group_ptr->members[member]];
    sprite_ptr->attr &= ~attr;
    sprite_ptr->dirty |= SPRITE_DIRTY_ATTR;
}

void HuSprPosSet(s16 group, s16 member, float x, float y)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->pos.x = x;
    sprite_ptr->pos.y = y;
    sprite_ptr->dirty |= SPRITE_DIRTY_XFORM;
}

void HuSprZRotSet(s16 group, s16 member, float z_rot)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->zRot = z_rot;
    sprite_ptr->dirty |= SPRITE_DIRTY_XFORM;
}

void HuSprScaleSet(s16 group, s16 member, float x, float y)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->scale.x = x;
    sprite_ptr->scale.y = y;
    sprite_ptr->dirty |= SPRITE_DIRTY_XFORM;
}

void HuSprTPLvlSet(s16 group, s16 member, float tp_lvl)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->a = tp_lvl*255;
    sprite_ptr->dirty |= SPRITE_DIRTY_COLOR;
}

void HuSprColorSet(s16 group, s16 member, u8 r, u8 g, u8 b)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->r = r;
    sprite_ptr->g = g;
    sprite_ptr->b = b;
    sprite_ptr->dirty |= SPRITE_DIRTY_COLOR;
}

void HuSprSpeedSet(s16 group, s16 member, float speed)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    HuSprData[group_ptr->members[member]].speed = speed;
}

void HuSprBankSet(s16 group, s16 member, s16 bank)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    ANIMDATA *anim = sprite_ptr->data;
    ANIMBANK *bank_ptr = &anim->bank[sprite_ptr->bank];
    ANIMFRAME *frame_ptr = &bank_ptr->frame[sprite_ptr->animNo];
    sprite_ptr->bank = bank;
    if(sprite_ptr->attr & HUSPR_ATTR_REVERSE) {
        sprite_ptr->animNo = bank_ptr->timeNum-1;
        frame_ptr = &bank_ptr->frame[sprite_ptr->animNo];
        sprite_ptr->time = frame_ptr->time;
    } else {
        sprite_ptr->time = 0;
        sprite_ptr->animNo = 0;
    }
}

void HuSprGrpPosSet(s16 group, float x, float y)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    group_ptr->pos.x = x;
    group_ptr->pos.y = y;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != -1) {
            HuSprData[group_ptr->members[i]].dirty |= SPRITE_DIRTY_XFORM;
        }
    }
}

void HuSprGrpCenterSet(s16 group, float x, float y)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    group_ptr->center.x = x;
    group_ptr->center.y = y;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprData[group_ptr->members[i]].dirty |= SPRITE_DIRTY_XFORM;
        }
    }
}

void HuSprGrpZRotSet(s16 group, float z_rot)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    group_ptr->zRot = z_rot;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprData[group_ptr->members[i]].dirty |= SPRITE_DIRTY_XFORM;
        }
    }
}

void HuSprGrpScaleSet(s16 group, float x, float y)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    group_ptr->scale.x = x;
    group_ptr->scale.y = y;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprData[group_ptr->members[i]].dirty |= SPRITE_DIRTY_XFORM;
        }
    }
}

void HuSprGrpTPLvlSet(s16 group, float tp_lvl)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprData[group_ptr->members[i]].a = tp_lvl*255;
            HuSprData[group_ptr->members[i]].dirty |= SPRITE_DIRTY_COLOR;
        }
    }
}

void HuSprGrpDrawNoSet(s16 group, s32 draw_no)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprData[group_ptr->members[i]].drawNo = draw_no;
        }
    }
}

void HuSprDrawNoSet(s16 group, s16 member, s32 draw_no)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->drawNo = draw_no;
}

void HuSprPriSet(s16 group, s16 member, s16 prio)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->prio = prio;
}

void HuSprGrpScissorSet(s16 group, s16 x, s16 y, s16 w, s16 h)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprScissorSet(group, i, x, y, w, h);
        }
    }
}

void HuSprScissorSet(s16 group, s16 member, s16 x, s16 y, s16 w, s16 h)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->scissorX = x;
    sprite_ptr->scissorY = y;
    sprite_ptr->scissorW = w;
    sprite_ptr->scissorH = h;
}

static s16 bitSizeTbl[11] = { 32, 24, 16, 8, 4, 16, 8, 8, 4, 8, 4 };

ANIMDATA *HuSprAnimMake(s16 sizeX, s16 sizeY, s16 dataFmt)
{
    ANIMLAYER *layer;
    ANIMBMP *bmp;
    ANIMDATA *anim;
    ANIMPAT *pat;
    ANIMFRAME *frame;
    void *temp;
    ANIMBANK *bank;
    ANIMDATA *new_anim;

    anim = new_anim = HuMemDirectMalloc(HEAP_MODEL, sizeof(ANIMDATA)+sizeof(ANIMBANK)+sizeof(ANIMFRAME)
                                            +sizeof(ANIMPAT)+sizeof(ANIMLAYER)+sizeof(ANIMBMP));

    bank = temp = &new_anim[1];
    anim->bank = bank;
    frame = temp = ((char *)temp+sizeof(ANIMBANK));
    bank->frame = frame;
    pat = temp = ((char *)temp+sizeof(ANIMFRAME));
    anim->pat = pat;
    layer = temp = ((char *)temp+sizeof(ANIMPAT));
    pat->layer = layer;
    bmp = temp = ((char *)temp+sizeof(ANIMLAYER));
    anim->bmp = bmp;
    anim->useNum = 0;
    anim->bankNum = 1;
    anim->patNum = 1;
    anim->bmpNum = (1|ANIM_BMP_ALLOC);
    bank->timeNum = 1;
    bank->unk = 10;
    frame->pat = 0;
    frame->time = 10;
    frame->shiftX = frame->shiftY = frame->flip = 0;
    pat->layerNum = 1;
    pat->centerX = sizeX/2;
    pat->centerY = sizeY/2;
    pat->sizeX = sizeX;
    pat->sizeY = sizeY;
    layer->alpha = 255;
    layer->flip = 0;
    layer->bmpNo = 0;
    layer->startX = layer->startY = 0;
    layer->sizeX = sizeX;
    layer->sizeY = sizeY;
    layer->shiftX = layer->shiftY = 0;
    layer->vtx[0] = layer->vtx[1] = 0;
    layer->vtx[2] = sizeX;
    layer->vtx[3] = 0;
    layer->vtx[4] = sizeX;
    layer->vtx[5] = sizeY;
    layer->vtx[6] = 0;
    layer->vtx[7] = sizeY;
    bmp->pixSize = bitSizeTbl[dataFmt];
    bmp->dataFmt = dataFmt;
    bmp->palNum = 0;
    bmp->sizeX = sizeX;
    bmp->sizeY = sizeY;
    bmp->dataSize = sizeX*sizeY*bitSizeTbl[dataFmt]/8;
    bmp->palData = NULL;
    bmp->data = NULL;
    return anim;
}

void HuSprBGSet(s16 group, s16 member,  ANIMDATA *bg, s16 bg_bank)
{
    s16 sprite = HuSprGrpData[group].members[member];
    HuSprSprBGSet(sprite, bg, bg_bank);
}

void HuSprSprBGSet(s16 sprite, ANIMDATA *bg, s16 bg_bank)
{
    HUSPRITE *sprite_ptr = &HuSprData[sprite];
    sprite_ptr->bg = bg;
    sprite_ptr->bgBank = bg_bank;
    sprite_ptr->wrapT = sprite_ptr->wrapS = GX_REPEAT;
    sprite_ptr->attr &= ~HUSPR_ATTR_LINEAR;
}

void AnimDebug(ANIMDATA *anim)
{
    ANIMPAT *pat;
    ANIMLAYER *layer;
    s16 i;
    s16 j;
    ANIMFRAME *frame;
    ANIMBANK *bank;
    ANIMBMP *bmp;
    
    OSReport("patNum %d,bankNum %d,bmpNum %d\n", anim->patNum, anim->bankNum, anim->bmpNum & ANIM_BMP_NUM_MASK);
    pat = anim->pat;
    for(i=0; i<anim->patNum; i++) {
        OSReport("PATTERN%d:\n", i);
        OSReport("\tlayerNum %d,center (%d,%d),size (%d,%d)\n", pat->layerNum, pat->centerX, pat->centerX, pat->sizeX, pat->sizeY);
        layer = pat->layer;
        for(j=0; j<pat->layerNum; j++) {
            OSReport("\t\tfileNo %d,flip %x\n", layer->bmpNo, layer->flip);
            OSReport("\t\tstart (%d,%d),size (%d,%d),shift (%d,%d)\n", layer->startX, layer->startY, layer->sizeX, layer->sizeY, layer->shiftX, layer->shiftY);
            if(j != pat->layerNum-1) {
                OSReport("\n");
            }
            layer++;
        }
        pat++;
    }
    bank = anim->bank;
    for(i=0; i<anim->bankNum; i++) {
        OSReport("BANK%d:\n", i);
        OSReport("\ttimeNum %d\n", bank->timeNum);
        frame = bank->frame;
        for(j=0; j<bank->timeNum; j++) {
            OSReport("\t\tpat %d,time %d,shift(%d,%d),flip %x\n", frame->pat, frame->time, frame->shiftX, frame->shiftY, frame->flip);
            frame++;
        }
        bank++;
    }
    bmp = anim->bmp;
    for(i=0; i<anim->bmpNum & ANIM_BMP_NUM_MASK; i++) {
        OSReport("BMP%d:\n", i);
        OSReport("\tpixSize %d,palNum %d,size (%d,%d)\n", bmp->pixSize, bmp->palNum, bmp->sizeX, bmp->sizeY);
        bmp++;
    }
}
