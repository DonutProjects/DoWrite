#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <linux/fs.h>
#include <time.h>
#include <limits.h>

#define BUF_SIZE (1<<20) // 1 MiB
#define SYSFS_DEP_DEPTH 8

#define VERSION "1.2"

static volatile sig_atomic_t stop = 0;

static void on_signal(int sig) { (void)sig; stop = 1; }

static int sysfs_block_path(dev_t dev, char *out, size_t n) {
    char linkpath[64];
    char *resolved;
    snprintf(linkpath, sizeof(linkpath), "/sys/dev/block/%u:%u",
             major(dev), minor(dev));

    resolved = realpath(linkpath, NULL);
    if (!resolved)
        return -1;

    if (strlen(resolved) >= n) {
        free(resolved);
        errno = ENAMETOOLONG;
        return -1;
    }

    strcpy(out, resolved);
    free(resolved);
    return 0;
}

static int path_prefix_component(const char *path, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(path, prefix, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

static int is_same_or_child_block(dev_t target, dev_t mounted) {
    char target_path[PATH_MAX];
    char mounted_path[PATH_MAX];

    if (target == mounted)
        return 1;

    if (sysfs_block_path(target, target_path, sizeof(target_path)) != 0 ||
        sysfs_block_path(mounted, mounted_path, sizeof(mounted_path)) != 0)
        return 0;

    return path_prefix_component(mounted_path, target_path);
}

static int sysfs_dev_from_path(const char *path, dev_t *dev) {
    char devfile[PATH_MAX];
    unsigned int maj, min;
    FILE *fp;

    if (snprintf(devfile, sizeof(devfile), "%s/dev", path) >= (int)sizeof(devfile)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    fp = fopen(devfile, "r");
    if (!fp)
        return -1;

    if (fscanf(fp, "%u:%u", &maj, &min) != 2) {
        fclose(fp);
        errno = EINVAL;
        return -1;
    }

    fclose(fp);
    *dev = makedev(maj, min);
    return 0;
}

static int blocks_overlap(dev_t a, dev_t b, int depth);

static int slave_blocks_overlap(dev_t dev, dev_t other, int depth) {
    char dev_path[PATH_MAX];
    char slaves_path[PATH_MAX];
    DIR *dir;
    struct dirent *ent;

    if (depth <= 0 || sysfs_block_path(dev, dev_path, sizeof(dev_path)) != 0)
        return 0;

    if (snprintf(slaves_path, sizeof(slaves_path), "%s/slaves", dev_path) >=
        (int)sizeof(slaves_path))
        return 0;

    dir = opendir(slaves_path);
    if (!dir)
        return 0;

    while ((ent = readdir(dir)) != NULL) {
        char slave_link[PATH_MAX];
        char slave_path[PATH_MAX];
        dev_t slave_dev;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        if (snprintf(slave_link, sizeof(slave_link), "%s/%s",
                     slaves_path, ent->d_name) >= (int)sizeof(slave_link))
            continue;

        if (!realpath(slave_link, slave_path))
            continue;

        if (sysfs_dev_from_path(slave_path, &slave_dev) != 0)
            continue;

        if (blocks_overlap(slave_dev, other, depth - 1)) {
            closedir(dir);
            return 1;
        }
    }

    closedir(dir);
    return 0;
}

static int blocks_overlap(dev_t a, dev_t b, int depth) {
    if (is_same_or_child_block(a, b) || is_same_or_child_block(b, a))
        return 1;

    if (depth <= 0)
        return 0;

    return slave_blocks_overlap(a, b, depth) ||
           slave_blocks_overlap(b, a, depth);
}

static int has_mounted_target_or_children(dev_t target) {
    FILE *fp = fopen("/proc/self/mountinfo", "r");
    if (!fp) return -1;

    char line[8192];
    int mounted = 0;

    while (fgets(line, sizeof(line), fp)) {
        unsigned int maj, min;

        if (sscanf(line, "%*d %*d %u:%u", &maj, &min) != 2)
            continue;

        if (blocks_overlap(target, makedev(maj, min), SYSFS_DEP_DEPTH)) {
            mounted = 1;
            break;
        }
    }
    fclose(fp);
    return mounted;
}

static int has_swap_target_or_children(dev_t target) {
    FILE *fp = fopen("/proc/swaps", "r");
    if (!fp) return -1;

    char line[8192];
    int used = 0;

    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }

    while (fgets(line, sizeof(line), fp)) {
        char path[PATH_MAX];
        struct stat st;

        if (sscanf(line, "%4095s", path) != 1)
            continue;

        if (stat(path, &st) != 0 || !S_ISBLK(st.st_mode))
            continue;

        if (blocks_overlap(target, st.st_rdev, SYSFS_DEP_DEPTH)) {
            used = 1;
            break;
        }
    }

    fclose(fp);
    return used;
}

static int check_mount_safety(const char *path, dev_t target) {
    int mounted = has_mounted_target_or_children(target);
    if (mounted < 0) {
        fprintf(stderr, "Refusing: cannot inspect /proc/self/mountinfo: %s\n",
                strerror(errno));
        return -1;
    }
    if (mounted) {
        fprintf(stderr,
                "Refusing: %s or an overlapping dependent device appears to be mounted.\n",
                path);
        return -1;
    }

    int swap = has_swap_target_or_children(target);
    if (swap < 0) {
        fprintf(stderr, "Refusing: cannot inspect /proc/swaps: %s\n",
                strerror(errno));
        return -1;
    }
    if (swap) {
        fprintf(stderr,
                "Refusing: %s or an overlapping dependent device appears to be used as swap.\n",
                path);
        return -1;
    }

    return 0;
}

static ssize_t write_all(int fd, const void *buf, size_t count) {
    const char *p = (const char *)buf;
    size_t left = count;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) { errno = EIO; return -1; }
        p += w; left -= (size_t)w;
    }
    return (ssize_t)count;
}

static int read_fully(int fd, void *buf, size_t count) {
    char *p = (char *)buf;
    size_t left = count;

    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0) {
            errno = EIO;
            return -1;
        }
        p += r;
        left -= (size_t)r;
    }

    return 0;
}

static void human_bytes(double v, char *out, size_t n) {
    const char *u[] = {"B","KiB","MiB","GiB","TiB"};
    int i=0; while (v>=1024.0 && i<4) { v/=1024.0; i++; }
    snprintf(out, n, "%.2f %s", v, u[i]);
}

static void print_usage(FILE *out, const char *prog) {
    fprintf(out, "Usage: %s [--yes] [--no-verify] [--version] [--help] <image.iso> <device>\n", prog);
    fprintf(out, "Writes a disk image to a block device and verifies it by default.\n");
    fprintf(out, "DoWrite %s\n", VERSION);
}

#ifndef O_DSYNC
    #define O_DSYNC O_SYNC
#endif

enum parse_result {
    PARSE_OK,
    PARSE_DONE,
    PARSE_ERROR
};

struct cli_args {
    int skip_confirm;
    int verify;
    const char *src;
    const char *dst_arg;
};

static enum parse_result parse_args(int argc, char **argv, struct cli_args *args) {
    int argi = 1;

    while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
        if (strcmp(argv[argi], "--yes") == 0) {
            args->skip_confirm = 1;
        } else if (strcmp(argv[argi], "--no-verify") == 0) {
            args->verify = 0;
        } else if (strcmp(argv[argi], "--help") == 0) {
            print_usage(stdout, argv[0]);
            return PARSE_DONE;
        } else if (strcmp(argv[argi], "--version") == 0) {
            fprintf(stdout, "%s\n", VERSION);
            return PARSE_DONE;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[argi]);
            print_usage(stderr, argv[0]);
            return PARSE_ERROR;
        }
        argi++;
    }

    if (argc - argi != 2) {
        print_usage(stderr, argv[0]);
        return PARSE_ERROR;
    }
    args->src = argv[argi];
    args->dst_arg = argv[argi + 1];
    return PARSE_OK;
}

static void install_signal_handlers(void) {
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static int open_source_image(const char *src, off_t *total) {
    int sfd = open(src, O_RDONLY | O_CLOEXEC);
    if (sfd < 0) {
        fprintf(stderr, "open(%s): %s\n", src, strerror(errno));
        return -1;
    }

    struct stat st_src;
    if (fstat(sfd, &st_src) != 0) {
        fprintf(stderr, "fstat(%s): %s\n", src, strerror(errno));
        close(sfd);
        return -1;
    }
    if (!S_ISREG(st_src.st_mode)) {
        fprintf(stderr, "Source must be a regular file: %s\n", src);
        close(sfd);
        return -1;
    }
    if (st_src.st_size < 0) {
        fprintf(stderr, "Source has an invalid size: %s\n", src);
        close(sfd);
        return -1;
    }

    *total = st_src.st_size;
    return sfd;
}

static int validate_target_device(const char *dst_arg, char *dst_resolved,
                                  size_t dst_resolved_size,
                                  struct stat *st_dst) {
    char *resolved = realpath(dst_arg, NULL);
    if (!resolved) {
        fprintf(stderr, "realpath(%s): %s\n", dst_arg, strerror(errno));
        return -1;
    }

    if (strlen(resolved) >= dst_resolved_size) {
        free(resolved);
        errno = ENAMETOOLONG;
        fprintf(stderr, "realpath(%s): %s\n", dst_arg, strerror(errno));
        return -1;
    }
    strcpy(dst_resolved, resolved);
    free(resolved);

    if (stat(dst_resolved, st_dst) != 0) {
        fprintf(stderr, "stat(%s): %s\n", dst_resolved, strerror(errno));
        return -1;
    }
    if (!S_ISBLK(st_dst->st_mode)) {
        fprintf(stderr, "Refusing: %s is not a block device.\n", dst_arg);
        return -1;
    }
    if (check_mount_safety(dst_resolved, st_dst->st_rdev) != 0) {
        return -1;
    }

    return 0;
}

static void print_write_plan(const char *src, const char *dst_arg,
                             const char *dst, off_t total, int verify) {
    printf("Source : %s (%lld bytes)\n", src, (long long)total);
    printf("Target : %s (BLOCK DEVICE)\n", dst);
    printf("Verify: %s%s\n",
         verify ? "yes" : "no",
         verify ? " (use --no-verify to skip)" : "");
    if (strcmp(dst_arg, dst) != 0)
        printf("Alias  : %s\n", dst_arg);
}

static int confirm_target(const char *dst_arg, const char *dst,
                          int skip_confirm) {
    if (skip_confirm) {
        printf("Skipping confirmation (--yes).\n");
        return 0;
    }

    printf("Type the target path to proceed: ");
    char confirm[PATH_MAX];
    if (!fgets(confirm, sizeof(confirm), stdin)) {
        return -1;
    }
    confirm[strcspn(confirm, "\r\n")] = 0;
    if (strcmp(confirm, dst) != 0 && strcmp(confirm, dst_arg) != 0) {
        printf("Aborted.\n");
        return -1;
    }

    return 0;
}

static int open_target_device(const char *dst, const struct stat *st_dst,
                              off_t total) {
    int dfd;
    struct stat st_open_dst;

    dfd = open(dst, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_EXCL | O_DSYNC);
    if (dfd < 0) {
        if (errno == EBUSY)
            fprintf(stderr, "open(%s): device is busy or mounted\n", dst);
        else
            fprintf(stderr, "open(%s): %s\n", dst, strerror(errno));
        return -1;
    }

    if (fstat(dfd, &st_open_dst) != 0) {
        fprintf(stderr, "fstat(%s): %s\n", dst, strerror(errno));
        close(dfd);
        return -1;
    }
    if (!S_ISBLK(st_open_dst.st_mode) || st_open_dst.st_rdev != st_dst->st_rdev) {
        fprintf(stderr, "Refusing: %s changed after validation.\n", dst);
        close(dfd);
        return -1;
    }
    if (check_mount_safety(dst, st_open_dst.st_rdev) != 0) {
        close(dfd);
        return -1;
    }

#ifdef BLKGETSIZE64
    unsigned long long dev_bytes = 0;
    if (ioctl(dfd, BLKGETSIZE64, &dev_bytes) != 0) {
        int saved_errno = errno;
        fprintf(stderr, "Refusing: cannot determine %s capacity: %s\n",
                dst, strerror(saved_errno));
        close(dfd);
        return -1;
    } else if (dev_bytes == 0) {
        fprintf(stderr, "Refusing: cannot determine %s capacity: invalid size\n",
                dst);
        close(dfd);
        return -1;
    } else {
        if ((unsigned long long)total > dev_bytes) {
            fprintf(stderr,
                    "Refusing: %s capacity is %llu bytes, source requires %lld bytes.\n",
                    dst, dev_bytes, (long long)total);
            close(dfd);
            return -1;
        }
    }
#endif

    return dfd;
}

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end) {
    return (end->tv_sec - start->tv_sec) +
           (end->tv_nsec - start->tv_nsec) / 1e9;
}

static void print_progress(off_t written, off_t total,
                           const struct timespec *t0,
                           const struct timespec *now) {
    double elapsed = elapsed_seconds(t0, now);
    double spd = (elapsed > 0) ? (written / elapsed) : 0.0;
    double remain = (spd > 0 && total > 0) ? ((total - written) / spd) : 0.0;
    int pct = (total > 0) ? (int)((written * 100.0) / (double)total) : 0;
    char hw[64], hs[64];

    if (pct > 100)
        pct = 100;

    human_bytes((double)written, hw, sizeof(hw));
    human_bytes(spd, hs, sizeof(hs));
    printf("\r%3d%%  %s written  |  %s/s  |  ETA: %.1fs",
           pct, hw, hs, remain);
    fflush(stdout);
}

static void print_verify_progress(off_t verified, off_t total,
                                  const struct timespec *t0,
                                  const struct timespec *now) {
    double elapsed = elapsed_seconds(t0, now);
    double spd = (elapsed > 0) ? (verified / elapsed) : 0.0;
    double remain = (spd > 0 && total > 0) ? ((total - verified) / spd) : 0.0;
    int pct = (total > 0) ? (int)((verified * 100.0) / (double)total) : 0;
    char hv[64], hs[64];

    if (pct > 100)
        pct = 100;

    human_bytes((double)verified, hv, sizeof(hv));
    human_bytes(spd, hs, sizeof(hs));
    printf("\r%3d%%  %s verified |  %s/s  |  ETA: %.1fs",
           pct, hv, hs, remain);
    fflush(stdout);
}

static int verify_image(int sfd, int dfd, off_t total) {
    int had_error = 0;
    off_t verified = 0;
    struct timespec t0;
    struct timespec t_last;
    char *src_buf = malloc(BUF_SIZE);
    char *dst_buf = malloc(BUF_SIZE);

    if (!src_buf || !dst_buf) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        free(src_buf);
        free(dst_buf);
        return 1;
    }

    if (lseek(sfd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "\nlseek(source): %s\n", strerror(errno));
        free(src_buf);
        free(dst_buf);
        return 1;
    }
    if (lseek(dfd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "\nlseek(target): %s\n", strerror(errno));
        free(src_buf);
        free(dst_buf);
        return 1;
    }

    printf("\nVerifying...\n");
    clock_gettime(CLOCK_MONOTONIC, &t0);
    t_last = t0;

    while (verified < total) {
        off_t remaining = total - verified;
        size_t chunk = (remaining > BUF_SIZE) ? BUF_SIZE : (size_t)remaining;

        if (stop) {
            fprintf(stderr, "\nVerification interrupted.\n");
            had_error = 1;
            break;
        }

        if (read_fully(sfd, src_buf, chunk) != 0) {
            fprintf(stderr, "\nverify read source: %s\n", strerror(errno));
            had_error = 1;
            break;
        }
        if (read_fully(dfd, dst_buf, chunk) != 0) {
            fprintf(stderr, "\nverify read target: %s\n", strerror(errno));
            had_error = 1;
            break;
        }

        if (memcmp(src_buf, dst_buf, chunk) != 0) {
            size_t i;
            for (i = 0; i < chunk; i++) {
                if (src_buf[i] != dst_buf[i])
                    break;
            }
            fprintf(stderr,
                    "\nVerification failed: mismatch at byte %lld.\n",
                    (long long)(verified + (off_t)i));
            had_error = 1;
            break;
        }

        verified += (off_t)chunk;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (elapsed_seconds(&t_last, &now) >= 0.10 || verified == total) {
            print_verify_progress(verified, total, &t0, &now);
            t_last = now;
        }
    }

    if (!had_error)
        printf("\nVerification passed.\n");

    free(src_buf);
    free(dst_buf);
    return had_error;
}

static int write_image(int sfd, int dfd, off_t total, off_t *written,
                       struct timespec *t0, int *write_started, int verify) {
    int had_error = 0;
    struct timespec t_last;
    char *buf = malloc(BUF_SIZE);

    if (!buf) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        return 1;
    }

#ifdef POSIX_FADV_SEQUENTIAL
    posix_fadvise(sfd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    printf("Writing... (Ctrl-C to abort safely)\n");
    *write_started = 1;
    clock_gettime(CLOCK_MONOTONIC, t0);
    t_last = *t0;

    for (;;) {
        if (stop) break;
        ssize_t r = read(sfd, buf, BUF_SIZE);
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "\nread: %s\n", strerror(errno));
            had_error = 1;
            break;
        }
        if (r == 0) break;

        if (write_all(dfd, buf, (size_t)r) < 0) {
            fprintf(stderr, "\nwrite: %s\n", strerror(errno));
            had_error = 1;
            break;
        }
        *written += r;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (elapsed_seconds(&t_last, &now) >= 0.10 || *written == total) {
            print_progress(*written, total, t0, &now);
            t_last = now;
        }
    }

    if (fdatasync(dfd) != 0) {
        fprintf(stderr, "\nfdatasync: %s\n", strerror(errno));
        had_error = 1;
    }
    if (!had_error && !stop && verify)
        had_error = verify_image(sfd, dfd, total);
#ifdef BLKRRPART
    if (!had_error && !stop) ioctl(dfd, BLKRRPART);
#endif

    free(buf);
    return had_error;
}

static void print_write_summary(const char *dst, off_t written,
                                const struct timespec *t0, int had_error) {
    struct timespec t1;
    double elapsed;
    char hw[64], hs[64];
    const char *status;
    const char *tag;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    elapsed = elapsed_seconds(t0, &t1);
    human_bytes((double)written, hw, sizeof(hw));
    human_bytes((elapsed > 0) ? (written / elapsed) : 0.0, hs, sizeof(hs));
    status = had_error ? "Failed" : (stop ? "Aborted" : "Done");
    tag = had_error ? " [ERROR]" : (stop ? " [ABORTED]" : "");
    printf("\n%s. Wrote %s in %.2fs (avg %s/s)%s\n",
           status, hw, elapsed, hs, tag);
    if (stop && !had_error) {
        fprintf(stderr,
                "Warning: %s contains a partial image and is likely not usable.\n",
                dst);
    }
}

int main(int argc, char **argv) {
    struct cli_args args = {0};
    enum parse_result parsed;
    int sfd = -1;
    int dfd = -1;
    int exit_status = 1;
    int had_error = 0;
    int write_started = 0;
    off_t total = 0;
    off_t written = 0;
    struct timespec t0 = {0};
    struct stat st_dst;
    char dst_resolved[PATH_MAX];
    const char *dst = dst_resolved;

    setvbuf(stdout, NULL, _IONBF, 0);
    args.verify = 1;

    parsed = parse_args(argc, argv, &args);
    if (parsed == PARSE_DONE)
        return 0;
    if (parsed == PARSE_ERROR)
        return 1;

    install_signal_handlers();

    sfd = open_source_image(args.src, &total);
    if (sfd < 0)
        goto cleanup;

    if (validate_target_device(args.dst_arg, dst_resolved,
                               sizeof(dst_resolved), &st_dst) != 0)
        goto cleanup;

    print_write_plan(args.src, args.dst_arg, dst, total, args.verify);
    if (confirm_target(args.dst_arg, dst, args.skip_confirm) != 0)
        goto cleanup;

    dfd = open_target_device(dst, &st_dst, total);
    if (dfd < 0)
        goto cleanup;

    had_error = write_image(sfd, dfd, total, &written, &t0, &write_started,
                            args.verify);

cleanup:

    if (sfd >= 0 && close(sfd) != 0 && write_started && !had_error) {
        fprintf(stderr, "\nclose(%s): %s\n", args.src, strerror(errno));
        had_error = 1;
    }
    if (dfd >= 0 && close(dfd) != 0 && write_started) {
        fprintf(stderr, "\nclose(%s): %s\n", dst, strerror(errno));
        had_error = 1;
    }

    if (write_started) {
        print_write_summary(dst, written, &t0, had_error);
        exit_status = had_error || stop;
    }

    return exit_status;
}
