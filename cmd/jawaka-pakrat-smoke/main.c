#include "internal/store/pakrat.h"
#include "internal/store/pakrat_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *action;
    const char *store_id;
    const char *version;
    jw_pakrat_context ctx;
} pakrat_smoke_opts;

static void jw__usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [options] install <store-id>\n"
        "       %s [options] install-target <store-id> <version>\n"
        "       %s [options] adopt <store-id>\n"
        "       %s [options] repair <store-id> <version>\n"
        "       %s [options] uninstall <store-id>\n"
        "       %s [options] rescan\n"
        "       %s [options] list\n"
        "\n"
        "  install replaces only paks Pak Rat owns; adopt also takes over a pak\n"
        "  already present on disk from a manual install.\n"
        "\n"
        "options:\n"
        "  --platform <id>        target platform namespace (default: PLATFORM or mac)\n"
        "  --sdcard-root <path>   SD root (default: SDCARD_PATH, JAWAKA_SDCARD_ROOT, ./mock-sdcard)\n"
        "  --state-dir <path>     UMRK internal data dir (default: <root>/.umrk/<platform>)\n"
        "  --db <path>            library DB (default: <state-dir>/library.db)\n"
        "  --platform-root <path> active platform manifest root (default: <root>/.system/leaf/platforms/<platform>)\n"
        "  --socket <path>        optional jawakad socket to notify after install/uninstall\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

static const char *jw__env_or_null(const char *name) {
    const char *value = getenv(name);
    return (value && value[0]) ? value : NULL;
}

static int jw__copy(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0 || !value) {
        return -1;
    }
    int n = snprintf(out, out_size, "%s", value);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

static int jw__join2(char *out, size_t out_size, const char *a, const char *b) {
    int n = snprintf(out, out_size, "%s/%s", a, b);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

static int jw__join3(char *out, size_t out_size, const char *a, const char *b,
                     const char *c) {
    int n = snprintf(out, out_size, "%s/%s/%s", a, b, c);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

static int jw__join5(char *out, size_t out_size, const char *a, const char *b,
                     const char *c, const char *d, const char *e) {
    int n = snprintf(out, out_size, "%s/%s/%s/%s/%s", a, b, c, d, e);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

static int jw__parse_args(int argc, char **argv, pakrat_smoke_opts *opts) {
    memset(opts, 0, sizeof(*opts));
    const char *platform = jw__env_or_null("PLATFORM");
    jw__copy(opts->ctx.platform, sizeof(opts->ctx.platform),
             platform ? platform : "mac");

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--platform") == 0 && i + 1 < argc) {
            if (jw__copy(opts->ctx.platform, sizeof(opts->ctx.platform), argv[i + 1]) != 0) {
                return -1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--sdcard-root") == 0 && i + 1 < argc) {
            if (jw__copy(opts->ctx.sdcard_root, sizeof(opts->ctx.sdcard_root), argv[i + 1]) != 0) {
                return -1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc) {
            if (jw__copy(opts->ctx.state_dir, sizeof(opts->ctx.state_dir), argv[i + 1]) != 0) {
                return -1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            if (jw__copy(opts->ctx.db_path, sizeof(opts->ctx.db_path), argv[i + 1]) != 0) {
                return -1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--platform-root") == 0 && i + 1 < argc) {
            if (jw__copy(opts->ctx.platform_root, sizeof(opts->ctx.platform_root), argv[i + 1]) != 0) {
                return -1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            if (jw__copy(opts->ctx.socket_path, sizeof(opts->ctx.socket_path), argv[i + 1]) != 0) {
                return -1;
            }
            i += 2;
        } else {
            break;
        }
    }
    if (i >= argc) {
        return -1;
    }
    opts->action = argv[i++];
    if (strcmp(opts->action, "install") == 0 ||
        strcmp(opts->action, "install-target") == 0 ||
        strcmp(opts->action, "adopt") == 0 ||
        strcmp(opts->action, "repair") == 0 ||
        strcmp(opts->action, "uninstall") == 0) {
        if (i >= argc || !argv[i][0]) {
            return -1;
        }
        opts->store_id = argv[i++];
    }
    if (strcmp(opts->action, "repair") == 0 ||
        strcmp(opts->action, "install-target") == 0) {
        if (i >= argc || !argv[i][0]) {
            return -1;
        }
        opts->version = argv[i++];
    }
    if (i != argc) {
        return -1;
    }

    if (!opts->ctx.sdcard_root[0]) {
        const char *root = jw__env_or_null("SDCARD_PATH");
        if (!root) {
            root = jw__env_or_null("JAWAKA_SDCARD_ROOT");
        }
        jw__copy(opts->ctx.sdcard_root, sizeof(opts->ctx.sdcard_root),
                 root ? root : "./mock-sdcard");
    }
    if (!opts->ctx.state_dir[0]) {
        const char *state = jw__env_or_null("UMRK_INTERNAL_DATA_PATH");
        if (state) {
            jw__copy(opts->ctx.state_dir, sizeof(opts->ctx.state_dir), state);
        } else if (jw__join3(opts->ctx.state_dir, sizeof(opts->ctx.state_dir),
                             opts->ctx.sdcard_root, ".umrk",
                             opts->ctx.platform) != 0) {
            return -1;
        }
    }
    if (!opts->ctx.db_path[0] &&
        jw__join2(opts->ctx.db_path, sizeof(opts->ctx.db_path),
                  opts->ctx.state_dir, "library.db") != 0) {
        return -1;
    }
    if (!opts->ctx.platform_root[0]) {
        const char *platform_root = jw__env_or_null("UMRK_PLATFORM_PATH");
        if (!platform_root) {
            platform_root = jw__env_or_null("SYSTEM_PATH");
        }
        if (platform_root) {
            jw__copy(opts->ctx.platform_root, sizeof(opts->ctx.platform_root),
                     platform_root);
        } else if (jw__join5(opts->ctx.platform_root,
                             sizeof(opts->ctx.platform_root),
                             opts->ctx.sdcard_root, ".system", "leaf",
                             "platforms", opts->ctx.platform) != 0) {
            return -1;
        }
    }
    if (!opts->ctx.socket_path[0]) {
        const char *socket = jw__env_or_null("JAWAKA_SOCKET_PATH");
        jw__copy(opts->ctx.socket_path, sizeof(opts->ctx.socket_path),
                 socket ? socket : "/tmp/jawaka-runtime/jawakad.sock");
    }

    return opts->ctx.platform[0] && opts->ctx.sdcard_root[0] &&
           opts->ctx.state_dir[0] && opts->ctx.db_path[0] &&
           opts->ctx.platform_root[0] ? 0 : -1;
}

static int jw__print_list(const jw_pakrat_context *ctx) {
    jw_pakrat_app_state states[128];
    int count = 0;
    int rc = jw_pakrat_list_app_states(ctx, states,
                                       (int)(sizeof(states) / sizeof(states[0])),
                                       &count);
    if (rc > 0) {
        printf("Pak Rat catalog URL is not configured\n");
        return 0;
    }
    if (rc == JW_PAKRAT_CATALOG_REQUIRES_NEWER_LEAF) {
        fprintf(stderr, "Pak Rat catalog requires a newer Leaf\n");
        return -1;
    }
    if (rc < 0) {
        fprintf(stderr, "failed to load Pak Rat app states\n");
        return -1;
    }

    for (int i = 0; i < count; i++) {
        const jw_pakrat_app_state *state = &states[i];
        printf("%s\t%s\t%s\tinstalled=%s\tmanaged=%d\tpath=Apps/%s"
               "\taction=%d\ttarget=%s\thistory=%d\tmissing_history=%d"
               "\tgated=%s\tmin_leaf=%s\n",
               jw_pakrat_app_status_name(state->status),
               state->package.id,
               state->package.version,
               state->installed_version[0] ? state->installed_version : "-",
               state->managed,
               state->package.install_path,
               state->primary_action_allowed,
               state->action_version[0] ? state->action_version : "-",
               state->action_uses_history,
               state->installed_version_missing_from_history,
               state->gated_version[0] ? state->gated_version : "-",
               state->gated_min_leaf_version[0]
                   ? state->gated_min_leaf_version
                   : "-");
    }
    return 0;
}

int main(int argc, char **argv) {
    pakrat_smoke_opts opts;
    if (jw__parse_args(argc, argv, &opts) != 0) {
        jw__usage(argv[0]);
        return 2;
    }

    int rc = -1;
    if (strcmp(opts.action, "install") == 0) {
        rc = jw_pakrat_install_app(&opts.ctx, opts.store_id, 0);
    } else if (strcmp(opts.action, "install-target") == 0) {
        rc = jw_pakrat_install_app_target(
            &opts.ctx, opts.store_id, opts.version, 0);
    } else if (strcmp(opts.action, "adopt") == 0) {
        rc = jw_pakrat_install_app(&opts.ctx, opts.store_id, 1);
    } else if (strcmp(opts.action, "repair") == 0) {
        rc = jw_pakrat_repair_app_version(
            &opts.ctx, opts.store_id, opts.version);
    } else if (strcmp(opts.action, "uninstall") == 0) {
        rc = jw_pakrat_uninstall_app(&opts.ctx, opts.store_id);
    } else if (strcmp(opts.action, "rescan") == 0) {
        rc = jw_pakrat_rescan(&opts.ctx);
    } else if (strcmp(opts.action, "list") == 0) {
        rc = jw__print_list(&opts.ctx);
    } else {
        jw__usage(argv[0]);
        rc = -1;
    }
    return rc == 0 ? 0 : 1;
}
