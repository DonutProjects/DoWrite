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

#define VERSION "1.1"

static volatile sig_atomic_t stop = 0;

static void on_signal(int sig) { (void)sig; stop = 1; }

static int sysfs_block_path(dev_t dev, char *out, size_t n) {
    char linkpath[64];
    snprintf(linkpath, sizeof(linkpath), "/sys/dev/block/%u:%u",
             major(dev), minor(dev));

    if (!realpath(linkpath, out))
        return -1;

    if (strlen(out) >= n) {
        errno = ENAMETOOLONG;
        return -1;
    }
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

static void human_bytes(double v, char *out, size_t n) {
    const char *u[] = {"B","KiB","MiB","GiB","TiB"};
    int i=0; while (v>=1024.0 && i<4) { v/=1024.0; i++; }
    snprintf(out, n, "%.2f %s", v, u[i]);
}

static void print_usage(FILE *out, const char *prog) {
    fprintf(out, "Usage: %s [--yes] [--version] [--help] <image.iso> <device>\n", prog);
    fprintf(out, "Writes a disk image to a block device.\n");
    fprintf(out, "DoWrite %s\n", VERSION);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int skip_confirm = 0;
    int argi = 1;
    while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
        if (strcmp(argv[argi], "--yes") == 0) {
            skip_confirm = 1;
        } else if (strcmp(argv[argi], "--help") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
        } else if (strcmp(argv[argi], "--version") == 0) {
            fprintf(stdout, "%s\n", VERSION);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[argi]);
            print_usage(stderr, argv[0]);
            return 1;
        }
        argi++;
    }

    if (argc - argi != 2) {
        print_usage(stderr, argv[0]);
        return 1;
    }
    const char *src = argv[argi];
    const char *dst_arg = argv[argi + 1];
    char dst_resolved[PATH_MAX];
    const char *dst = dst_resolved;

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int sfd = open(src, O_RDONLY | O_CLOEXEC);
    if (sfd < 0) {
        fprintf(stderr, "open(%s): %s\n", src, strerror(errno));
        return 1;
    }

    struct stat st_src;
    if (fstat(sfd, &st_src) != 0) {
        fprintf(stderr, "fstat(%s): %s\n", src, strerror(errno));
        close(sfd);
        return 1;
    }
    if (!S_ISREG(st_src.st_mode)) {
        fprintf(stderr, "Source must be a regular file: %s\n", src);
        close(sfd);
        return 1;
    }
    if (st_src.st_size < 0) {
        fprintf(stderr, "Source has an invalid size: %s\n", src);
        close(sfd);
        return 1;
    }
    off_t total = st_src.st_size;

    if (!realpath(dst_arg, dst_resolved)) {
        fprintf(stderr, "realpath(%s): %s\n", dst_arg, strerror(errno));
        close(sfd);
        return 1;
    }

    struct stat st_dst;
    if (stat(dst, &st_dst) != 0) {
        fprintf(stderr, "stat(%s): %s\n", dst, strerror(errno));
        close(sfd);
        return 1;
    }
    if (!S_ISBLK(st_dst.st_mode)) {
        fprintf(stderr, "Refusing: %s is not a block device.\n", dst_arg);
        close(sfd);
        return 1;
    }
    if (check_mount_safety(dst, st_dst.st_rdev) != 0) {
        close(sfd);
        return 1;
    }

    printf("Source : %s (%lld bytes)\n", src, (long long)total);
    printf("Target : %s (BLOCK DEVICE)\n", dst);
    if (strcmp(dst_arg, dst) != 0)
        printf("Alias  : %s\n", dst_arg);
    if (skip_confirm) {
        printf("Skipping confirmation (--yes).\n");
    } else {
        printf("Type the target path to proceed: ");
        char confirm[PATH_MAX];
        if (!fgets(confirm, sizeof(confirm), stdin)) {
            close(sfd);
            return 1;
        }
        confirm[strcspn(confirm, "\r\n")] = 0;
        if (strcmp(confirm, dst) != 0) {
            printf("Aborted.\n");
            close(sfd);
            return 1;
        }
    }

#ifdef POSIX_FADV_SEQUENTIAL
    posix_fadvise(sfd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

#ifndef O_DSYNC
    #define O_DSYNC O_SYNC
#endif

    int dfd = open(dst, O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_EXCL | O_DSYNC);
    if (dfd < 0) {
        if (errno == EBUSY)
            fprintf(stderr, "open(%s): device is busy or mounted\n", dst);
        else
            fprintf(stderr, "open(%s): %s\n", dst, strerror(errno));
        close(sfd);
        return 1;
    }

    struct stat st_open_dst;
    if (fstat(dfd, &st_open_dst) != 0) {
        fprintf(stderr, "fstat(%s): %s\n", dst, strerror(errno));
        close(sfd);
        close(dfd);
        return 1;
    }
    if (!S_ISBLK(st_open_dst.st_mode) || st_open_dst.st_rdev != st_dst.st_rdev) {
        fprintf(stderr, "Refusing: %s changed after validation.\n", dst);
        close(sfd);
        close(dfd);
        return 1;
    }
    if (check_mount_safety(dst, st_open_dst.st_rdev) != 0) {
        close(sfd);
        close(dfd);
        return 1;
    }

#ifdef BLKGETSIZE64
    unsigned long long dev_bytes = 0;
    if (ioctl(dfd, BLKGETSIZE64, &dev_bytes) != 0) {
        int saved_errno = errno;
        fprintf(stderr, "Refusing: cannot determine %s capacity: %s\n",
                dst, strerror(saved_errno));
        close(sfd);
        close(dfd);
        return 1;
    } else if (dev_bytes == 0) {
        fprintf(stderr, "Refusing: cannot determine %s capacity: invalid size\n",
                dst);
        close(sfd);
        close(dfd);
        return 1;
    } else {
        if ((unsigned long long)total > dev_bytes) {
            fprintf(stderr,
                    "Refusing: %s capacity is %llu bytes, source requires %lld bytes.\n",
                    dst, dev_bytes, (long long)total);
            close(sfd);
            close(dfd);
            return 1;
        }
    }
#endif

    char *buf = malloc(BUF_SIZE);
    if (!buf) { fprintf(stderr, "malloc: %s\n", strerror(errno)); close(sfd); close(dfd); return 1; }

    printf("Writing... (Ctrl-C to abort safely)\n");
    off_t written = 0;
    int had_error = 0;
    struct timespec t0, t_last; clock_gettime(CLOCK_MONOTONIC, &t0); t_last = t0;

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
        written += r;

        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        double dt = (now.tv_sec - t_last.tv_sec) + (now.tv_nsec - t_last.tv_nsec)/1e9;
        if (dt >= 0.10 || written == total) {
            double elapsed = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec)/1e9;
            double spd = (elapsed > 0) ? (written / elapsed) : 0.0;
            double remain = (spd > 0 && total > 0) ? ( (total - written) / spd ) : 0.0;
            int pct = (total > 0) ? (int)((written * 100.0) / (double)total) : 0;
            if (pct > 100) pct = 100;

            char hw[64], hs[64];
            human_bytes((double)written, hw, sizeof(hw));
            human_bytes(spd, hs, sizeof(hs));
            printf("\r%3d%%  %s written  |  %s/s  |  ETA: %.1fs", pct, hw, hs, remain);
            fflush(stdout);
            t_last = now;
        }
    }

    if (fdatasync(dfd) != 0) {
        fprintf(stderr, "\nfdatasync: %s\n", strerror(errno));
        had_error = 1;
    }
#ifdef BLKRRPART
    if (!had_error && !stop) ioctl(dfd, BLKRRPART);
#endif

    free(buf);

    if (close(sfd) != 0 && !had_error) {
        fprintf(stderr, "\nclose(%s): %s\n", src, strerror(errno));
        had_error = 1;
    }
    if (close(dfd) != 0) {
        fprintf(stderr, "\nclose(%s): %s\n", dst, strerror(errno));
        had_error = 1;
    }

    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec)/1e9;
    char hw[64], hs[64]; human_bytes((double)written, hw, sizeof(hw));
    human_bytes( (elapsed>0)? (written/elapsed) : 0.0, hs, sizeof(hs));
    const char *status = had_error ? "Failed" : (stop ? "Aborted" : "Done");
    const char *tag = had_error ? " [ERROR]" : (stop ? " [ABORTED]" : "");
    printf("\n%s. Wrote %s in %.2fs (avg %s/s)%s\n",
           status, hw, elapsed, hs, tag);
    return had_error || stop;
}
