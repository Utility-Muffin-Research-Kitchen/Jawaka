#include "internal/platform/device.h"

#include <string.h>

/* Stock modes map to platform cfg names; effects map to helper command names.
   Both travel over the same IPC "mode" string. */
const char *jw_led_mode_name(jw_led_mode mode) {
    switch (mode) {
        case JW_LED_MODE_BREATH:   return "BREATH";
        case JW_LED_MODE_RAINBOW:  return "RAINBOW";
        case JW_LED_MODE_COMET:    return "comet";
        case JW_LED_MODE_SWEEP:    return "sweep";
        case JW_LED_MODE_FOUNTAIN: return "fountain";
        case JW_LED_MODE_HICCUP:   return "hiccup";
        case JW_LED_MODE_STATIC:
        default:                   return "FOREVER";
    }
}

bool jw_led_mode_parse(const char *name, jw_led_mode *out) {
    if (!name || !out) return false;
    if (strcmp(name, "FOREVER") == 0)  { *out = JW_LED_MODE_STATIC;   return true; }
    if (strcmp(name, "BREATH") == 0)   { *out = JW_LED_MODE_BREATH;   return true; }
    if (strcmp(name, "RAINBOW") == 0)  { *out = JW_LED_MODE_RAINBOW;  return true; }
    if (strcmp(name, "comet") == 0)    { *out = JW_LED_MODE_COMET;    return true; }
    if (strcmp(name, "sweep") == 0)    { *out = JW_LED_MODE_SWEEP;    return true; }
    if (strcmp(name, "fountain") == 0) { *out = JW_LED_MODE_FOUNTAIN; return true; }
    if (strcmp(name, "hiccup") == 0)   { *out = JW_LED_MODE_HICCUP;   return true; }
    return false;
}

bool jw_platform_parse_audio_output(const char *name, jw_platform_audio_output *out) {
    if (!name || !out) return false;
    if (strcmp(name, "SPEAKER") == 0 || strcmp(name, "speaker") == 0) {
        *out = JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
    } else if (strcmp(name, "HEADSET") == 0 || strcmp(name, "headset") == 0 ||
               strcmp(name, "HEADPHONE") == 0 || strcmp(name, "headphone") == 0) {
        *out = JW_PLATFORM_AUDIO_OUTPUT_HEADSET;
    } else if (strcmp(name, "HDMI") == 0 || strcmp(name, "hdmi") == 0) {
        *out = JW_PLATFORM_AUDIO_OUTPUT_HDMI;
    } else if (strcmp(name, "BLUETOOTH") == 0 || strcmp(name, "bluetooth") == 0 ||
               strcmp(name, "BT") == 0 || strcmp(name, "bt") == 0) {
        *out = JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH;
    } else {
        return false;
    }
    return true;
}

bool jw_platform_parse_perf_profile(const char *name, jw_platform_perf_profile *out) {
    if (!name || !out) return false;
    if (strcmp(name, "auto") == 0 || strcmp(name, "AUTO") == 0) {
        *out = JW_PLATFORM_PERF_PROFILE_AUTO;
    } else if (strcmp(name, "frontend") == 0 || strcmp(name, "FRONTEND") == 0) {
        *out = JW_PLATFORM_PERF_PROFILE_FRONTEND;
    } else if (strcmp(name, "balanced") == 0 || strcmp(name, "BALANCED") == 0) {
        *out = JW_PLATFORM_PERF_PROFILE_BALANCED;
    } else if (strcmp(name, "performance") == 0 || strcmp(name, "PERFORMANCE") == 0) {
        *out = JW_PLATFORM_PERF_PROFILE_PERFORMANCE;
    } else if (strcmp(name, "battery-saver") == 0 ||
               strcmp(name, "battery_saver") == 0 ||
               strcmp(name, "BATTERY-SAVER") == 0 ||
               strcmp(name, "BATTERY_SAVER") == 0) {
        *out = JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER;
    } else if (strcmp(name, "sleep") == 0 || strcmp(name, "SLEEP") == 0) {
        *out = JW_PLATFORM_PERF_PROFILE_SLEEP;
    } else if (strcmp(name, "custom") == 0 || strcmp(name, "CUSTOM") == 0) {
        *out = JW_PLATFORM_PERF_PROFILE_CUSTOM;
    } else {
        return false;
    }
    return true;
}

const char *jw_platform_perf_profile_name(jw_platform_perf_profile profile) {
    switch (profile) {
        case JW_PLATFORM_PERF_PROFILE_AUTO: return "auto";
        case JW_PLATFORM_PERF_PROFILE_FRONTEND: return "frontend";
        case JW_PLATFORM_PERF_PROFILE_BALANCED: return "balanced";
        case JW_PLATFORM_PERF_PROFILE_PERFORMANCE: return "performance";
        case JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER: return "battery-saver";
        case JW_PLATFORM_PERF_PROFILE_SLEEP: return "sleep";
        case JW_PLATFORM_PERF_PROFILE_CUSTOM: return "custom";
        default: return "unknown";
    }
}

const char *jw_platform_perf_profile_label(jw_platform_perf_profile profile) {
    switch (profile) {
        case JW_PLATFORM_PERF_PROFILE_AUTO: return "Auto";
        case JW_PLATFORM_PERF_PROFILE_FRONTEND: return "Frontend";
        case JW_PLATFORM_PERF_PROFILE_BALANCED: return "Balanced";
        case JW_PLATFORM_PERF_PROFILE_PERFORMANCE: return "Performance";
        case JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER: return "Battery Saver";
        case JW_PLATFORM_PERF_PROFILE_SLEEP: return "Sleep";
        case JW_PLATFORM_PERF_PROFILE_CUSTOM: return "Custom";
        default: return "Unknown";
    }
}

const char *jw_platform_audio_output_name(jw_platform_audio_output output) {
    switch (output) {
        case JW_PLATFORM_AUDIO_OUTPUT_SPEAKER: return "SPEAKER";
        case JW_PLATFORM_AUDIO_OUTPUT_HEADSET: return "HEADSET";
        case JW_PLATFORM_AUDIO_OUTPUT_HDMI: return "HDMI";
        case JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH: return "BLUETOOTH";
        default: return "UNKNOWN";
    }
}

const char *jw_platform_audio_output_label(jw_platform_audio_output output) {
    switch (output) {
        case JW_PLATFORM_AUDIO_OUTPUT_SPEAKER: return "Speaker";
        case JW_PLATFORM_AUDIO_OUTPUT_HEADSET: return "Headset";
        case JW_PLATFORM_AUDIO_OUTPUT_HDMI: return "HDMI";
        case JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH: return "Bluetooth";
        default: return "Unknown";
    }
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
