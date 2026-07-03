#ifdef __SWITCH__
#include <switch.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dolphin/dvd.h"

#include <dirent.h>
#include <sys/stat.h>

#define MAX_VIRTUAL_FILES 4096
static char g_VirtualFiles[MAX_VIRTUAL_FILES][256];
static u32 g_VirtualFileCount = 0;

// Root directory the game's data files actually live under. Discovered at
// startup by probing candidates, because the working directory is unreliable
// across launch environments (hbmenu on HW vs. Eden/Ryujinx loading an NRO).
static char g_AssetBase[256] = "romfs:/files";

static void ScanDirectory(const char* baseDir, const char* subDir) {
    char path[512];
    if (subDir && strlen(subDir) > 0) {
        snprintf(path, sizeof(path), "%s/%s", baseDir, subDir);
    } else {
        snprintf(path, sizeof(path), "%s", baseDir);
    }
    
    DIR* d = opendir(path);
    if (!d) return;
    
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        char relPath[512];
        if (subDir && strlen(subDir) > 0) {
            snprintf(relPath, sizeof(relPath), "%s/%s", subDir, entry->d_name);
        } else {
            snprintf(relPath, sizeof(relPath), "%s", entry->d_name);
        }
        
        char fullPath[512];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", baseDir, relPath);
        
        struct stat st;
        if (stat(fullPath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                ScanDirectory(baseDir, relPath);
            } else if (S_ISREG(st.st_mode)) {
                BOOL exists = FALSE;
                for (u32 i = 0; i < g_VirtualFileCount; i++) {
                    if (strcasecmp(g_VirtualFiles[i], relPath) == 0) {
                        exists = TRUE;
                        break;
                    }
                }
                if (!exists && g_VirtualFileCount < MAX_VIRTUAL_FILES) {
                    strncpy(g_VirtualFiles[g_VirtualFileCount++], relPath, 256);
                }
            }
        }
    }
    closedir(d);
}

static void MapDVDPath(const char* dvdPath, char* outPath, size_t maxLen) {
    // Primary: the discovered asset root (e.g. sdmc:/files on Eden).
    snprintf(outPath, maxLen, "%s/%s", g_AssetBase, dvdPath);
    FILE* f = fopen(outPath, "rb");
    if (f) {
        fclose(f);
        return;
    }
    snprintf(outPath, maxLen, "romfs:/files/%s", dvdPath);
    f = fopen(outPath, "rb");
    if (f) {
        fclose(f);
        return;
    }
    snprintf(outPath, maxLen, "romfs:/%s", dvdPath);
    f = fopen(outPath, "rb");
    if (f) {
        fclose(f);
        return;
    }
    snprintf(outPath, maxLen, "files/%s", dvdPath);
}

void DVDInit(void) {
#ifdef __SWITCH__
    g_VirtualFileCount = 0;

    // Find where the game's data files actually live.
    static const char* candidates[] = { "romfs:/files", "sdmc:/files", "files" };
    for (u32 c = 0; c < sizeof(candidates) / sizeof(candidates[0]); c++) {
        DIR* d = opendir(candidates[c]);
        if (d) {
            closedir(d);
            strncpy(g_AssetBase, candidates[c], sizeof(g_AssetBase) - 1);
            g_AssetBase[sizeof(g_AssetBase) - 1] = '\0';
            break;
        }
    }
    OSReport("DVDInit: asset base = %s\n", g_AssetBase);
    ScanDirectory(g_AssetBase, "");

    OSReport("Virtual FST Initialized. Total files found: %d\n", g_VirtualFileCount);
    for (u32 i = 0; i < (g_VirtualFileCount < 20 ? g_VirtualFileCount : 20); i++) {
        OSReport("File %d: %s\n", i, g_VirtualFiles[i]);
    }
    consoleUpdate(NULL);
#endif
}

BOOL DVDOpen(char* fileName, DVDFileInfo* fileInfo) {
#ifdef __SWITCH__
    char path[512];
    MapDVDPath(fileName, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) return FALSE;

    fseek(f, 0, SEEK_END);
    fileInfo->length = ftell(f);
    fseek(f, 0, SEEK_SET);
    fileInfo->fileHandle = f;
    return TRUE;
#else
    return FALSE;
#endif
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo) {
#ifdef __SWITCH__
    if (entrynum < 0 || entrynum >= (s32)g_VirtualFileCount) return FALSE;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_AssetBase, g_VirtualFiles[entrynum]);
    FILE* f = fopen(path, "rb");
    if (!f) return FALSE;

    fseek(f, 0, SEEK_END);
    fileInfo->length = ftell(f);
    fseek(f, 0, SEEK_SET);
    fileInfo->fileHandle = f;
    return TRUE;
#else
    return FALSE;
#endif
}

BOOL DVDClose(DVDFileInfo* f) {
#ifdef __SWITCH__
    if (f && f->fileHandle) {
        fclose((FILE*)f->fileHandle);
        f->fileHandle = NULL;
        return TRUE;
    }
#endif
    return FALSE;
}

s32 DVDGetDriveStatus() {
    return 0; // DVD_STATE_END
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) {
    return 0;
}

s32 DVDConvertPathToEntrynum(char* pathPtr) {
#ifdef __SWITCH__
    if (!pathPtr) return -1;
    if (pathPtr[0] == '/') pathPtr++;
    for (u32 i = 0; i < g_VirtualFileCount; i++) {
        if (strcasecmp(g_VirtualFiles[i], pathPtr) == 0) {
            return i;
        }
    }
#endif
    return -1;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset,
                      DVDCallback callback, s32 prio) {
#ifdef __SWITCH__
    if (!fileInfo || !fileInfo->fileHandle) return FALSE;
    FILE* f = (FILE*)fileInfo->fileHandle;
    fseek(f, offset, SEEK_SET);
    size_t read_bytes = fread(addr, 1, length, f);

    if (callback) {
        callback((s32)read_bytes, fileInfo);
    }
    return TRUE;
#else
    return FALSE;
#endif
}

BOOL DVDReadPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, s32 prio) {
#ifdef __SWITCH__
    if (!fileInfo || !fileInfo->fileHandle) return FALSE;
    FILE* f = (FILE*)fileInfo->fileHandle;
    fseek(f, offset, SEEK_SET);
    size_t read_bytes = fread(addr, 1, length, f);
    return read_bytes == (size_t)length;
#else
    return FALSE;
#endif
}

#include <stdarg.h>

// Engine logging. The text console is torn down once GL takes over the display,
// so printf-based logging would crash. We instead:
//   1) write to the system debug log (svcOutputDebugString) - shows in Ryujinx,
//   2) append to a log file on the SD card (mp4_log.txt) - readable after a run,
//      which is the only way to see boot progress on Eden (it hides svc output).
static FILE* g_LogFile = NULL;

static void LogOpen(void) {
    if (g_LogFile == NULL) {
        g_LogFile = fopen("mp4_log.txt", "w");
    }
}

void OSReport(const char* msg, ...) {
    char buf[512];
    va_list args;
    va_start(args, msg);
    int len = vsnprintf(buf, sizeof(buf), msg, args);
    va_end(args);
    if (len < 0) return;
    if (len > (int)sizeof(buf) - 1) len = sizeof(buf) - 1;
    svcOutputDebugString(buf, len);
    LogOpen();
    if (g_LogFile) {
        fwrite(buf, 1, len, g_LogFile);
        fflush(g_LogFile);
        // Force the bytes to the host file now, so the log survives a crash
        // even if the emulator hasn't flushed its own console log yet.
        fsync(fileno(g_LogFile));
    }
}

void OSPanic(const char* file, int line, const char* msg, ...) {
    char buf[512];
    va_list args;
    va_start(args, msg);
    vsnprintf(buf, sizeof(buf), msg, args);
    va_end(args);
    char line_buf[640];
    int len = snprintf(line_buf, sizeof(line_buf), "PANIC at %s:%d: %s\n", file, line, buf);
    if (len > 0) svcOutputDebugString(line_buf, len < (int)sizeof(line_buf) ? len : (int)sizeof(line_buf) - 1);
    for(;;);
}
