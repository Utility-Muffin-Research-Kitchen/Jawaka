#include "internal/platform/cat_services.h"

#include "catastrophe.h"
#include "internal/ipc/ipc_client.h"
#include "internal/platform/wifi.h"

#include <stdio.h>
#include <string.h>

static char s_socket_path[1024];
static int s_battery_percent = -1;
static int s_charging_state = -1;
static uint32_t s_power_cache_ms = 0;

#define JW_CAT_POWER_CACHE_TTL_MS 5000

static int jw__cat_socket_ready(void) {
    return s_socket_path[0] != '\0';
}

static int jw__cat_refresh_power_cache(void) {
    if (!jw__cat_socket_ready()) {
        return -1;
    }

    uint32_t now = SDL_GetTicks();
    if (s_power_cache_ms != 0 &&
        (uint32_t)(now - s_power_cache_ms) < JW_CAT_POWER_CACHE_TTL_MS) {
        return 0;
    }

    int battery = -1;
    int charging = -1;
    if (jw_ipc_platform_power_status(s_socket_path, &battery, &charging) != 0) {
        return -1;
    }

    s_battery_percent = battery;
    s_charging_state = charging;
    s_power_cache_ms = now ? now : 1;
    return 0;
}

static int jw__cat_wifi_strength(void *userdata) {
    (void)userdata;
    if (!jw_wifi_available()) {
        return -1;
    }
    return jw_wifi_strength_now();
}

static int jw__cat_battery_percent(void *userdata) {
    (void)userdata;
    if (jw__cat_refresh_power_cache() != 0) {
        return -1;
    }
    return s_battery_percent;
}

static int jw__cat_charging_state(void *userdata) {
    (void)userdata;
    if (jw__cat_refresh_power_cache() != 0) {
        return -1;
    }
    return s_charging_state;
}

static int jw__cat_power_suspend(void *userdata) {
    (void)userdata;
    return jw__cat_socket_ready() &&
           jw_ipc_platform_action(s_socket_path, "sleep", 0) == 0
        ? CAT_OK
        : CAT_ERROR;
}

static int jw__cat_power_shutdown(void *userdata) {
    (void)userdata;
    return jw__cat_socket_ready() &&
           jw_ipc_platform_action(s_socket_path, "poweroff", 0) == 0
        ? CAT_OK
        : CAT_ERROR;
}

void jw_cat_services_install(const char *socket_path) {
    snprintf(s_socket_path, sizeof(s_socket_path), "%s", socket_path ? socket_path : "");
    s_battery_percent = -1;
    s_charging_state = -1;
    s_power_cache_ms = 0;

    cat_platform_services services;
    memset(&services, 0, sizeof(services));
    services.wifi_strength = jw__cat_wifi_strength;
    services.battery_percent = jw__cat_battery_percent;
    services.charging_state = jw__cat_charging_state;
    services.power_suspend = jw__cat_power_suspend;
    services.power_shutdown = jw__cat_power_shutdown;
    cat_set_platform_services(&services);
}

void jw_cat_services_clear(void) {
    s_socket_path[0] = '\0';
    s_battery_percent = -1;
    s_charging_state = -1;
    s_power_cache_ms = 0;
    cat_set_platform_services(NULL);
}
