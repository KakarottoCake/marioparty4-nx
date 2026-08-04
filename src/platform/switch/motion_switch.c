#ifdef __SWITCH__

#include <math.h>
#include <string.h>

#include "game/hu3d.h"
#include "game/hsfload.h"
#include "game/init.h"
#include "game/memory.h"

extern const char *SwitchHsfMotionTargetName(const HSFMOTION *motion,
                                              u16 offset);
extern void SwitchHsfClusterAdjustObject(HSFDATA *model, HSFDATA *motionModel);
extern void OSReport(const char *msg, ...);

HU3DMOTION Hu3DMotion[HU3D_MOTION_MAX];

static BOOL SwitchMotionIDOK(s16 id) {
    return id >= 0 && id < HU3D_MOTION_MAX && Hu3DMotion[id].hsf &&
           Hu3DMotion[id].hsf->motion;
}

static HSFMOTION *SwitchMotionData(s16 id) {
    return SwitchMotionIDOK(id) ? Hu3DMotion[id].hsf->motion : NULL;
}

static float *SwitchMotionValue(HSFOBJECT *object, u16 channel) {
    HSFCONSTDATA *constant;

    if (!object || object->type == HSF_OBJ_CAMERA ||
        object->type == HSF_OBJ_LIGHT) {
        return (float *)-1;
    }
    constant = (HSFCONSTDATA *)object->constData;
    switch (channel) {
        case 8:
            if (constant && (constant->attr & HU3D_CONST_FORCE_POSX)) {
                return (float *)-1;
            }
            return &object->mesh.curr.pos.x;
        case 9:
            if (constant && (constant->attr & HU3D_CONST_FORCE_POSY)) {
                return (float *)-1;
            }
            return &object->mesh.curr.pos.y;
        case 10:
            if (constant && (constant->attr & HU3D_CONST_FORCE_POSZ)) {
                return (float *)-1;
            }
            return &object->mesh.curr.pos.z;
        case 28:
            if (constant && (constant->attr & HU3D_CONST_FORCE_ROTX)) {
                return (float *)-1;
            }
            return &object->mesh.curr.rot.x;
        case 29:
            if (constant && (constant->attr & HU3D_CONST_FORCE_ROTY)) {
                return (float *)-1;
            }
            return &object->mesh.curr.rot.y;
        case 30:
            if (constant && (constant->attr & HU3D_CONST_FORCE_ROTZ)) {
                return (float *)-1;
            }
            return &object->mesh.curr.rot.z;
        case 31:
            return &object->mesh.curr.scale.x;
        case 32:
            return &object->mesh.curr.scale.y;
        case 33:
            return &object->mesh.curr.scale.z;
        default:
            return (float *)-1;
    }
}

float *GetObjTRXPtr(HSFOBJECT *object, u16 channel) {
    return SwitchMotionValue(object, channel);
}

static HSFOBJECT *SwitchMotionObject(HSFDATA *model, HSFMOTION *motion,
                                      u16 target) {
    (void)motion;
    if (!model || !model->object || target == 0xFFFF ||
        target >= (u16)model->objectNum) {
        return NULL;
    }
    return &model->object[target];
}

static float SwitchMotionBezier(const HSFTRACK *track, float time) {
    const float *data = (const float *)track->data;
    s32 count = track->numKeyframes;
    s32 i;

    if (!data || count <= 0) {
        return track->value;
    }
    if (time <= data[0] || count == 1) {
        return data[1];
    }
    for (i = 1; i < count; i++) {
        const float *previous = data + (i - 1) * 4;
        const float *current = data + i * 4;
        float t;
        float t2;
        float t3;
        if (time >= current[0]) {
            continue;
        }
        if (current[0] <= previous[0]) {
            return current[1];
        }
        t = (time - previous[0]) / (current[0] - previous[0]);
        t2 = t * t;
        t3 = t2 * t;
        return previous[1] * (2.0f * t3 - 3.0f * t2 + 1.0f) +
               current[1] * (-2.0f * t3 + 3.0f * t2) +
               previous[2] * (t3 - 2.0f * t2 + t) +
               current[3] * (t3 - t2);
    }
    return data[(count - 1) * 4 + 1];
}

float GetBezier(s32 count, HSFTRACK *track, float time) {
    (void)count;
    return SwitchMotionBezier(track, time);
}

float GetConstant(s32 count, float *data, float time) {
    s32 i;
    if (!data || count <= 0) {
        return 0.0f;
    }
    if (time <= 0.0f || count == 1) {
        return data[1];
    }
    for (i = 0; i < count; i++) {
        if (time < data[i * 2]) {
            return i == 0 ? data[1] : data[(i - 1) * 2 + 1];
        }
    }
    return data[(count - 1) * 2 + 1];
}

float GetLinear(s32 count, float data[][2], float time) {
    s32 i;
    if (!data || count <= 0) {
        return 0.0f;
    }
    if (time <= data[0][0] || count == 1) {
        return data[0][1];
    }
    for (i = 1; i < count; i++) {
        float span;
        if (time >= data[i][0]) {
            continue;
        }
        span = data[i][0] - data[i - 1][0];
        if (span <= 0.0f) {
            return data[i][1];
        }
        return data[i - 1][1] +
               (time - data[i - 1][0]) *
                   ((data[i][1] - data[i - 1][1]) / span);
    }
    return data[count - 1][1];
}

float GetCurve(HSFTRACK *track, float time) {
    if (!track) {
        return 0.0f;
    }
    switch (track->curveType) {
        case HSF_CURVE_STEP:
            return GetConstant(track->numKeyframes, track->data, time);
        case HSF_CURVE_LINEAR:
            return GetLinear(track->numKeyframes, track->data, time);
        case HSF_CURVE_BEZIER:
            return GetBezier(track->numKeyframes, track, time);
        case HSF_CURVE_CONST:
            return track->value;
        default:
            return track->value;
    }
}

static void SwitchMotionResetModel(HSFDATA *model) {
    s32 i;
    if (!model || !model->object) {
        return;
    }
    for (i = 0; i < model->objectNum; i++) {
        if (model->object[i].type != HSF_OBJ_CAMERA &&
            model->object[i].type != HSF_OBJ_LIGHT) {
            model->object[i].mesh.curr = model->object[i].mesh.base;
        }
    }
}

void Hu3DMotionExec(s16 modelId, s16 motionId, float time, s32 overlay) {
    HU3DMODEL *modelData;
    HSFDATA *model;
    HSFMOTION *motion;
    s32 i;

    if (modelId < 0 || modelId >= HU3D_MODEL_MAX ||
        !SwitchMotionIDOK(motionId)) {
        return;
    }
    modelData = &Hu3DData[modelId];
    model = modelData->hsf;
    motion = SwitchMotionData(motionId);
    if (!model || !motion) {
        return;
    }
    if (!overlay) {
        SwitchMotionResetModel(model);
    }
    for (i = 0; i < motion->numTracks; i++) {
        HSFTRACK *track = &motion->track[i];
        HSFOBJECT *object;
        float *value;
        if (track->type == HSF_TRACK_TRANSFORM) {
            object = SwitchMotionObject(model, motion, track->target);
            if (!object) {
                continue;
            }
            value = SwitchMotionValue(object, track->channel);
            if (value != (float *)-1) {
                *value = GetCurve(track, time);
            }
        } else if (track->type == HSF_TRACK_MORPH &&
                   track->morphWeight >= 0 && track->morphWeight < 33) {
            object = SwitchMotionObject(model, motion, track->target);
            if (object) {
                object->mesh.mesh.morphWeight[track->morphWeight] =
                    GetCurve(track, time);
            }
        }
    }
    (void)modelData;
}

static void SwitchMotionWorkAdvance(HU3DMOTWORK *work, HSFMOTION *motion,
                                    u32 attr, u32 pauseBit, u32 revBit,
                                    u32 loopBit) {
    float end;
    if (!work || !motion || (attr & pauseBit)) {
        return;
    }
    end = work->end > 0.0f ? work->end : motion->maxTime;
    if (end <= work->start) {
        work->time = work->start;
        return;
    }
    work->time += (attr & revBit ? -1.0f : 1.0f) * work->speed *
                  minimumVcountf;
    if (attr & loopBit) {
        while (work->time < work->start) {
            work->time += end - work->start;
        }
        while (work->time >= end) {
            work->time -= end - work->start;
        }
    } else if (work->time < work->start) {
        work->time = work->start;
    } else if (work->time >= end) {
        work->time = end;
    }
}

void Hu3DMotionNext(s16 modelId) {
    HU3DMODEL *model;
    if (modelId < 0 || modelId >= HU3D_MODEL_MAX) {
        return;
    }
    model = &Hu3DData[modelId];
    if (model->motId != -1 && SwitchMotionIDOK(model->motId)) {
        SwitchMotionWorkAdvance(&model->motWork,
                                SwitchMotionData(model->motId), model->motAttr,
                                HU3D_MOTATTR_PAUSE, HU3D_MOTATTR_REV,
                                HU3D_MOTATTR_LOOP);
    }
    if (model->motIdOvl != -1 && SwitchMotionIDOK(model->motIdOvl)) {
        SwitchMotionWorkAdvance(&model->motOvlWork,
                                SwitchMotionData(model->motIdOvl), model->motAttr,
                                HU3D_MOTATTR_OVL_PAUSE, HU3D_MOTATTR_OVL_REV,
                                HU3D_MOTATTR_OVL_LOOP);
    }
    if (model->motIdShape != -1 && SwitchMotionIDOK(model->motIdShape)) {
        SwitchMotionWorkAdvance(&model->motShapeWork,
                                SwitchMotionData(model->motIdShape),
                                model->motAttr,
                                HU3D_MOTATTR_SHAPE_PAUSE,
                                HU3D_MOTATTR_SHAPE_REV,
                                HU3D_MOTATTR_SHAPE_LOOP);
    }
    if (model->attr & HU3D_ATTR_CLUSTER_ON) {
        s32 i;
        for (i = 0; i < 4; i++) {
            if (model->motIdCluster[i] != -1 &&
                SwitchMotionIDOK(model->motIdCluster[i]) &&
                !(model->clusterAttr[i] & HU3D_CLUSTER_ATTR_PAUSE)) {
                HSFMOTION *motion = SwitchMotionData(model->motIdCluster[i]);
                float maxTime = motion ? motion->maxTime : 0.0f;
                float direction =
                    (model->clusterAttr[i] & HU3D_CLUSTER_ATTR_REV) ? -1.0f : 1.0f;
                model->clusterTime[i] += model->clusterSpeed[i] *
                                         minimumVcountf * direction;
                if (model->clusterAttr[i] & HU3D_CLUSTER_ATTR_LOOP) {
                    if (maxTime > 0.0f) {
                        while (model->clusterTime[i] < 0.0f) model->clusterTime[i] += maxTime;
                        while (model->clusterTime[i] >= maxTime) model->clusterTime[i] -= maxTime;
                    }
                } else {
                    if (model->clusterTime[i] < 0.0f) model->clusterTime[i] = 0.0f;
                    if (model->clusterTime[i] > maxTime) model->clusterTime[i] = maxTime;
                }
            }
        }
    }
}

void Hu3DSubMotionExec(s16 modelId) {
    (void)modelId;
    /* Motion shifts need a separate blend buffer.  Keep the existing pose
     * until that representation is added instead of applying a bad jump. */
}

void Hu3DMotionInit(void) {
    s32 i;
    for (i = 0; i < HU3D_MOTION_MAX; i++) {
        Hu3DMotion[i].attr = 0;
        Hu3DMotion[i].modelId = -1;
        Hu3DMotion[i].hsf = NULL;
    }
}

static s16 SwitchMotionSlot(void) {
    s16 i;
    for (i = 0; i < HU3D_MOTION_MAX; i++) {
        if (!Hu3DMotion[i].hsf) {
            return i;
        }
    }
    return -1;
}

s16 Hu3DMotionCreate(void *data) {
    s16 id;
    HSFDATA *hsf;
    if (!data) {
        return -1;
    }
    id = SwitchMotionSlot();
    if (id < 0) {
        return -1;
    }
    hsf = LoadHSF(data);
    if (!hsf || !hsf->motion) {
        if (hsf) {
            HuMemDirectFree(hsf);
        }
        return -1;
    }
    Hu3DMotion[id].attr = 0;
    Hu3DMotion[id].modelId = -1;
    Hu3DMotion[id].hsf = hsf;
    return id;
}

s16 Hu3DMotionModelCreate(s16 modelId) {
    s16 id;
    if (modelId < 0 || modelId >= HU3D_MODEL_MAX || !Hu3DData[modelId].hsf ||
        !Hu3DData[modelId].hsf->motion) {
        return -1;
    }
    id = SwitchMotionSlot();
    if (id < 0) {
        return -1;
    }
    Hu3DMotion[id].attr = 0;
    Hu3DMotion[id].modelId = modelId;
    Hu3DMotion[id].hsf = Hu3DData[modelId].hsf;
    Hu3DData[modelId].motIdSrc = id;
    return id;
}

s32 Hu3DMotionKill(s16 id) {
    s16 i;
    HU3DMOTION *motion;
    if (id < 0 || id >= HU3D_MOTION_MAX || !(motion = &Hu3DMotion[id])->hsf) {
        return 0;
    }
    for (i = 0; i < HU3D_MODEL_MAX; i++) {
        if (Hu3DData[i].hsf && Hu3DData[i].motId == id &&
            motion->modelId != i) {
            return 0;
        }
    }
    if (motion->modelId == -1) {
        HuMemDirectFree(motion->hsf);
    } else if (motion->modelId < HU3D_MODEL_MAX) {
        Hu3DData[motion->modelId].motIdSrc = -1;
    }
    motion->attr = 0;
    motion->modelId = -1;
    motion->hsf = NULL;
    return 1;
}

void Hu3DMotionAllKill(void) {
    s16 i;
    for (i = 0; i < HU3D_MOTION_MAX; i++) {
        if (Hu3DMotion[i].hsf) {
            Hu3DMotionKill(i);
        }
    }
}

void Hu3DMotionSet(s16 modelId, s16 motionId) {
    HU3DMODEL *model;
    if (modelId < 0 || modelId >= HU3D_MODEL_MAX ||
        !SwitchMotionIDOK(motionId)) {
        return;
    }
    model = &Hu3DData[modelId];
    model->motIdShift = -1;
    model->motId = motionId;
    model->motWork.time = 0.0f;
    model->motWork.start = 0.0f;
    model->motWork.end = Hu3DMotionMotionMaxTimeGet(motionId);
}

void Hu3DMotionOverlaySet(s16 modelId, s16 motionId) {
    if (modelId < 0 || modelId >= HU3D_MODEL_MAX ||
        !SwitchMotionIDOK(motionId)) {
        return;
    }
    Hu3DData[modelId].motIdOvl = motionId;
    Hu3DData[modelId].motOvlWork.time = 0.0f;
    Hu3DData[modelId].motOvlWork.speed = 1.0f;
    Hu3DData[modelId].motOvlWork.start = 0.0f;
    Hu3DData[modelId].motOvlWork.end = Hu3DMotionMotionMaxTimeGet(motionId);
}

void Hu3DMotionOverlayReset(s16 modelId) {
    if (modelId >= 0 && modelId < HU3D_MODEL_MAX) {
        Hu3DData[modelId].motIdOvl = -1;
    }
}

float Hu3DMotionOverlayTimeGet(s16 modelId) {
    return modelId >= 0 && modelId < HU3D_MODEL_MAX
               ? Hu3DData[modelId].motOvlWork.time
               : 0.0f;
}

void Hu3DMotionOverlayTimeSet(s16 modelId, float time) {
    if (modelId >= 0 && modelId < HU3D_MODEL_MAX) {
        Hu3DData[modelId].motOvlWork.time = time;
    }
}

void Hu3DMotionOverlaySpeedSet(s16 modelId, float speed) {
    if (modelId >= 0 && modelId < HU3D_MODEL_MAX) {
        Hu3DData[modelId].motOvlWork.speed = speed;
    }
}

float Hu3DMotionMotionMaxTimeGet(s16 motionId) {
    HSFMOTION *motion = SwitchMotionData(motionId);
    return motion ? motion->maxTime + 0.0001f : 0.0f;
}

float Hu3DMotionMaxTimeGet(s16 modelId) {
    if (modelId < 0 || modelId >= HU3D_MODEL_MAX) {
        return 0.0f;
    }
    return Hu3DMotionMotionMaxTimeGet(Hu3DData[modelId].motId);
}

float Hu3DMotionShiftMaxTimeGet(s16 modelId) {
    if (modelId < 0 || modelId >= HU3D_MODEL_MAX) {
        return 0.0f;
    }
    return Hu3DMotionMotionMaxTimeGet(Hu3DData[modelId].motIdShift);
}

void Hu3DMotionTimeSet(s16 modelId, float time) {
    float maxTime = Hu3DMotionMaxTimeGet(modelId);
    if (modelId < 0 || modelId >= HU3D_MODEL_MAX) {
        return;
    }
    if (time < 0.0f) time = 0.0f;
    if (maxTime > 0.0f && time > maxTime) time = maxTime;
    Hu3DData[modelId].motWork.time = time;
}

float Hu3DMotionTimeGet(s16 modelId) {
    return modelId >= 0 && modelId < HU3D_MODEL_MAX
               ? Hu3DData[modelId].motWork.time
               : 0.0f;
}

float Hu3DMotionShiftTimeGet(s16 modelId) {
    return modelId >= 0 && modelId < HU3D_MODEL_MAX
               ? Hu3DData[modelId].motShiftWork.time
               : 0.0f;
}

void Hu3DMotionSpeedSet(s16 modelId, float speed) {
    if (modelId >= 0 && modelId < HU3D_MODEL_MAX) {
        Hu3DData[modelId].motWork.speed = speed;
    }
}

void Hu3DMotionShiftSpeedSet(s16 modelId, float speed) {
    if (modelId >= 0 && modelId < HU3D_MODEL_MAX) {
        Hu3DData[modelId].motShiftWork.speed = speed;
    }
}

void Hu3DMotionStartEndSet(s16 modelId, float start, float end) {
    if (modelId >= 0 && modelId < HU3D_MODEL_MAX) {
        Hu3DData[modelId].motWork.start = start;
        Hu3DData[modelId].motWork.end = end;
    }
}

void Hu3DMotionShiftStartEndSet(s16 modelId, float start, float end) {
    if (modelId >= 0 && modelId < HU3D_MODEL_MAX) {
        Hu3DData[modelId].motShiftWork.start = start;
        Hu3DData[modelId].motShiftWork.end = end;
    }
}

s32 Hu3DMotionEndCheck(s16 modelId) {
    if (modelId < 0 || modelId >= HU3D_MODEL_MAX) {
        return TRUE;
    }
    if (Hu3DData[modelId].motAttr & HU3D_MOTATTR_REV) {
        return Hu3DData[modelId].motWork.time <= 0.0f;
    }
    return Hu3DMotionMaxTimeGet(modelId) <= Hu3DData[modelId].motWork.time;
}

s16 Hu3DMotionIDGet(s16 modelId) {
    return modelId >= 0 && modelId < HU3D_MODEL_MAX ? Hu3DData[modelId].motId : -1;
}

s16 Hu3DMotionShiftIDGet(s16 modelId) {
    return modelId >= 0 && modelId < HU3D_MODEL_MAX
               ? Hu3DData[modelId].motIdShift
               : -1;
}

void Hu3DMotionShiftSet(s16 modelId, s16 motionId, float time, float end,
                        u32 attr) {
    if (modelId < 0 || modelId >= HU3D_MODEL_MAX ||
        !SwitchMotionIDOK(motionId)) {
        return;
    }
    Hu3DData[modelId].motIdShift = motionId;
    Hu3DData[modelId].motShiftWork.time = time;
    Hu3DData[modelId].motShiftWork.speed = 1.0f;
    Hu3DData[modelId].motShiftWork.start = 0.0f;
    Hu3DData[modelId].motShiftWork.end = Hu3DMotionMotionMaxTimeGet(motionId);
    Hu3DData[modelId].motOvlWork.start = 0.0f;
    Hu3DData[modelId].motOvlWork.end = end;
    Hu3DData[modelId].motAttr = attr & ~HU3D_MOTATTR;
}

s16 Hu3DMotionClusterSet(s16 modelId, s16 motionId) {
    s16 i;
    if (modelId < 0 || modelId >= HU3D_MODEL_MAX ||
        !SwitchMotionIDOK(motionId)) {
        return -1;
    }
    for (i = 0; i < 4; i++) {
        if (Hu3DData[modelId].motIdCluster[i] == -1) {
            Hu3DData[modelId].motIdCluster[i] = motionId;
            Hu3DData[modelId].clusterTime[i] = 0.0f;
            Hu3DData[modelId].clusterSpeed[i] = 1.0f;
            Hu3DData[modelId].clusterAttr[i] = HU3D_ATTR_NONE;
            Hu3DData[modelId].attr |= HU3D_ATTR_CLUSTER_ON;
            SwitchHsfClusterAdjustObject(Hu3DData[modelId].hsf,
                                          Hu3DMotion[motionId].hsf);
            return i;
        }
    }
    OSReport("Switch: cluster motion slots full\n");
    return -1;
}

void Hu3DMotionShapeSet(s16 modelId, s16 motionId) {
    if (modelId < 0 || modelId >= HU3D_MODEL_MAX ||
        !SwitchMotionIDOK(motionId)) {
        return;
    }
    Hu3DData[modelId].motIdShape = motionId;
    Hu3DData[modelId].motShapeWork.time = 0.0f;
    Hu3DData[modelId].motShapeWork.speed = 1.0f;
    Hu3DData[modelId].motShapeWork.start = 0.0f;
    Hu3DData[modelId].motShapeWork.end =
        Hu3DMotionMotionMaxTimeGet(motionId);
}

s16 Hu3DMotionShapeIDGet(s16 modelId) {
    return modelId >= 0 && modelId < HU3D_MODEL_MAX
               ? Hu3DData[modelId].motIdShape
               : -1;
}

void Hu3DMotionShapeReset(s16 modelId) {
    if (modelId >= 0 && modelId < HU3D_MODEL_MAX) {
        Hu3DData[modelId].motIdShape = -1;
    }
}

float Hu3DMotionShapeMaxTimeGet(s16 modelId) {
    if (modelId < 0 || modelId >= HU3D_MODEL_MAX) {
        return 0.0f;
    }
    return Hu3DMotionMotionMaxTimeGet(Hu3DData[modelId].motIdShape);
}

void Hu3DMotionShapeSpeedSet(s16 modelId, float speed) {
    if (modelId >= 0 && modelId < HU3D_MODEL_MAX) {
        Hu3DData[modelId].motShapeWork.speed = speed;
    }
}

void Hu3DMotionShapeTimeSet(s16 modelId, float time) {
    if (modelId >= 0 && modelId < HU3D_MODEL_MAX) {
        Hu3DData[modelId].motShapeWork.time = time;
    }
}

void Hu3DMotionShapeStartEndSet(s16 modelId, float start, float end) {
    if (modelId >= 0 && modelId < HU3D_MODEL_MAX) {
        Hu3DData[modelId].motShapeWork.start = start;
        Hu3DData[modelId].motShapeWork.end = end;
    }
}

s16 Hu3DJointMotion(s16 modelId, void *data) {
    return Hu3DMotionCreate(data);
}

void JointModel_Motion(s16 modelId, s16 motionId) {
    (void)modelId;
    (void)motionId;
}

void Hu3DCameraMotionExec(s16 modelId) {
    (void)modelId;
}

void SetObjMatMotion(s16 modelId, HSFTRACK *track, float value) {
    (void)modelId;
    (void)track;
    (void)value;
}

void SetObjAttrMotion(s16 modelId, HSFTRACK *track, float value) {
    (void)modelId;
    (void)track;
    (void)value;
}

void SetObjCameraMotion(s16 modelId, HSFTRACK *track, float value) {
    (void)modelId;
    (void)track;
    (void)value;
}

void SetObjLightMotion(s16 modelId, HSFTRACK *track, float value) {
    (void)modelId;
    (void)track;
    (void)value;
}

#endif
