#include "controller.h"
#include "game/pad.h"

#ifdef __SWITCH__
PadState g_Pads[4];
static SwitchControllerType g_ControllerTypes[4] = { SWITCH_CONTROLLER_NONE };

void Switch_InitControllers(void) {
    padConfigureInput(4, HidNpadStyleSet_NpadStandard | HidNpadStyleTag_NpadGc);
    padInitializeDefault(&g_Pads[0]);
    padInitialize(&g_Pads[1], HidNpadIdType_No2);
    padInitialize(&g_Pads[2], HidNpadIdType_No3);
    padInitialize(&g_Pads[3], HidNpadIdType_No4);
}

void Switch_UpdateControllers(void) {
    int i;
    for (i = 0; i < 4; i++) {
        padUpdate(&g_Pads[i]);
        if (padIsConnected(&g_Pads[i])) {
            u32 style = padGetStyleSet(&g_Pads[i]);
            if (style & HidNpadStyleTag_NpadGc) {
                g_ControllerTypes[i] = SWITCH_CONTROLLER_GAMECUBE;
            } else if (style & HidNpadStyleTag_NpadJoyLeft) {
                g_ControllerTypes[i] = SWITCH_CONTROLLER_JOYCON_L;
            } else if (style & HidNpadStyleTag_NpadJoyRight) {
                g_ControllerTypes[i] = SWITCH_CONTROLLER_JOYCON_R;
            } else {
                g_ControllerTypes[i] = SWITCH_CONTROLLER_PRO;
            }

            u64 keys = padGetButtons(&g_Pads[i]);
            u16 button = 0;
            if (keys & HidNpadButton_A) button |= PAD_BUTTON_A;
            if (keys & HidNpadButton_B) button |= PAD_BUTTON_B;
            if (keys & HidNpadButton_X) button |= PAD_BUTTON_X;
            if (keys & HidNpadButton_Y) button |= PAD_BUTTON_Y;
            if (keys & HidNpadButton_L) button |= PAD_BUTTON_TRIGGER_L;
            if (keys & HidNpadButton_R) button |= PAD_BUTTON_TRIGGER_R;
            if (keys & HidNpadButton_ZL) button |= PAD_BUTTON_TRIGGER_L;
            if (keys & HidNpadButton_ZR) button |= PAD_TRIGGER_Z;
            if (keys & (HidNpadButton_Plus | HidNpadButton_Minus)) button |= PAD_BUTTON_START;
            if (keys & HidNpadButton_Up) button |= PAD_BUTTON_UP;
            if (keys & HidNpadButton_Down) button |= PAD_BUTTON_DOWN;
            if (keys & HidNpadButton_Left) button |= PAD_BUTTON_LEFT;
            if (keys & HidNpadButton_Right) button |= PAD_BUTTON_RIGHT;

            HuPadBtnDown[i] = ~HuPadBtn[i] & button;
            HuPadBtn[i] = button;

            HidAnalogStickState lstick = padGetStickPos(&g_Pads[i], 0);
            HuPadStkX[i] = (s8)(lstick.x / 256);
            HuPadStkY[i] = (s8)(lstick.y / 256);

            HidAnalogStickState rstick = padGetStickPos(&g_Pads[i], 1);
            HuPadSubStkX[i] = (s8)(rstick.x / 256);
            HuPadSubStkY[i] = (s8)(rstick.y / 256);
            
            HuPadErr[i] = 0; 
        } else {
            g_ControllerTypes[i] = SWITCH_CONTROLLER_NONE;
            HuPadErr[i] = -1;
        }
    }
}

SwitchControllerType Switch_GetControllerType(int player_index) {
    if (player_index < 0 || player_index >= 4) return SWITCH_CONTROLLER_NONE;
    return g_ControllerTypes[player_index];
}

const char* Switch_GetButtonGlyph(int player_index, u32 original_button) {
    SwitchControllerType type = Switch_GetControllerType(player_index);
    if (type == SWITCH_CONTROLLER_GAMECUBE) {
        switch (original_button) {
            case PAD_BUTTON_A: return "gc_btn_a";
            case PAD_BUTTON_B: return "gc_btn_b";
            case PAD_BUTTON_X: return "gc_btn_x";
            case PAD_BUTTON_Y: return "gc_btn_y";
            case PAD_TRIGGER_Z: return "gc_btn_z";
            case PAD_BUTTON_TRIGGER_L: return "gc_btn_l";
            case PAD_BUTTON_TRIGGER_R: return "gc_btn_r";
            case PAD_BUTTON_START: return "gc_btn_start";
            default: return "gc_btn_generic";
        }
    } else {
        switch (original_button) {
            case PAD_BUTTON_A: return "sw_btn_a";
            case PAD_BUTTON_B: return "sw_btn_b";
            case PAD_BUTTON_X: return "sw_btn_x";
            case PAD_BUTTON_Y: return "sw_btn_y";
            case PAD_TRIGGER_Z: return "sw_btn_zr";
            case PAD_BUTTON_TRIGGER_L: return "sw_btn_l";
            case PAD_BUTTON_TRIGGER_R: return "sw_btn_r";
            case PAD_BUTTON_START: return "sw_btn_plus";
            default: return "sw_btn_generic";
        }
    }
}
#else
void Switch_InitControllers(void) {}
void Switch_UpdateControllers(void) {}
SwitchControllerType Switch_GetControllerType(int player_index) { return SWITCH_CONTROLLER_NONE; }
const char* Switch_GetButtonGlyph(int player_index, u32 original_button) { return ""; }
#endif
