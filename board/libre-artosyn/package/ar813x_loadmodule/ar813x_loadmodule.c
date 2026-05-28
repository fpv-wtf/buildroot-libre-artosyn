#include <cjson/cJSON.h>
#include <cjson/cJSON_Utils.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr); exit(1);
}

static bool file_exists(const char *p) {
    return access(p, F_OK) == 0;
}

static int write_file(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) { perror(path); return -1; }
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return n < 0 ? -1 : 0;
}

static void mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (!len) die("empty dir");
    if (tmp[len-1] == '/') tmp[len-1] = 0;
    for (char *p = tmp+1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) && errno != EEXIST)
                die("mkdir %s: %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) && errno != EEXIST)
        die("mkdir %s: %s", tmp, strerror(errno));
}

static int run_cmd(bool allow_fail, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) { execvp(argv[0], argv); perror(argv[0]); _exit(127); }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        if (!allow_fail) {
            fprintf(stderr, "command failed: %s\n", argv[0]);
            return -1;
        }
    }
    return 0;
}

static void copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) die("open %s: %s", src, strerror(errno));
    int out = open(dst, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (out < 0) die("open %s: %s", dst, strerror(errno));
    char buf[65536]; ssize_t r;
    while ((r = read(in, buf, sizeof(buf))) > 0) {
        char *p = buf;
        while (r > 0) {
            ssize_t w = write(out, p, r);
            if (w < 0) die("write %s: %s", dst, strerror(errno));
            p += w; r -= w;
        }
    }
    close(out); close(in);
}

// TBD: is it same on all platforms?
static void gpio23_set(const char *value) {
    write_file("/sys/class/gpio/export", "23");
    write_file("/sys/class/gpio/gpio23/direction", "out");
    write_file("/sys/class/gpio/gpio23/value", value);
    usleep(100 * 1000);
    write_file("/sys/class/gpio/unexport", "23");
}

static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s+1 : path;
}

static char *read_file_alloc(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) die("malloc");
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) die("read %s", path);
    buf[sz] = 0; fclose(f);
    return buf;
}

static cJSON *apply_patch(cJSON *base, const char *patch_path) {
    char *txt = read_file_alloc(patch_path);
    if (!txt) die("cannot read patch: %s", patch_path);

    char *start = txt;
    if ((unsigned char)start[0]==0xEF &&
        (unsigned char)start[1]==0xBB &&
        (unsigned char)start[2]==0xBF) start += 3;

    cJSON *patch = cJSON_Parse(start);
    free(txt);
    if (!patch) {
        const char *err = cJSON_GetErrorPtr();
        die("JSON parse error in %s near: %.60s", patch_path, err ? err : "?");
    }

    /* RFC 7396 merge patch: objects recurse, scalars/arrays replace */
    cJSON *result = cJSONUtils_MergePatch(base, patch);
    if (!result) die("MergePatch failed for %s", patch_path);
    cJSON_Delete(patch);
    return result;
}

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s --fw fw.img --cfg base.json [options]\n\n"
        "Required:\n"
        "  --fw  fw.img        firmware image\n"
        "  --cfg base.json        base config JSON\n\n"
        "Options:\n"
        "  --ko      artosyn_sdio.ko    kernel module (default /mod/artosyn_sdio.ko)\n"
        "  --stage   /tmp         staging dir   (default /tmp/ar813x)\n"
        "  --patch   patch.json    patch JSON, merged in order on top of --cfg\n"
        "                         may be repeated; skipped if file does not exist\n"
        "  --no-kill              skip killall daemon/auto_sync\n"
        "  --no-reset             skip GPIO23 toggle\n"
        "  --wait-ms N            device node wait ms (default 10000)\n",
        p);
}

int main(int argc, char **argv) {
    const char *ko       = "/mod/artosyn_sdio.ko";
    const char *stage    = "/tmp/ar813x";
    const char *fw_path  = NULL;
    const char *cfg_path = NULL;
    bool  do_kill  = true;
    bool  do_reset = true;
    int   wait_ms  = 10000;

    /* patch list */
    const char **patches    = NULL;
    int          patch_count = 0;

    static struct option opts[] = {
        {"ko",       required_argument, 0, 'k'},
        {"stage",    required_argument, 0, 's'},
        {"fw",       required_argument, 0, 'f'},
        {"cfg",      required_argument, 0, 'c'},
        {"patch",    required_argument, 0, 'p'},
        {"no-kill",  no_argument,       0, 1000},
        {"no-reset", no_argument,       0, 1001},
        {"wait-ms",  required_argument, 0, 1002},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "k:s:f:c:p:h", opts, NULL)) != -1) {
        switch (c) {
        case 'k': ko       = optarg; break;
        case 's': stage    = optarg; break;
        case 'f': fw_path  = optarg; break;
        case 'c': cfg_path = optarg; break;
        case 'p':
            patches = realloc(patches, sizeof(char*) * (patch_count + 1));
            if (!patches) die("realloc");
            patches[patch_count++] = optarg;
            break;
        case 1000: do_kill  = false; break;
        case 1001: do_reset = false; break;
        case 1002: wait_ms  = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (!fw_path)  die("--fw is required");
    if (!cfg_path) die("--cfg is required");
    if (!file_exists(fw_path))  die("fw not found: %s", fw_path);
    if (!file_exists(cfg_path)) die("cfg not found: %s", cfg_path);
    if (!file_exists(ko))       die("ko not found: %s", ko);

    mkdir_p(stage);

    char fw_staged[PATH_MAX];
    snprintf(fw_staged, sizeof(fw_staged), "%s/%s", stage, basename_of(fw_path));
    printf("staging: %s -> %s\n", fw_path, fw_staged);
    copy_file(fw_path, fw_staged);

    char cfg_out[PATH_MAX];
    snprintf(cfg_out, sizeof(cfg_out), "%s/%s.merged.json", stage, basename_of(cfg_path));

    printf("base cfg: %s\n", cfg_path);

    char *base_txt = read_file_alloc(cfg_path);
    if (!base_txt) die("cannot read cfg: %s", cfg_path);
    cJSON *merged = cJSON_Parse(base_txt);
    free(base_txt);
    if (!merged) die("cannot parse cfg JSON: %s", cfg_path);

    for (int i = 0; i < patch_count; i++) {
        if (!file_exists(patches[i])) {
            printf("  patch [%d]: %s (skip — not found)\n", i+1, patches[i]);
            continue;
        }
        printf("  patch [%d]: %s\n", i+1, patches[i]);
        merged = apply_patch(merged, patches[i]);
    }

    char *out_txt = cJSON_Print(merged);
    if (!out_txt) die("cJSON_Print failed");
    cJSON_Delete(merged);

    FILE *fout = fopen(cfg_out, "w");
    if (!fout) die("cannot write %s: %s", cfg_out, strerror(errno));
    fputs(out_txt, fout);
    fputc('\n', fout);
    fclose(fout);
    free(out_txt);
    printf("merged cfg: %s\n", cfg_out);

    if (file_exists("/sys/class/net/sdio0")) {
        char *down[] = {"ifconfig", "sdio0", "down", NULL};
        run_cmd(true, down);
    }

    if (do_reset) {
        printf("assert GPIO23\n");
        gpio23_set("0");
    }

    printf("rmmod artosyn_sdio\n");
    char *rm[] = {"rmmod", "artosyn_sdio", NULL};
    run_cmd(true, rm);

    if (do_reset) {
        printf("release GPIO23\n");
        gpio23_set("1");
    }

    char fwdir[PATH_MAX];
    snprintf(fwdir, sizeof(fwdir), "%s/", stage);
    printf("firmware_class path: %s\n", fwdir);
    if (write_file("/sys/module/firmware_class/parameters/path", fwdir) != 0)
        return 1;

    char fw_arg[PATH_MAX+16], cfg_arg[PATH_MAX+16];
    snprintf(fw_arg,  sizeof(fw_arg),  "fw_name=%s",  basename_of(fw_staged));
    snprintf(cfg_arg, sizeof(cfg_arg), "cfg_name=%s", basename_of(cfg_out));

    printf("insmod %s %s %s\n", ko, fw_arg, cfg_arg);
    char *insmod[] = {"insmod", (char*)ko, fw_arg, cfg_arg, NULL};
    if (run_cmd(false, insmod) != 0) return 1;

    printf("waiting for /dev/artosyn_sdio");
    fflush(stdout);
    int loops = wait_ms / 100;
    if (loops < 1) loops = 1;
    bool ok = false;
    for (int i = 0; i < loops; i++) {
        if (file_exists("/dev/artosyn_sdio")) { ok = true; break; }
        usleep(100 * 1000);
        if (i % 10 == 9) { printf("."); fflush(stdout); }
    }
    printf("\n");
    if (!ok) { fprintf(stderr, "timeout waiting for /dev/artosyn_sdio\n"); return 1; }
    printf("ready: /dev/artosyn_sdio\n");

    return 0;
}
