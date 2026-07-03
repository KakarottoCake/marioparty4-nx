#include <switch.h>
#include <stdio.h>
#include "controller.h"
#include "gfx_switch.h"
#include "projection_override/projection_override.h"

#include <dolphin/dvd.h>

#include <dirent.h>
void ListDir(const char* path) {
    DIR* d = opendir(path);
    if (!d) {
        printf("ListDir %s: FAILED\n", path);
        consoleUpdate(NULL);
        return;
    }
    printf("ListDir %s:\n", path);
    struct dirent* entry;
    int count = 0;
    while ((entry = readdir(d)) != NULL && count < 8) {
        printf("  %s\n", entry->d_name);
        consoleUpdate(NULL);
        count++;
    }
    closedir(d);
}

int main(int argc, char **argv) {
    consoleInit(NULL);
    
    printf("argv[0]: %s\n", argc > 0 ? argv[0] : "NULL");
    consoleUpdate(NULL);
    
    // Change working directory to NRO directory
    if (argc > 0 && argv[0]) {
        char dirPath[512];
        strncpy(dirPath, argv[0], sizeof(dirPath));
        char* lastSlash = strrchr(dirPath, '/');
        if (lastSlash) {
            *lastSlash = '\0';
            chdir(dirPath);
        }
    }
    
    char cwd[256] = "";
    getcwd(cwd, sizeof(cwd));
    printf("CWD: %s\n", cwd);
    consoleUpdate(NULL);
    
    // Mount RomFS (self). Homebrew NROs with no embedded RomFS will fail here;
    // that's fine, assets are loaded from loose files on the SD card instead.
    Result romfsRc = romfsInit();
    OSReport("romfsInit() result: 0x%x (0 = ok)\n", romfsRc);
    
    ListDir(".");
    ListDir("sys");
    ListDir("romfs:/");
    ListDir("romfs:/sys");
    
    Switch_InitControllers();
    DVDInit();
    
    printf("Mario Party 4 Switch Port Initializing...\n");
    consoleUpdate(NULL);
    
    // Quick test of the projection override
    float new_aspect = OverrideProjectionAspect(4.0f / 3.0f);
    printf("Aspect ratio override test: 4:3 -> %f\n", new_aspect);
    consoleUpdate(NULL);
    
    // Quick test of DVD redirection
    DVDFileInfo file;
    if (DVDOpen("dll/bootDll.rel", &file)) {
        printf("Redirection check: dll/bootDll.rel opened successfully! Size: %u bytes\n", file.length);
        DVDClose(&file);
    } else {
        printf("Redirection check: Failed to open dll/bootDll.rel\n");
    }
    consoleUpdate(NULL);
    
    extern void game_main(void);
    printf("Booting original game engine loop...\n");
    consoleUpdate(NULL);

    // Hand the display over from the text console to the GL renderer.
    OSReport("Reached console->GL handoff. Calling GfxInit()...\n");
    consoleExit(NULL);
    if (!GfxInit()) {
        OSReport("GfxInit() FAILED\n");
        // Fall back to console so we can at least report the failure.
        consoleInit(NULL);
        printf("GfxInit() FAILED - no GL context.\n");
        consoleUpdate(NULL);
        while (appletMainLoop()) { consoleUpdate(NULL); }
        romfsExit();
        consoleExit(NULL);
        return 1;
    }

    // game_main() runs the engine's own infinite frame loop and does not return.
    OSReport("GfxInit() OK. Entering game_main()...\n");
    game_main();

    GfxExit();
    romfsExit();
    return 0;
}
