#include "internal/scrape/scrape_systems.h"

#include <stdio.h>
#include <stdlib.h>

static void expect_int(const char *label, int actual, int expected) {
    if (actual == expected)
        return;
    fprintf(stderr, "%s: got %d, expected %d\n", label, actual, expected);
    exit(1);
}

int main(void) {
    int ids[JW_SCRAPE_MAX_PLATFORM_IDS] = {0};

    expect_int("PC-98 count", (int)jw_scrape_platform_ids("pc98", ids, 4), 1);
    expect_int("PC-98 id", ids[0], 208);
    expect_int("Atomiswave id", jw_scrape_platform_id("atomiswave"), 53);

    expect_int("Naomi count", (int)jw_scrape_platform_ids("NAOMI", ids, 4), 3);
    expect_int("Naomi", ids[0], 56);
    expect_int("Naomi GD-ROM", ids[1], 227);
    expect_int("Naomi 2", ids[2], 230);

    ids[0] = ids[1] = 0;
    expect_int("Naomi truncated count",
               (int)jw_scrape_platform_ids("naomi", ids, 2), 3);
    expect_int("Naomi truncated first", ids[0], 56);
    expect_int("Naomi truncated second", ids[1], 227);
    expect_int("unknown", (int)jw_scrape_platform_ids("PORTS", NULL, 0), 0);

    puts("ScreenScraper platform mapping checks passed");
    return 0;
}
