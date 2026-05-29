#include "internal/platform/device.h"

int jw_platform_clamp_brightness_percent(int percent) {
    if (percent < JW_PLATFORM_BRIGHTNESS_MIN_PERCENT) {
        return JW_PLATFORM_BRIGHTNESS_MIN_PERCENT;
    }
    if (percent > JW_PLATFORM_BRIGHTNESS_MAX_PERCENT) {
        return JW_PLATFORM_BRIGHTNESS_MAX_PERCENT;
    }
    return percent;
}
