/*
 * rico_init.c - PID 1 for the integration-test guest (a tiny initramfs holding
 * just this and rico_guest; no busybox, no shell).
 *
 * 1. mount /proc and /sys
 * 2. find the ivshmem PCI device (vendor 1af4, device 1110) in sysfs
 * 3. run /rico_guest <that device's resource2> <args from kernel cmdline>
 * 4. print RICO_GUEST_EXIT=<status> and power off, which makes QEMU exit
 *
 * Args come from the kernel command line: rico_args=10000+--pingpong+--cpu+1
 * ('+' separates words).
 *
 * Driver mode (tests rico.ko instead of the raw BAR): add
 *     rico_ko=/rico.ko  [rico_prog=/rico_drvtest]
 * init then insmods the module, waits for /dev/rico, runs the program with
 * "/dev/rico" in place of the BAR path, and finally rmmods the module and
 * prints RICO_RMMOD=<rc>.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int read_hex(const char *path, unsigned *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = fscanf(f, "%x", out) == 1;
    fclose(f);
    return ok ? 0 : -1;
}

static int find_ivshmem(char *bar, size_t len)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    struct dirent *e;
    if (!d) return -1;
    while ((e = readdir(d))) {
        char p[512];
        unsigned ven, dev;
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/vendor", e->d_name);
        if (read_hex(p, &ven)) continue;
        snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/device", e->d_name);
        if (read_hex(p, &dev)) continue;
        if (ven == 0x1af4 && dev == 0x1110) {
            snprintf(bar, len, "/sys/bus/pci/devices/%s/resource2", e->d_name);
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

/* Copy the value of `key` (e.g. "rico_ko=") from the command line into out. */
static int cmdline_val(const char *cmdline, const char *key, char *out, size_t len)
{
    const char *p = cmdline;
    size_t kl = strlen(key);
    while ((p = strstr(p, key))) {
        if (p == cmdline || p[-1] == ' ') {
            p += kl;
            size_t n = strcspn(p, " \n");
            if (n >= len) n = len - 1;
            memcpy(out, p, n);
            out[n] = 0;
            return 0;
        }
        p += kl;
    }
    return -1;
}

static int load_module(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -errno;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    void *buf = malloc(sz);
    if (!buf || fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return -EIO; }
    fclose(f);
    long rc = syscall(SYS_init_module, buf, (unsigned long)sz, "");
    int err = rc ? errno : 0;
    free(buf);
    return -err;
}

/* devtmpfs normally creates /dev/rico; fall back to mknod from sysfs. */
static int wait_for_node(const char *node)
{
    for (int i = 0; i < 100; i++) {
        struct stat st;
        if (!stat(node, &st)) return 0;
        FILE *f = fopen("/sys/class/misc/rico/dev", "r");
        if (f) {
            unsigned maj, min;
            int ok = fscanf(f, "%u:%u", &maj, &min) == 2;
            fclose(f);
            if (ok && !mknod(node, S_IFCHR | 0600, makedev(maj, min))) return 0;
        }
        usleep(50000);
    }
    return -1;
}

int main(void)
{
    int status = 127;
    char cmdline[4096] = "", bar[512], ko[256] = "", prog[256] = "/rico_guest";
    int rmmod_rc = -1;

    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);   /* EBUSY if already mounted: fine */

    FILE *f = fopen("/proc/cmdline", "r");
    if (f) { if (!fgets(cmdline, sizeof cmdline, f)) cmdline[0] = 0; fclose(f); }
    char *a = strstr(cmdline, "rico_args=");
    if (!a) { puts("rico_init: no rico_args= on cmdline"); goto done; }
    a += strlen("rico_args=");
    a[strcspn(a, " \n")] = 0;
    cmdline_val(cmdline, "rico_ko=", ko, sizeof ko);
    cmdline_val(cmdline, "rico_prog=", prog, sizeof prog);

    if (ko[0]) {
        int rc = load_module(ko);
        printf("rico_init: insmod %s: %s\n", ko, rc ? strerror(-rc) : "ok");
        if (rc) { status = 125; goto done; }
        if (wait_for_node("/dev/rico")) { puts("rico_init: /dev/rico did not appear"); status = 124; goto unload; }
        puts("rico_init: /dev/rico present");
        snprintf(bar, sizeof bar, "/dev/rico");
    } else if (find_ivshmem(bar, sizeof bar)) {
        puts("rico_init: ivshmem device not found");
        goto done;
    }
    printf("rico_init: using %s\n", bar);

    char *argv[64] = { prog, bar };
    int argc = 2;
    for (char *tok = strtok(a, "+"); tok && argc < 63; tok = strtok(NULL, "+"))
        argv[argc++] = tok;
    argv[argc] = NULL;
    fflush(stdout);

    pid_t pid = fork();
    if (pid == 0) { execv(argv[0], argv); perror("exec"); _exit(126); }
    int st = 0;
    waitpid(pid, &st, 0);
    status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);

unload:
    if (ko[0]) {
        rmmod_rc = syscall(SYS_delete_module, "rico", O_NONBLOCK) ? errno : 0;
        printf("RICO_RMMOD=%d\n", rmmod_rc);
    }
done:
    printf("RICO_GUEST_EXIT=%d\n", status);
    fflush(stdout);
    sync();
    reboot(RB_POWER_OFF);
    for (;;) pause();
}
