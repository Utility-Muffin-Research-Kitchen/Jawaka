#ifndef JW_DISCOVERY_DELETE_JOB_H
#define JW_DISCOVERY_DELETE_JOB_H

#include <stdbool.h>
#include <stddef.h>
#include "internal/ipc/delete_client.h"
#include "internal/discovery/content.h"

typedef struct jw_delete_job jw_delete_job;
typedef struct cJSON cJSON;

jw_delete_job *jw_delete_job_start(const char *db_path, const char *sdcard_root,
                                  const char *source_id, const char *rom_relpath,
                                  const jw_content_disc *disc);
/* These functions run on the daemon main thread. Filesystem work stays on a
   worker, so the retained connection can poll or cancel preparation. */
bool jw_delete_job_busy(jw_delete_job *job);
bool jw_delete_job_committing(jw_delete_job *job);
bool jw_delete_job_changed(jw_delete_job *job); /* consumes the refresh flag */
void jw_delete_job_cancel(jw_delete_job *job);
int jw_delete_job_commit(jw_delete_job *job, const char *token);
void jw_delete_job_reject(jw_delete_job *job, const char *message, const char *readonly_source);
cJSON *jw_delete_job_status(jw_delete_job *job);
void jw_delete_job_destroy(jw_delete_job *job);

#endif
