#include "internal/platform/device.h"

#include <string.h>

const char *jw_led_mode_name(jw_led_mode mode) {
    switch (mode) {
        case JW_LED_MODE_BREATH:  return "BREATH";
        case JW_LED_MODE_RAINBOW: return "RAINBOW";
        case JW_LED_MODE_STATIC:
        default:                  return "FOREVER";
    }
}

bool jw_led_mode_parse(const char *name, jw_led_mode *out) {
    if (!name || !out) return false;
    if (strcmp(name, "FOREVER") == 0)  { *out = JW_LED_MODE_STATIC;  return true; }
    if (strcmp(name, "BREATH") == 0)   { *out = JW_LED_MODE_BREATH;  return true; }
    if (strcmp(name, "RAINBOW") == 0)  { *out = JW_LED_MODE_RAINBOW; return true; }
    return false;
}

int jw_platform_clamp_brightness_percent(int percent) {
    if (percent < JW_PLATFORM_BRIGHTNESS_MIN_PERCENT) {
        return JW_PLATFORM_BRIGHTNESS_MIN_PERCENT;
    }
    if (percent > JW_PLATFORM_BRIGHTNESS_MAX_PERCENT) {
        return JW_PLATFORM_BRIGHTNESS_MAX_PERCENT;
    }
    return percent;
}
