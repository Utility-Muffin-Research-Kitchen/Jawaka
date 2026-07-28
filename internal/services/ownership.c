#include "internal/services/ownership.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)

#include <dirent.h>

/* Parses one line of /proc/<pid>/stat. Field 2 (comm) is parenthesized and
 * may itself contain ')' or spaces, so -- like internal/power/
 * suspend_inhibit.c's jw__process_start() -- this locates the LAST ')' on
 * the line and treats everything after it as the remaining space-
 * delimited fields, per proc(5). Field 3 is state (a single character,
 * 'Z' for zombie); field 5 is pgrp. */
static bool jw__proc_stat_pgid_zombie(const char *line, pid_t *out_pgid, bool *out_zombie) {
    const char *close_paren = strrchr(line, ')');
    if (!close_paren || close_paren[1] != ' ') {
        return false;
    }
    const char *cursor = close_paren + 2; /* field 3: state */
    if (!cursor[0]) {
        return false;
    }
    *out_zombie = (cursor[0] == 'Z');
    if (cursor[1] != ' ') {
        return false;
    }
    cursor += 2; /* field 4: ppid */
    for (int field = 4; field <= 5; field++) {
        char *end = NULL;
        long value = strtol(cursor, &end, 10);
        if (end == cursor) {
            return false;
        }
        if (field == 5) {
            *out_pgid = (pid_t)value;
            return true;
        }
        while (*end == ' ') {
            end++;
        }
        cursor = end;
    }
    return false;
}

static bool jw__all_digits(const char *s) {
    if (!s || !s[0]) {
        return false;
    }
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

bool jw_svc_group_absent(pid_t pgid) {
    if (pgid <= 0) {
        return false;
    }

    DIR *proc = opendir("/proc");
    if (!proc) {
        return false; /* cannot enumerate: never claim absence */
    }

    bool absent = true;
    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL) {
        if (!jw__all_digits(entry->d_name)) {
            continue; /* not a pid directory (".", "..", "self", "net", ...) */
        }

        char stat_path[64];
        snprintf(stat_path, sizeof(stat_path), "/proc/%s/stat", entry->d_name);
        FILE *fp = fopen(stat_path, "r");
        if (!fp) {
            continue; /* exited between readdir() and open(): not a member anymore */
        }
        char line[4096];
        bool got_line = fgets(line, sizeof(line), fp) != NULL;
        fclose(fp);
        if (!got_line) {
            continue;
        }

        pid_t member_pgid = 0;
        bool zombie = false;
        if (!jw__proc_stat_pgid_zombie(line, &member_pgid, &zombie)) {
            continue; /* unparseable: treat as not a provable member, same as gone */
        }
        if (member_pgid == pgid && !zombie) {
            absent = false;
            break;
        }
    }
    closedir(proc);
    return absent;
}

#elif defined(__APPLE__)

#include <libproc.h>
#include <sys/proc.h>
#include <sys/proc_info.h>

/* macOS has no /proc; libproc's process table enumeration is the closest
 * real equivalent (used for real dev-time testing here, not just as a
 * stub -- this function is exercised by fork()/setpgid() fixtures in
 * ownership_test.c on this platform too, not only on Linux/MLP1). */
bool jw_svc_group_absent(pid_t pgid) {
    if (pgid <= 0) {
        return false;
    }

    int needed = proc_listallpids(NULL, 0);
    if (needed <= 0) {
        return false;
    }

    /* The pid list can grow between the sizing call and the fetch; retry
     * with slack a few times rather than silently under-covering it. */
    for (int attempt = 0; attempt < 4; attempt++) {
        size_t capacity = (size_t)needed + 64 + (size_t)needed / 4;
        pid_t *pids = (pid_t *)malloc(capacity * sizeof(pid_t));
        if (!pids) {
            return false;
        }
        int got_bytes = proc_listallpids(pids, (int)(capacity * sizeof(pid_t)));
        if (got_bytes < 0) {
            free(pids);
            return false;
        }
        int got_count = got_bytes / (int)sizeof(pid_t);
        if ((size_t)got_count >= capacity) {
            /* Filled the buffer exactly full: the table may have grown
             * past what we sized for. Re-size and retry rather than risk
             * missing a member. */
            free(pids);
            needed = proc_listallpids(NULL, 0);
            if (needed <= 0) {
                return false;
            }
            continue;
        }

        bool absent = true;
        for (int i = 0; i < got_count; i++) {
            struct proc_bsdinfo info;
            int r = proc_pidinfo(pids[i], PROC_PIDTBSDINFO, 0, &info, sizeof(info));
            if (r != (int)sizeof(info)) {
                continue; /* exited or inaccessible: not a provable member */
            }
            if ((pid_t)info.pbi_pgid == pgid && info.pbi_status != SZOMB) {
                absent = false;
                break;
            }
        }
        free(pids);
        return absent;
    }
    return false; /* process table kept growing past every retry: cannot prove absence */
}

#else

bool jw_svc_group_absent(pid_t pgid) {
    (void)pgid;
    return false; /* unsupported platform: never claim absence */
}

#endif
