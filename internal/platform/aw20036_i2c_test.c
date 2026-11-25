/* The pure parts of aw20036_i2c.h: the channel map and the frame conversion.
   Linux hosts only (<linux/i2c-dev.h>); nothing here
   opens a bus. */
#include "internal/platform/aw20036_i2c.h"

#include <stdio.h>

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
    } \
} while (0)

static void test_channel_map(void) {
    /* Ring LED 0 is the chip's group 4: blue 12, green 13, red 14. */
    CHECK(jw_aw20036_channel(0, 2) == 12, "LED 0 blue");
    CHECK(jw_aw20036_channel(0, 1) == 13, "LED 0 green");
    CHECK(jw_aw20036_channel(0, 0) == 14, "LED 0 red");
    /* LED 5 is group 7: the last three channels. */
    CHECK(jw_aw20036_channel(5, 0) == 23, "LED 5 red");
    int seen[JW_AW20036_CHANNELS] = { 0 };
    for (int led = 0; led < JW_AW20036_LEDS; led++) {
        for (int c = 0; c < 3; c++) {
            int ch = jw_aw20036_channel(led, c);
            CHECK(ch >= 0 && ch < JW_AW20036_CHANNELS, "channel %d in range", ch);
            if (ch >= 0 && ch < JW_AW20036_CHANNELS) seen[ch]++;
        }
    }
    for (int ch = 0; ch < JW_AW20036_CHANNELS; ch++) {
        CHECK(seen[ch] == 1, "channel %d used %d times", ch, seen[ch]);
    }
}

static void test_frame(void) {
    uint32_t argb[JW_AW20036_LEDS] = { 0xff102030u, 0x80ffffffu, 0x00ff0000u };
    uint8_t dim[JW_AW20036_CHANNELS], fade[JW_AW20036_CHANNELS];
    jw_aw20036_frame(argb, JW_AW20036_LEDS, dim, fade);
    CHECK(dim[14] == 63 && dim[13] == 63 && dim[12] == 63, "LED 0 DIM is alpha >> 2");
    CHECK(fade[14] == 0x10 && fade[13] == 0x20 && fade[12] == 0x30,
          "LED 0 FADE is its colour: %02x %02x %02x", fade[14], fade[13], fade[12]);
    int ch = jw_aw20036_channel(1, 0);
    CHECK(dim[ch] == 32 && fade[ch] == 0xff, "LED 1 at alpha 0x80: DIM %d", dim[ch]);
    ch = jw_aw20036_channel(2, 0);
    CHECK(dim[ch] == 0 && fade[ch] == 0xff, "LED 2 at alpha 0 has no current");
    ch = jw_aw20036_channel(7, 1);
    CHECK(dim[ch] == 0 && fade[ch] == 0, "LED 7 dark");

    /* Fewer LEDs than the ring: the rest stay dark, whatever the buffer holds. */
    uint32_t two[2] = { 0xffffffffu, 0xffffffffu };
    jw_aw20036_frame(two, 2, dim, fade);
    ch = jw_aw20036_channel(2, 2);
    CHECK(dim[ch] == 0 && fade[ch] == 0, "LED 2 dark with two LEDs");
}

int main(void) {
    test_channel_map();
    test_frame();
    if (failures) {
        fprintf(stderr, "aw20036 tests: %d failed\n", failures);
        return 1;
    }
    printf("aw20036 tests passed\n");
    return 0;
}
