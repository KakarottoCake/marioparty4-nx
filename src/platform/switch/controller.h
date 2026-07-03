#ifndef SWITCH_CONTROLLER_H
#define SWITCH_CONTROLLER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "types.h"

typedef enum {
    SWITCH_CONTROLLER_NONE,
    SWITCH_CONTROLLER_PRO,
    SWITCH_CONTROLLER_GAMECUBE,
    SWITCH_CONTROLLER_JOYCON_L,
    SWITCH_CONTROLLER_JOYCON_R
} SwitchControllerType;

void Switch_InitControllers(void);
void Switch_UpdateControllers(void);
SwitchControllerType Switch_GetControllerType(int player_index);
const char* Switch_GetButtonGlyph(int player_index, u32 original_button);

#ifdef __SWITCH__
extern PadState g_Pads[4];
#endif

#endif
