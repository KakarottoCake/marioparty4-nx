#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "dolphin/os.h"
#include "dolphin/types.h"
#include "sfx_index_data.h"

#define SWITCH_AUDIO_SAMPLE_RATE 48000
#define SWITCH_AUDIO_CHANNELS 2
#define SWITCH_AUDIO_SAMPLES 960
#define SWITCH_AUDIO_BUFFER_SIZE (SWITCH_AUDIO_SAMPLES * SWITCH_AUDIO_CHANNELS * sizeof(s16))
#define SWITCH_AUDIO_BUFFER_COUNT 3
#define SWITCH_AUDIO_PI 3.14159265358979323846f
#define SWITCH_AUDIO_DSP_BLOCK_BYTES 8
#define SWITCH_AUDIO_DSP_BLOCK_SAMPLES 14
#define SWITCH_AUDIO_EFFECT_COUNT 12
#define SWITCH_AUDIO_EFFECT_MAX_MS 140
#define SWITCH_AUDIO_SFX_MAX_ENTRIES 2308
#define SWITCH_AUDIO_SFX_ENTRY_SIZE 24
#define SWITCH_AUDIO_SFX_SAMPLE_RATE 32000
#define SWITCH_AUDIO_SFX_INDEX_SIZE \
    (16 + SWITCH_AUDIO_SFX_MAX_ENTRIES * SWITCH_AUDIO_SFX_ENTRY_SIZE)

typedef struct SwitchAudioDspChannel_s {
    u8 *data;
    u32 dataSize;
    u32 dataOffset;
    u32 blockSample;
    s16 coef[8][2];
    s16 hist1;
    s16 hist2;
    s16 sample;
} SwitchAudioDspChannel;

typedef struct SwitchAudioEffect_s {
    bool active;
    u32 id;
    u32 frequency;
    u32 samplesLeft;
    u32 samplesTotal;
    s16 volume;
    s16 pan;
    float phase;
    s16 *sampleData;
    u32 sampleCount;
    u64 samplePhase;
    u64 samplePhaseStep;
} SwitchAudioEffect;

typedef struct SwitchAudioSfxEntry_s {
    u32 dataOffset;
    u32 dataSize;
    u32 sampleCount;
    u32 coeffOffset;
    u8 volume;
    u8 pan;
    u16 flags;
} SwitchAudioSfxEntry;

static s16 s_audioBuffers[SWITCH_AUDIO_BUFFER_COUNT][SWITCH_AUDIO_SAMPLES * SWITCH_AUDIO_CHANNELS]
    __attribute__((aligned(0x1000)));
static AudioOutBuffer s_audioOutBuffers[SWITCH_AUDIO_BUFFER_COUNT];
static bool s_audioReady;
static u32 s_audioSampleRate = SWITCH_AUDIO_SAMPLE_RATE;
static SwitchAudioEffect s_effects[SWITCH_AUDIO_EFFECT_COUNT];
static u32 s_effectSequence;
static SwitchAudioSfxEntry s_sfxEntries[SWITCH_AUDIO_SFX_MAX_ENTRIES];
static u32 s_sfxEntryCount;
static FILE *s_sfxBankFile;
static bool s_sfxIndexAttempted;
static bool s_sfxIndexReady;
static bool s_reportedPlayback;
static bool s_reportedSfxPlayback;
static bool s_streamPlaying;
static u32 s_streamId;
static u32 s_streamRate;
static u32 s_streamLoopSamples;
static u32 s_streamLoopStartSamples;
static bool s_streamLoopEnabled;
static u32 s_streamVolume;
static u64 s_streamPhase;
static u64 s_streamPhaseStep;
static u32 s_streamDecodedSamples;
static u32 s_streamFadeRemaining;
static u32 s_streamFadeTotal;
static SwitchAudioDspChannel s_streamChannels[2];

static u16 SwitchAudioReadBE16(const u8 *data);
static u32 SwitchAudioReadBE32(const u8 *data);
static BOOL SwitchAudioReadAt(FILE *file, void *data, size_t size, u32 offset);
static s16 SwitchAudioDecodeDspSample(SwitchAudioDspChannel *channel);

static void SwitchAudioFreeEffect(SwitchAudioEffect *effect)
{
    free(effect->sampleData);
    effect->sampleData = NULL;
    effect->sampleCount = 0;
    effect->samplePhase = 0;
    effect->samplePhaseStep = 0;
    effect->active = FALSE;
}

static FILE *SwitchAudioOpenSfxIndexFile(void)
{
    static const char *paths[] = {
        "romfs:/files/sound/mp4sfx.idx",
        "sdmc:/switch/files/sound/mp4sfx.idx",
        "sdmc:/files/sound/mp4sfx.idx",
        "files/sound/mp4sfx.idx"
    };
    u32 i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *file = fopen(paths[i], "rb");
        if (file != NULL) {
            OSReport("SwitchAudio: opened %s\n", paths[i]);
            return file;
        }
    }
    return NULL;
}

static FILE *SwitchAudioOpenMsmFile(void)
{
    static const char *paths[] = {
        "romfs:/files/sound/mpgcsnd.msm",
        "sdmc:/switch/files/sound/mpgcsnd.msm",
        "sdmc:/files/sound/mpgcsnd.msm",
        "files/sound/mpgcsnd.msm"
    };
    u32 i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *file = fopen(paths[i], "rb");
        if (file != NULL) {
            OSReport("SwitchAudio: opened %s\n", paths[i]);
            return file;
        }
    }
    return NULL;
}

static BOOL SwitchAudioReadSfxIndex(FILE *file, u32 baseOffset)
{
    u8 header[16];
    u8 entry[SWITCH_AUDIO_SFX_ENTRY_SIZE];
    u32 entrySize;
    u32 count;
    u32 i;

    if (!SwitchAudioReadAt(file, header, sizeof(header), baseOffset) ||
        memcmp(header, "SFXI", 4) != 0 || SwitchAudioReadBE16(header + 4) != 1) {
        return FALSE;
    }
    entrySize = SwitchAudioReadBE16(header + 6);
    count = SwitchAudioReadBE32(header + 8);
    if (entrySize != SWITCH_AUDIO_SFX_ENTRY_SIZE || count > SWITCH_AUDIO_SFX_MAX_ENTRIES) {
        return FALSE;
    }
    memset(s_sfxEntries, 0, sizeof(s_sfxEntries));
    for (i = 0; i < count; i++) {
        if (!SwitchAudioReadAt(file, entry, sizeof(entry), baseOffset + 16 + i * entrySize)) {
            return FALSE;
        }
        s_sfxEntries[i].dataOffset = SwitchAudioReadBE32(entry + 0);
        s_sfxEntries[i].dataSize = SwitchAudioReadBE32(entry + 4);
        s_sfxEntries[i].sampleCount = SwitchAudioReadBE32(entry + 8);
        s_sfxEntries[i].coeffOffset = SwitchAudioReadBE32(entry + 12);
        s_sfxEntries[i].volume = entry[16];
        s_sfxEntries[i].pan = entry[17];
        s_sfxEntries[i].flags = SwitchAudioReadBE16(entry + 18);
    }
    s_sfxEntryCount = count;
    s_sfxIndexReady = TRUE;
    OSReport("SwitchAudio: SFX index loaded %u entries\n", count);
    return TRUE;
}

static BOOL SwitchAudioLoadSfxIndex(void)
{
    FILE *file;
    long fileEnd;

    if (s_sfxIndexAttempted) {
        return s_sfxIndexReady;
    }
    s_sfxIndexAttempted = TRUE;

    file = SwitchAudioOpenSfxIndexFile();
    if (file != NULL) {
        if (SwitchAudioReadSfxIndex(file, 0)) {
            fclose(file);
            return TRUE;
        }
        fclose(file);
    }

    /* Eden keeps the original asset filename visible even when a new
     * sidecar file is not visible.  The build tool can append this metadata
     * index to mpgcsnd.msm without changing any original sound bytes. */
    file = SwitchAudioOpenMsmFile();
    if (file != NULL && fseek(file, 0, SEEK_END) == 0) {
        fileEnd = ftell(file);
        if (fileEnd >= SWITCH_AUDIO_SFX_INDEX_SIZE &&
            SwitchAudioReadSfxIndex(file, (u32)fileEnd - SWITCH_AUDIO_SFX_INDEX_SIZE)) {
            s_sfxBankFile = file;
            OSReport("SwitchAudio: using appended SFX index\n");
            return TRUE;
        }
    }
    if (file != NULL) {
        fclose(file);
    }
    if (g_switchSfxIndexDataSize >= 16 &&
        memcmp(g_switchSfxIndexData, "SFXI", 4) == 0 &&
        SwitchAudioReadBE16(g_switchSfxIndexData + 4) == 1 &&
        SwitchAudioReadBE16(g_switchSfxIndexData + 6) == SWITCH_AUDIO_SFX_ENTRY_SIZE &&
        SwitchAudioReadBE32(g_switchSfxIndexData + 8) <= SWITCH_AUDIO_SFX_MAX_ENTRIES &&
        16 + SwitchAudioReadBE32(g_switchSfxIndexData + 8) * SWITCH_AUDIO_SFX_ENTRY_SIZE <=
            g_switchSfxIndexDataSize) {
        u32 count = SwitchAudioReadBE32(g_switchSfxIndexData + 8);
        memset(s_sfxEntries, 0, sizeof(s_sfxEntries));
        for (u32 i = 0; i < count; i++) {
            const u8 *entry = g_switchSfxIndexData + 16 + i * SWITCH_AUDIO_SFX_ENTRY_SIZE;
            s_sfxEntries[i].dataOffset = SwitchAudioReadBE32(entry + 0);
            s_sfxEntries[i].dataSize = SwitchAudioReadBE32(entry + 4);
            s_sfxEntries[i].sampleCount = SwitchAudioReadBE32(entry + 8);
            s_sfxEntries[i].coeffOffset = SwitchAudioReadBE32(entry + 12);
            s_sfxEntries[i].volume = entry[16];
            s_sfxEntries[i].pan = entry[17];
            s_sfxEntries[i].flags = SwitchAudioReadBE16(entry + 18);
        }
        s_sfxEntryCount = count;
        s_sfxIndexReady = TRUE;
        OSReport("SwitchAudio: embedded SFX index loaded %u entries\n", count);
        return TRUE;
    }
    OSReport("SwitchAudio: SFX index unavailable; using fallback tones\n");
    return FALSE;
}

static BOOL SwitchAudioLoadSfxSample(u32 effectId, SwitchAudioEffect *effect)
{
    const SwitchAudioSfxEntry *entry;
    u8 *compressed;
    u8 coeff[0x28];
    SwitchAudioDspChannel channel;
    u32 i;

    if (!s_sfxIndexReady || effectId >= s_sfxEntryCount) {
        return FALSE;
    }
    entry = &s_sfxEntries[effectId];
    if ((entry->flags & 1) == 0 || entry->dataSize == 0 || entry->sampleCount == 0 ||
        entry->sampleCount > 0x1000000) {
        return FALSE;
    }
    if (s_sfxBankFile == NULL) {
        s_sfxBankFile = SwitchAudioOpenMsmFile();
        if (s_sfxBankFile == NULL) {
            return FALSE;
        }
    }
    compressed = (u8 *)malloc(entry->dataSize);
    if (compressed == NULL || !SwitchAudioReadAt(s_sfxBankFile, compressed, entry->dataSize,
                                                   entry->dataOffset) ||
        !SwitchAudioReadAt(s_sfxBankFile, coeff, sizeof(coeff), entry->coeffOffset)) {
        free(compressed);
        return FALSE;
    }
    memset(&channel, 0, sizeof(channel));
    channel.data = compressed;
    channel.dataSize = entry->dataSize;
    channel.blockSample = SWITCH_AUDIO_DSP_BLOCK_SAMPLES;
    for (i = 0; i < 8; i++) {
        channel.coef[i][0] = (s16)SwitchAudioReadBE16(coeff + 8 + i * 4);
        channel.coef[i][1] = (s16)SwitchAudioReadBE16(coeff + 10 + i * 4);
    }
    effect->sampleData = (s16 *)malloc(entry->sampleCount * sizeof(s16));
    if (effect->sampleData == NULL) {
        free(compressed);
        return FALSE;
    }
    for (i = 0; i < entry->sampleCount; i++) {
        effect->sampleData[i] = SwitchAudioDecodeDspSample(&channel);
    }
    free(compressed);
    effect->sampleCount = entry->sampleCount;
    effect->samplePhase = 0;
    effect->samplePhaseStep = ((u64)SWITCH_AUDIO_SFX_SAMPLE_RATE << 16) /
                              s_audioSampleRate;
    return TRUE;
}

static u16 SwitchAudioReadBE16(const u8 *data)
{
    return ((u16)data[0] << 8) | data[1];
}

static u32 SwitchAudioReadBE32(const u8 *data)
{
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) |
           ((u32)data[2] << 8) | data[3];
}

static BOOL SwitchAudioReadAt(FILE *file, void *data, size_t size, u32 offset)
{
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        return FALSE;
    }
    return fread(data, 1, size, file) == size;
}

static FILE *SwitchAudioOpenPdt(void)
{
    static const char *paths[] = {
        "romfs:/files/sound/mpgcstr.pdt",
        "sdmc:/switch/files/sound/mpgcstr.pdt",
        "sdmc:/files/sound/mpgcstr.pdt",
        "files/sound/mpgcstr.pdt"
    };
    u32 i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *file = fopen(paths[i], "rb");
        if (file != NULL) {
            OSReport("SwitchAudio: opened %s\n", paths[i]);
            return file;
        }
    }
    return NULL;
}

void SwitchAudioStopStream(void)
{
    u32 i;

    for (i = 0; i < 2; i++) {
        free(s_streamChannels[i].data);
        memset(&s_streamChannels[i], 0, sizeof(s_streamChannels[i]));
    }
    s_streamPlaying = FALSE;
    s_streamId = 0xFFFFFFFF;
    s_streamRate = 0;
    s_streamLoopSamples = 0;
    s_streamLoopStartSamples = 0;
    s_streamLoopEnabled = FALSE;
    s_streamPhase = 0;
    s_streamPhaseStep = 0;
    s_streamDecodedSamples = 0;
    s_streamFadeRemaining = 0;
    s_streamFadeTotal = 0;
}

static void SwitchAudioResetStreamDecoder(void)
{
    u32 i;

    for (i = 0; i < 2; i++) {
        s_streamChannels[i].dataOffset = 0;
        s_streamChannels[i].blockSample = SWITCH_AUDIO_DSP_BLOCK_SAMPLES;
        s_streamChannels[i].hist1 = 0;
        s_streamChannels[i].hist2 = 0;
        s_streamChannels[i].sample = 0;
    }
    s_streamDecodedSamples = 0;
}

static s16 SwitchAudioDecodeDspSample(SwitchAudioDspChannel *channel)
{
    u8 header;
    u8 encoded;
    u32 predictor;
    u32 scale;
    s32 nibble;
    s32 sampleIndex;
    s64 value;

    if (channel->data == NULL || channel->dataOffset + SWITCH_AUDIO_DSP_BLOCK_BYTES >
        channel->dataSize) {
        return 0;
    }

    if (channel->blockSample >= SWITCH_AUDIO_DSP_BLOCK_SAMPLES) {
        channel->blockSample = 0;
    }

    header = channel->data[channel->dataOffset];
    predictor = (header >> 4) & 7;
    scale = header & 0xF;
    sampleIndex = (s32)channel->blockSample;
    encoded = channel->data[channel->dataOffset + 1 + (sampleIndex >> 1)];
    if ((sampleIndex & 1) == 0) {
        nibble = (encoded >> 4) & 0xF;
    } else {
        nibble = encoded & 0xF;
    }
    if (nibble >= 8) {
        nibble -= 16;
    }

    value = (s64)channel->coef[predictor][1] * channel->hist2;
    value += (s64)channel->coef[predictor][0] * channel->hist1;
    value += ((s64)nibble << scale) << 11;
    value <<= 5;

    if ((u16)(value & 0xFFFF) > 0x8000 ||
        ((u16)(value & 0xFFFF) == 0x8000 && (value & 0x10000))) {
        value += 0x10000;
    }
    if (value > 2147483647LL) {
        value = 2147483647LL;
    } else if (value < -2147483648LL) {
        value = -2147483648LL;
    }

    channel->sample = (s16)(value >> 16);
    channel->hist2 = channel->hist1;
    channel->hist1 = channel->sample;
    channel->blockSample++;
    if (channel->blockSample == SWITCH_AUDIO_DSP_BLOCK_SAMPLES) {
        channel->dataOffset += SWITCH_AUDIO_DSP_BLOCK_BYTES;
    }
    return channel->sample;
}

static s16 SwitchAudioApplyVolume(s16 sample, u32 volume)
{
    s32 value = ((s32)sample * (s32)volume) / 127;
    if (value > 32767) {
        value = 32767;
    } else if (value < -32768) {
        value = -32768;
    }
    return (s16)value;
}

static void SwitchAudioGetStreamSample(s16 *left, s16 *right)
{
    u64 loopLimit;
    u32 targetSample;

    if (!s_streamPlaying || s_streamLoopSamples == 0) {
        *left = 0;
        *right = 0;
        return;
    }

    loopLimit = (u64)s_streamLoopSamples << 16;
    if (s_streamPhase >= loopLimit) {
        if (!s_streamLoopEnabled) {
            s_streamPlaying = FALSE;
            *left = 0;
            *right = 0;
            return;
        }
        s_streamPhase = ((u64)s_streamLoopStartSamples << 16) +
                        (s_streamPhase - loopLimit);
        SwitchAudioResetStreamDecoder();
    }

    targetSample = (u32)(s_streamPhase >> 16);
    while (s_streamDecodedSamples <= targetSample) {
        s_streamChannels[0].sample = SwitchAudioDecodeDspSample(&s_streamChannels[0]);
        s_streamChannels[1].sample = SwitchAudioDecodeDspSample(&s_streamChannels[1]);
        s_streamDecodedSamples++;
    }

    {
        u32 volume = s_streamVolume;
        if (s_streamFadeRemaining != 0 && s_streamFadeTotal != 0) {
            volume = (volume * s_streamFadeRemaining) / s_streamFadeTotal;
            s_streamFadeRemaining--;
        }
        *left = SwitchAudioApplyVolume(s_streamChannels[0].sample, volume);
        *right = SwitchAudioApplyVolume(s_streamChannels[1].sample, volume);
    }
    s_streamPhase += s_streamPhaseStep;
}

static void SwitchAudioMixEffects(s32 *left, s32 *right)
{
    u32 i;

    for (i = 0; i < SWITCH_AUDIO_EFFECT_COUNT; i++) {
        SwitchAudioEffect *effect = &s_effects[i];
        s32 sample;
        s32 leftGain;
        s32 rightGain;

        if (!effect->active) {
            continue;
        }
        if (effect->sampleData != NULL) {
            u32 sampleIndex;

            if (effect->samplePhase >= ((u64)effect->sampleCount << 16)) {
                SwitchAudioFreeEffect(effect);
                continue;
            }
            sampleIndex = (u32)(effect->samplePhase >> 16);
            sample = effect->sampleData[sampleIndex];
            effect->samplePhase += effect->samplePhaseStep;
        } else {
            sample = (s32)(sinf(effect->phase) * 32767.0f);
            effect->phase += 2.0f * SWITCH_AUDIO_PI * (float)effect->frequency /
                             (float)s_audioSampleRate;
            if (effect->phase >= 2.0f * SWITCH_AUDIO_PI) {
                effect->phase -= 2.0f * SWITCH_AUDIO_PI;
            }
            if (--effect->samplesLeft == 0) {
                SwitchAudioFreeEffect(effect);
                continue;
            }
        }
        sample = (sample * effect->volume) / 127;
        if (effect->sampleData == NULL && effect->samplesLeft < effect->samplesTotal / 4) {
            sample = (sample * effect->samplesLeft * 4) / effect->samplesTotal;
        }
        leftGain = 127 - effect->pan;
        rightGain = effect->pan;
        *left += (sample * leftGain) / 127;
        *right += (sample * rightGain) / 127;
    }
}

static void SwitchAudioFillBuffer(s16 *buffer)
{
    u32 i;

    for (i = 0; i < SWITCH_AUDIO_SAMPLES; i++) {
        s16 streamLeft = 0;
        s16 streamRight = 0;
        s32 left;
        s32 right;

        SwitchAudioGetStreamSample(&streamLeft, &streamRight);
        left = (s32)streamLeft;
        right = (s32)streamRight;
        SwitchAudioMixEffects(&left, &right);
        if (left > 32767) {
            left = 32767;
        } else if (left < -32768) {
            left = -32768;
        }
        if (right > 32767) {
            right = 32767;
        } else if (right < -32768) {
            right = -32768;
        }
        buffer[i * 2] = (s16)left;
        buffer[i * 2 + 1] = (s16)right;
    }
}

BOOL SwitchAudioStartStream(u32 streamId)
{
    FILE *file;
    u8 header[0x20];
    u8 listEntry[4];
    u8 pack[0x20];
    u8 coeff[0x20];
    u32 streamMax;
    u32 adpcmParamOfs;
    u32 packOfs;
    u32 sampleRate;
    u32 loopLen;
    u32 loopStart;
    u32 i;

    if (!s_audioReady) {
        return FALSE;
    }
    if (s_streamPlaying && s_streamId == streamId) {
        s_streamFadeRemaining = 0;
        s_streamFadeTotal = 0;
        return TRUE;
    }

    file = SwitchAudioOpenPdt();
    if (file == NULL) {
        OSReport("SwitchAudio: mpgcstr.pdt not found\n");
        return FALSE;
    }
    if (!SwitchAudioReadAt(file, header, sizeof(header), 0)) {
        OSReport("SwitchAudio: PDT header read failed\n");
        fclose(file);
        return FALSE;
    }

    streamMax = SwitchAudioReadBE16(header + 2);
    adpcmParamOfs = SwitchAudioReadBE32(header + 0x14);
    if (streamId >= streamMax ||
        !SwitchAudioReadAt(file, listEntry, sizeof(listEntry), 0x20 + streamId * 4)) {
        OSReport("SwitchAudio: PDT stream %u is invalid\n", streamId);
        fclose(file);
        return FALSE;
    }

    packOfs = SwitchAudioReadBE32(listEntry);
    if (packOfs == 0 || !SwitchAudioReadAt(file, pack, sizeof(pack), packOfs)) {
        OSReport("SwitchAudio: PDT stream %u pack read failed\n", streamId);
        fclose(file);
        return FALSE;
    }

    sampleRate = SwitchAudioReadBE16(pack + 6);
    loopLen = (SwitchAudioReadBE32(pack + 8) >> 1) & ~0x1F;
    loopStart = (SwitchAudioReadBE32(pack + 12) >> 1) & ~7;
    if ((pack[0] & 1) == 0 || sampleRate == 0 || loopLen < SWITCH_AUDIO_DSP_BLOCK_BYTES ||
        (loopLen & (SWITCH_AUDIO_DSP_BLOCK_BYTES - 1)) != 0 ||
        (loopStart & (SWITCH_AUDIO_DSP_BLOCK_BYTES - 1)) != 0 || loopStart >= loopLen) {
        OSReport("SwitchAudio: PDT stream %u format unsupported\n", streamId);
        fclose(file);
        return FALSE;
    }

    SwitchAudioStopStream();
    for (i = 0; i < 2; i++) {
        u32 sampleOfs = SwitchAudioReadBE32(pack + 16 + i * 8);
        u32 adpcmIndex = SwitchAudioReadBE16(pack + 20 + i * 8);
        u32 coeffOfs = adpcmParamOfs + adpcmIndex * sizeof(coeff);

        if (!SwitchAudioReadAt(file, coeff, sizeof(coeff), coeffOfs)) {
            OSReport("SwitchAudio: PDT stream %u coefficients read failed\n", streamId);
            fclose(file);
            SwitchAudioStopStream();
            return FALSE;
        }
        s_streamChannels[i].data = (u8 *)malloc(loopLen);
        if (s_streamChannels[i].data == NULL ||
            !SwitchAudioReadAt(file, s_streamChannels[i].data, loopLen, sampleOfs)) {
            OSReport("SwitchAudio: PDT stream %u sample data read failed\n", streamId);
            fclose(file);
            SwitchAudioStopStream();
            return FALSE;
        }
        s_streamChannels[i].dataSize = loopLen;
        for (u32 j = 0; j < 8; j++) {
            s_streamChannels[i].coef[j][0] = (s16)SwitchAudioReadBE16(coeff + j * 4);
            s_streamChannels[i].coef[j][1] = (s16)SwitchAudioReadBE16(coeff + j * 4 + 2);
        }
    }
    fclose(file);

    s_streamRate = sampleRate;
    s_streamId = streamId;
    s_streamLoopSamples = (loopLen / SWITCH_AUDIO_DSP_BLOCK_BYTES) *
                          SWITCH_AUDIO_DSP_BLOCK_SAMPLES;
    s_streamLoopStartSamples = (loopStart / SWITCH_AUDIO_DSP_BLOCK_BYTES) *
                               SWITCH_AUDIO_DSP_BLOCK_SAMPLES;
    s_streamLoopEnabled = (pack[0] & 2) != 0;
    s_streamVolume = pack[1] > 0 ? pack[1] : 127;
    s_streamPhaseStep = ((u64)s_streamRate << 16) / s_audioSampleRate;
    s_streamFadeRemaining = 0;
    s_streamFadeTotal = 0;
    SwitchAudioResetStreamDecoder();
    s_streamPlaying = TRUE;
    OSReport("SwitchAudio: stream %u started %uHz %u samples\n",
             streamId, s_streamRate, s_streamLoopSamples);
    return TRUE;
}

static void SwitchAudioPrepareBuffer(AudioOutBuffer *audioBuffer, s16 *data)
{
    audioBuffer->next = NULL;
    audioBuffer->buffer = data;
    audioBuffer->buffer_size = SWITCH_AUDIO_BUFFER_SIZE;
    audioBuffer->data_size = SWITCH_AUDIO_BUFFER_SIZE;
    audioBuffer->data_offset = 0;
}

BOOL SwitchAudioInit(void)
{
    Result rc;
    u32 i;

    if (s_audioReady) {
        return TRUE;
    }

    rc = audoutInitialize();
    if (R_FAILED(rc)) {
        OSReport("SwitchAudio: audoutInitialize failed: 0x%08x\n", rc);
        return FALSE;
    }

    s_audioSampleRate = audoutGetSampleRate();
    if (s_audioSampleRate == 0) {
        s_audioSampleRate = SWITCH_AUDIO_SAMPLE_RATE;
    }
    OSReport("SwitchAudio: device %uHz %uch PCM=%u\n",
             s_audioSampleRate, audoutGetChannelCount(), audoutGetPcmFormat());

    rc = audoutStartAudioOut();
    if (R_FAILED(rc)) {
        OSReport("SwitchAudio: audoutStartAudioOut failed: 0x%08x\n", rc);
        audoutExit();
        return FALSE;
    }

    memset(s_audioBuffers, 0, sizeof(s_audioBuffers));
    memset(s_effects, 0, sizeof(s_effects));
    memset(s_sfxEntries, 0, sizeof(s_sfxEntries));
    s_effectSequence = 0;
    s_sfxEntryCount = 0;
    s_sfxBankFile = NULL;
    s_sfxIndexAttempted = FALSE;
    s_sfxIndexReady = FALSE;
    s_reportedPlayback = FALSE;
    s_reportedSfxPlayback = FALSE;
    SwitchAudioStopStream();
    for (i = 0; i < SWITCH_AUDIO_BUFFER_COUNT; i++) {
        SwitchAudioPrepareBuffer(&s_audioOutBuffers[i], s_audioBuffers[i]);
        rc = audoutAppendAudioOutBuffer(&s_audioOutBuffers[i]);
        if (R_FAILED(rc)) {
            OSReport("SwitchAudio: append buffer %u failed: 0x%08x\n", i, rc);
            audoutStopAudioOut();
            audoutExit();
            return FALSE;
        }
    }

    s_audioReady = TRUE;
    OSReport("SwitchAudio: native PCM output ready\n");
    SwitchAudioLoadSfxIndex();
    /* Start the title stream early so the native backend is audible even
     * while the original title process is still loading its first scene. */
    SwitchAudioStartStream(20);
    return TRUE;
}

void SwitchAudioTick(void)
{
    AudioOutBuffer *released;
    u32 releasedCount;

    if (!s_audioReady) {
        return;
    }

    do {
        released = NULL;
        releasedCount = 0;
        if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &releasedCount)) ||
            released == NULL) {
            break;
        }
        if (!s_reportedPlayback) {
            OSReport("SwitchAudio: playback buffer recycled\n");
            s_reportedPlayback = TRUE;
        }
        SwitchAudioFillBuffer((s16 *)released->buffer);
        if (R_FAILED(audoutAppendAudioOutBuffer(released))) {
            OSReport("SwitchAudio: recycled buffer append failed\n");
            break;
        }
    } while (releasedCount != 0);
}

static s32 SwitchAudioPlayEffectEx(u32 effectId, s16 volume, s16 pan)
{
    SwitchAudioEffect *effect = NULL;
    u32 i;

    if (!s_audioReady) {
        return -1;
    }
    for (i = 0; i < SWITCH_AUDIO_EFFECT_COUNT; i++) {
        if (!s_effects[i].active) {
            effect = &s_effects[i];
            break;
        }
    }
    if (effect == NULL) {
        effect = &s_effects[s_effectSequence % SWITCH_AUDIO_EFFECT_COUNT];
    }
    SwitchAudioFreeEffect(effect);
    s_effectSequence++;
    effect->active = TRUE;
    effect->id = effectId;
    effect->frequency = 260 + (effectId & 7) * 55;
    effect->samplesTotal = (s_audioSampleRate * (70 + ((effectId >> 3) & 3) * 20)) / 1000;
    if (effect->samplesTotal == 0) {
        effect->samplesTotal = 1;
    }
    effect->samplesLeft = effect->samplesTotal;
    effect->volume = volume < 0 ? 0 : (volume > 127 ? 127 : volume);
    effect->pan = pan < 0 ? 0 : (pan > 127 ? 127 : pan);
    effect->phase = 0.0f;
    if (SwitchAudioLoadSfxSample(effectId, effect)) {
        const SwitchAudioSfxEntry *entry = &s_sfxEntries[effectId];
        effect->volume = (s16)(((s32)effect->volume * entry->volume) / 127);
        effect->pan = (s16)(effect->pan + entry->pan - 64);
        if (effect->pan < 0) {
            effect->pan = 0;
        } else if (effect->pan > 127) {
            effect->pan = 127;
        }
        if (!s_reportedSfxPlayback) {
            OSReport("SwitchAudio: original SFX DSP playback active (id %u, %u samples)\n",
                     effectId, effect->sampleCount);
            s_reportedSfxPlayback = TRUE;
        }
    }
    return (s32)(s_effectSequence & 0x7FFFFFFF);
}

s32 SwitchAudioPlayEffect(u32 effectId)
{
    return SwitchAudioPlayEffectEx(effectId, 96, 64);
}

s32 SwitchAudioPlayEffectVolPan(u32 effectId, s16 volume, s16 pan)
{
    return SwitchAudioPlayEffectEx(effectId, volume, pan);
}

void SwitchAudioPlayTone(u32 frequency)
{
    /* Compatibility path for older callers; gameplay effects use their ID. */
    SwitchAudioPlayEffect(frequency);
}

void SwitchAudioFadeOutStream(s32 speed)
{
    if (!s_streamPlaying) {
        return;
    }
    if (speed <= 0) {
        s_streamFadeRemaining = 0;
        s_streamFadeTotal = 0;
        s_streamPlaying = FALSE;
        return;
    }
    s_streamFadeTotal = ((u32)s_audioSampleRate * (u32)speed) / 900;
    if (s_streamFadeTotal == 0) {
        s_streamFadeTotal = 1;
    }
    s_streamFadeRemaining = s_streamFadeTotal;
}

void SwitchAudioExit(void)
{
    if (!s_audioReady) {
        return;
    }
    for (u32 i = 0; i < SWITCH_AUDIO_EFFECT_COUNT; i++) {
        SwitchAudioFreeEffect(&s_effects[i]);
    }
    if (s_sfxBankFile != NULL) {
        fclose(s_sfxBankFile);
        s_sfxBankFile = NULL;
    }
    SwitchAudioStopStream();
    audoutStopAudioOut();
    audoutExit();
    s_audioReady = FALSE;
}
