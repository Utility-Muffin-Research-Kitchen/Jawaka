#include "internal/storage/sources.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    setenv("SDCARD_PATHS", "/card1:/card2", 1);
    setenv("UMRK_SECONDARY_SDCARD_PATH", "/card2", 1);
    setenv("MUSIC_PATHS", "/music1:/music2", 1);

    jw_storage_source_list sources;
    assert(jw_storage_sources_resolve("/card1", &sources) == 0);
    assert(sources.count == 2);
    assert(strcmp(sources.sources[0].id, "primary") == 0);
    assert(strcmp(sources.sources[0].music_path, "/music1") == 0);
    assert(strcmp(sources.sources[1].id, "secondary_sd") == 0);
    assert(strcmp(sources.sources[1].music_path, "/music2") == 0);

    unsetenv("MUSIC_PATHS");
    setenv("MUSIC_PATH", "/primary-music", 1);
    assert(jw_storage_sources_resolve("/card1", &sources) == 0);
    assert(strcmp(sources.sources[0].music_path, "/primary-music") == 0);
    assert(strcmp(sources.sources[1].music_path, "/card2/Music") == 0);
    return 0;
}
