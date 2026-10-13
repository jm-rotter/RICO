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
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
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

int main(void)
{
    int status = 127;
    char cmdline[4096] = "", bar[512];

    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);

    FILE *f = fopen("/proc/cmdline", "r");
    if (f) { if (!fgets(cmdline, sizeof cmdline, f)) cmdline[0] = 0; fclose(f); }
    char *a = strstr(cmdline, "rico_args=");
    if (!a) { puts("rico_init: no rico_args= on cmdline"); goto done; }
    a += strlen("rico_args=");
    a[strcspn(a, " \n")] = 0;

    if (find_ivshmem(bar, sizeof bar)) { puts("rico_init: ivshmem device not found"); goto done; }
    printf("rico_init: using %s\n", bar);

    char *argv[64] = { "/rico_guest", bar };
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

done:
    printf("RICO_GUEST_EXIT=%d\n", status);
    fflush(stdout);
    sync();
    reboot(RB_POWER_OFF);
    for (;;) pause();
}
