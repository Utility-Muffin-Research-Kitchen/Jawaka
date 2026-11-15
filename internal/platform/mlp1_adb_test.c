/* Runs the real MLP1 ADB command strings against a fake rootfs: stock paths
   swapped for temp files, and mount, chattr and usb-gadget replaced by
   recording stubs on PATH. Covers LoongOS 1.4.0.52, which has no
   /etc/init.d/S50usb-gadget.sh, and a freshly flashed rootfs with no
   /etc/.usb_config. */
#include "internal/platform/mlp1_adb.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Not assert(): several checks carry the side effect under test. */
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "mlp1-adb-test: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static char g_root[] = "/tmp/jawaka-mlp1-adb-XXXXXX";
static char g_bin[PATH_MAX];
static char g_config[PATH_MAX];
static char g_gadget[PATH_MAX];
static char g_adbd[PATH_MAX];
static char g_initd[PATH_MAX];
static char g_log[PATH_MAX];

/* snprintf that fails the test instead of truncating. */
__attribute__((format(printf, 3, 4)))
static void fmt(char *buf, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(buf, size, format, ap);
    va_end(ap);
    CHECK(n >= 0 && (size_t)n < size);
}

static void path_in_root(char *buf, const char *rel) {
    fmt(buf, PATH_MAX, "%s/%s", g_root, rel);
}

static void write_file(const char *path, const char *text, mode_t mode) {
    FILE *f = fopen(path, "w");
    CHECK(f);
    CHECK(fputs(text, f) >= 0);
    CHECK(fclose(f) == 0);
    CHECK(chmod(path, mode) == 0);
}

static char *slurp(const char *path) {
    static char text[4096];
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    return text;
}

/* Every occurrence of from becomes to, the rest verbatim. */
static void replace_all(const char *in, const char *from, const char *to,
                        char *out, size_t size) {
    size_t used = 0;
    size_t from_len = strlen(from);
    for (const char *at; (at = strstr(in, from)) != NULL; in = at + from_len) {
        int n = snprintf(out + used, size - used, "%.*s%s", (int)(at - in), in, to);
        CHECK(n >= 0 && (size_t)n < size - used);
        used += (size_t)n;
    }
    int n = snprintf(out + used, size - used, "%s", in);
    CHECK(n >= 0 && (size_t)n < size - used);
}

/* The stock command with the fake rootfs paths; returns the exit code. */
static int run(const char *command) {
    char step[2048];
    char cmd[2048];
    replace_all(command, JW_MLP1_USB_CONFIG, g_config, step, sizeof(step));
    replace_all(step, JW_MLP1_USB_GADGET, g_gadget, cmd, sizeof(cmd));
    CHECK(strstr(cmd, "/etc/") == NULL && strstr(cmd, "/usr/bin/") == NULL);
    unlink(g_log);
    int status = system(cmd);
    CHECK(status != -1 && WIFEXITED(status));
    return WEXITSTATUS(status);
}

static const char *log_text(void) {
    const char *text = slurp(g_log);
    return text ? text : "";
}

/* Stubs log "<tool> <args>" one call per line. MOUNT_FAIL and the gadget's
   FAIL_<verb> files make a call fail. */
static void install_stubs(void) {
    char stub[PATH_MAX + 16];
    char script[PATH_MAX * 3];

    fmt(stub, sizeof(stub), "%s/mount", g_bin);
    fmt(script, sizeof(script),
             "#!/bin/sh\necho \"mount $*\" >>'%s'\n"
             "[ ! -e '%s/MOUNT_FAIL' ]\n", g_log, g_root);
    write_file(stub, script, 0755);

    /* Real chattr cannot run on a temp file in CI; +i leaves a flag file. */
    fmt(stub, sizeof(stub), "%s/chattr", g_bin);
    fmt(script, sizeof(script),
             "#!/bin/sh\necho \"chattr $*\" >>'%s'\n"
             "case \"$1\" in +i) : >\"$2.immutable\" ;; -i) rm -f \"$2.immutable\" ;; esac\n",
             g_log);
    write_file(stub, script, 0755);

    fmt(script, sizeof(script),
             "#!/bin/sh\necho \"usb-gadget $*\" >>'%s'\n"
             "[ ! -e '%s/FAIL_'\"$1\" ]\n", g_log, g_root);
    write_file(g_gadget, script, 0755);
    write_file(g_adbd, "#!/bin/sh\nexit 0\n", 0755);
}

static void set_flag(const char *name, bool on) {
    char path[PATH_MAX];
    path_in_root(path, name);
    if (on) {
        write_file(path, "", 0644);
    } else {
        unlink(path);
    }
}

static bool immutable(void) {
    char path[PATH_MAX + 16];
    fmt(path, sizeof(path), "%s.immutable", g_config);
    return access(path, F_OK) == 0;
}

static void test_paths(void) {
    /* 1.4.0.52 removed the init script; the gadget binary is the interface. */
    CHECK(strcmp(JW_MLP1_USB_GADGET, "/usr/bin/usb-gadget") == 0);
    CHECK(strstr(JW_MLP1_USB_RESTART_CMD, "/etc/init.d/") == NULL);
    CHECK(strstr(JW_MLP1_USB_RESTART_CMD, JW_MLP1_USB_GADGET " restart") != NULL);
}

static void test_supported(void) {
    CHECK(jw_mlp1_adb_tools_present(g_adbd, g_gadget));

    /* An init script without the gadget binary is not enough... */
    write_file(g_initd, "#!/bin/sh\nexit 0\n", 0755);
    char moved[PATH_MAX + 16];
    fmt(moved, sizeof(moved), "%s.moved", g_gadget);
    CHECK(rename(g_gadget, moved) == 0);
    CHECK(!jw_mlp1_adb_tools_present(g_adbd, g_gadget));
    CHECK(rename(moved, g_gadget) == 0);

    /* ...and the binary alone, as on 1.4.0.52, is. */
    CHECK(unlink(g_initd) == 0);
    CHECK(jw_mlp1_adb_tools_present(g_adbd, g_gadget));

    CHECK(chmod(g_adbd, 0644) == 0);
    CHECK(!jw_mlp1_adb_tools_present(g_adbd, g_gadget));
    CHECK(chmod(g_adbd, 0755) == 0);
}

static void test_enable_creates_missing_config(void) {
    unlink(g_config);
    CHECK(run(JW_MLP1_ADB_ENABLE_CMD) == 0);
    const char *cfg = slurp(g_config);
    CHECK(cfg && strcmp(cfg, "usb_adb_en\n") == 0);
    CHECK(immutable());
    const char *log = log_text();
    const char *rw = strstr(log, "mount -o remount,rw /\n");
    const char *unpin = strstr(log, "chattr -i ");
    const char *pin = strstr(log, "chattr +i ");
    CHECK(rw && unpin && pin && rw < unpin && unpin < pin);
}

static void test_enable_repins_existing_config(void) {
    write_file(g_config, "usb_mtp_en\n", 0644);
    CHECK(run(JW_MLP1_ADB_ENABLE_CMD) == 0);
    const char *cfg = slurp(g_config);
    CHECK(cfg && strcmp(cfg, "usb_adb_en\n") == 0);
    CHECK(immutable());
}

static void test_enable_needs_writable_rootfs(void) {
    write_file(g_config, "usb_mtp_en\n", 0644);
    set_flag("MOUNT_FAIL", true);
    CHECK(run(JW_MLP1_ADB_ENABLE_CMD) != 0);
    set_flag("MOUNT_FAIL", false);
    const char *cfg = slurp(g_config);
    CHECK(cfg && strcmp(cfg, "usb_mtp_en\n") == 0);
    CHECK(strstr(log_text(), "chattr") == NULL);
}

static void test_disable_unpins(void) {
    CHECK(run(JW_MLP1_ADB_ENABLE_CMD) == 0);
    CHECK(immutable());
    CHECK(run(JW_MLP1_ADB_DISABLE_CMD) == 0);
    const char *cfg = slurp(g_config);
    CHECK(cfg && strcmp(cfg, "usb_mtp_en\n") == 0);
    CHECK(!immutable());
    CHECK(strstr(log_text(), "chattr +i") == NULL);
}

static void test_restart_calls_gadget(void) {
    CHECK(run(JW_MLP1_USB_RESTART_CMD) == 0);
    CHECK(strcmp(log_text(), "usb-gadget restart\n") == 0);
}

static void test_restart_falls_back_to_start(void) {
    set_flag("FAIL_restart", true);
    CHECK(run(JW_MLP1_USB_RESTART_CMD) == 0);
    CHECK(strcmp(log_text(), "usb-gadget restart\nusb-gadget start\n") == 0);

    set_flag("FAIL_start", true);
    CHECK(run(JW_MLP1_USB_RESTART_CMD) != 0);
    set_flag("FAIL_restart", false);
    set_flag("FAIL_start", false);
}

int main(void) {
    CHECK(mkdtemp(g_root));
    path_in_root(g_bin, "bin");
    CHECK(mkdir(g_bin, 0755) == 0);
    path_in_root(g_config, "usb_config");
    path_in_root(g_gadget, "usb-gadget");
    path_in_root(g_adbd, "adbd");
    path_in_root(g_initd, "S50usb-gadget.sh");
    path_in_root(g_log, "calls.log");

    const char *path = getenv("PATH");
    char new_path[PATH_MAX * 2];
    fmt(new_path, sizeof(new_path), "%s:%s", g_bin, path ? path : "/usr/bin:/bin");
    CHECK(setenv("PATH", new_path, 1) == 0);

    install_stubs();
    test_paths();
    test_supported();
    test_enable_creates_missing_config();
    test_enable_repins_existing_config();
    test_enable_needs_writable_rootfs();
    test_disable_unpins();
    test_restart_calls_gadget();
    test_restart_falls_back_to_start();

    char cleanup[PATH_MAX + 16];
    fmt(cleanup, sizeof(cleanup), "rm -rf '%s'", g_root);
    (void)system(cleanup);
    puts("mlp1-adb-test: ok");
    return 0;
}
