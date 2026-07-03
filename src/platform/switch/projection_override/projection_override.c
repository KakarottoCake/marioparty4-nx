#include "projection_override.h"

float OverrideProjectionAspect(float aspect) {
#ifdef __SWITCH__
    // Hor+ scaling: scale 4:3 (1.3333f) and 1.2f aspect ratios to widescreen 16:9 (1.7777f)
    if (aspect > 1.1f && aspect < 1.4f) {
        return aspect * ((16.0f / 9.0f) / (4.0f / 3.0f));
    }
#endif
    return aspect;
}
