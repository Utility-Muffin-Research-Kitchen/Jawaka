#ifndef JW_POWER_HOLD_SAVE_IO_H
#define JW_POWER_HOLD_SAVE_IO_H

#include <stdbool.h>
#include <stddef.h>

/* File-system half of the power-hold save: RetroArch writes and fsyncs
   "<slot path>.tmp-<request id>" and replies TMP_READY; jawakad publishes it
   here. Replacement is always rename(2) over the old slot, never unlink first,
   so any failure before the rename leaves the previous quicksave intact. */

typedef enum {
    JW_PHS_PUBLISH_OK = 0,
    JW_PHS_PUBLISH_REJECTED,       /* reply path is not the pinned temporary file */
    JW_PHS_PUBLISH_THUMB_ERROR,    /* old thumbnail could not be removed; not published */
    JW_PHS_PUBLISH_RENAME_ERROR,   /* rename failed; old state untouched */
    JW_PHS_PUBLISH_DIR_SYNC_ERROR  /* renamed, but the directory flush failed */
} jw_phs_publish_result;

/* "<final_path>.tmp-<request_id>"; false on truncation or an empty id. */
bool jw_power_hold_save_tmp_path(const char *final_path, const char *request_id,
                                 char *out, size_t out_size);

/* Publish `reported_tmp` over `final_path` after checking it is exactly
   `expected_tmp` and a regular file. Removes `thumb_path` first (a missing
   thumbnail is fine) so the resume UI never pairs the new state with an old
   frame, then renames and fsyncs the containing directory. */
jw_phs_publish_result jw_power_hold_save_publish(const char *expected_tmp,
                                                 const char *reported_tmp,
                                                 const char *final_path,
                                                 const char *thumb_path,
                                                 char *error, size_t error_size);

/* Remove the pinned temporary file if present. Call only once its writer is
   gone. Returns 0 when nothing remains. */
int jw_power_hold_save_remove_tmp(const char *expected_tmp);

#endif
