/*
 * dsh-eval-guard seccomp supervisor.
 *
 * Installs a seccomp user-notification filter before exec'ing bwrap and enforces
 * a path policy on file-access and network syscalls. Normal syscalls are not
 * logged; only denied operations are appended to the audit file and cause the
 * whole process group to be killed.
 *
 * Usage:
 *   dsh-eval-supervisor --policy <policy-file> --audit <audit-file> -- <program> [args...]
 *
 * Policy file (line oriented, '#' comments):
 *   workspace <abs-path>       read/write allowed subtree
 *   read <abs-path>            read-only allowed subtree (prefix)
 *   exact <abs-path>           read-only allowed exact path (directory traversal)
 *   dev <abs-path>             read/write allowed device/no-trace file
 *   rule deny <abs-path>       ordered read rule (later rules win)
 *   rule allow <abs-path>      ordered read rule (later rules win)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)
#endif

#ifndef SYS_openat2
#define SYS_openat2 437
#endif
#ifndef SYS_faccessat2
#define SYS_faccessat2 439
#endif
#ifndef SYS_io_uring_setup
#define SYS_io_uring_setup 425
#endif
#ifndef SYS_open_by_handle_at
#define SYS_open_by_handle_at 304
#endif

#define MAX_RULES 512
#define MAX_PATH 4096

typedef struct {
  char path[MAX_PATH];
  int allow;
} Rule;

static char g_workspace[MAX_PATH];
static char g_read[MAX_RULES][MAX_PATH];
static int g_read_count;
static char g_exact[MAX_RULES][MAX_PATH];
static int g_exact_count;
static char g_dev[MAX_RULES][MAX_PATH];
static int g_dev_count;
static Rule g_rules[MAX_RULES];
static int g_rule_count;
static int g_audit_fd = -1;
static volatile sig_atomic_t g_stop;
/* bwrap itself needs AF_UNIX and AF_NETLINK sockets during setup. The target
 * command is allowed no sockets at all once bwrap has exec'd it. */
static int g_command_started;
static pid_t g_current_pid;
static int g_tmpbind;

/** Whether the notifying process is bubblewrap itself (setup phase). */
static int is_bwrap(pid_t pid) {
  char proc[64];
  char exe[MAX_PATH];
  snprintf(proc, sizeof(proc), "/proc/%d/exe", (int)pid);
  ssize_t n = readlink(proc, exe, sizeof(exe) - 1);
  if (n < 0) return 0;
  exe[n] = '\0';
  const char *base = strrchr(exe, '/');
  base = base == NULL ? exe : base + 1;
  return strstr(base, "bwrap") != NULL;
}

static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

static void die(const char *msg) {
  fprintf(stderr, "dsh-eval-supervisor: %s: %s\n", msg, strerror(errno));
  _exit(125);
}

static int starts_with_seg(const char *path, const char *prefix) {
  size_t n;
  if (path == NULL || prefix == NULL || prefix[0] == '\0') return 0;
  n = strlen(prefix);
  if (n == 1 && prefix[0] == '/') return path[0] == '/';
  if (strncmp(path, prefix, n) != 0) return 0;
  return path[n] == '\0' || path[n] == '/';
}

static void lexical_normalize(char *p) {
  char out[MAX_PATH * 2];
  size_t oi = 0;
  char *seg;
  char *save = NULL;
  int absolute = p[0] == '/';
  char *copy = strdup(p);
  if (copy == NULL) return;
  if (absolute) out[oi++] = '/';
  for (seg = strtok_r(copy, "/", &save); seg != NULL; seg = strtok_r(NULL, "/", &save)) {
    if (strcmp(seg, ".") == 0) continue;
    if (strcmp(seg, "..") == 0) {
      while (oi > 0 && out[oi - 1] != '/') oi--;
      if (oi > 0 && out[oi - 1] == '/' && oi > 1) oi--;
      continue;
    }
    if (oi > 0 && out[oi - 1] != '/') out[oi++] = '/';
    size_t sl = strlen(seg);
    if (oi + sl + 1 >= sizeof(out)) break;
    memcpy(out + oi, seg, sl);
    oi += sl;
  }
  if (oi == 0) { out[oi++] = absolute ? '/' : '.'; }
  out[oi] = '\0';
  /* collapse a duplicate leading slash produced above */
  if (out[0] == '/' && out[1] == '/') memmove(out, out + 1, strlen(out));
  snprintf(p, MAX_PATH, "%s", out);
  free(copy);
}

static int read_link_path(const char *proc_path, char *out, size_t outsz) {
  ssize_t n = readlink(proc_path, out, outsz - 1);
  if (n < 0) return -1;
  out[n] = '\0';
  return 0;
}

static int read_remote(pid_t pid, unsigned long addr, void *buf, size_t len) {
  struct iovec local = { .iov_base = buf, .iov_len = len };
  struct iovec remote = { .iov_base = (void *)addr, .iov_len = len };
  ssize_t n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
  if (n == (ssize_t)len) return 0;
  char mem[64];
  int fd;
  snprintf(mem, sizeof(mem), "/proc/%d/mem", (int)pid);
  fd = open(mem, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  n = pread(fd, buf, len, (off_t)addr);
  close(fd);
  return n == (ssize_t)len ? 0 : -1;
}

static int read_remote_string(pid_t pid, unsigned long addr, char *out, size_t outsz) {
  size_t off = 0;
  if (addr == 0) return -1;
  while (off + 1 < outsz) {
    char chunk[128];
    size_t want = sizeof(chunk);
    if (off + want > outsz - 1) want = outsz - 1 - off;
    if (want == 0) break;
    if (read_remote(pid, addr + off, chunk, want) != 0) {
      /* A short read may still have delivered bytes; take what we need. */
      for (size_t i = 0; i < want; i++) {
        if (chunk[i] == '\0') { out[off + i] = '\0'; return 0; }
        out[off + i] = chunk[i];
      }
      return -1;
    }
    for (size_t i = 0; i < want; i++) {
      if (chunk[i] == '\0') { out[off + i] = '\0'; return 0; }
      out[off + i] = chunk[i];
    }
    off += want;
  }
  out[outsz - 1] = '\0';
  return 0;
}

/* Resolve the deepest existing ancestor through /proc/<pid>/root and append
 * the missing tail. This maps bind mounts (notably /tmp) correctly even when
 * the target file does not exist yet. */
static int resolve_best(pid_t pid, const char *candidate, int *existed, char *out, size_t outsz) {
  char current[MAX_PATH * 2];
  char tail[MAX_PATH * 2];
  tail[0] = '\0';
  snprintf(current, sizeof(current), "%s", candidate);
  *existed = 0;
  for (;;) {
    char root[MAX_PATH * 3];
    char real[MAX_PATH * 3];
    snprintf(root, sizeof(root), "/proc/%d/root%s", (int)pid, current);
    if (realpath(root, real) != NULL) {
      *existed = tail[0] == '\0';
      if (tail[0] == '\0') snprintf(out, outsz, "%s", real);
      else snprintf(out, outsz, "%s/%s", real, tail);
      lexical_normalize(out);
      return 0;
    }
    char *slash = strrchr(current, '/');
    if (slash == NULL) break;
    char comp[MAX_PATH * 2];
    snprintf(comp, sizeof(comp), "%s", slash + 1);
    char next_tail[MAX_PATH * 2];
    if (tail[0] == '\0') snprintf(next_tail, sizeof(next_tail), "%s", comp);
    else snprintf(next_tail, sizeof(next_tail), "%s/%s", comp, tail);
    snprintf(tail, sizeof(tail), "%s", next_tail);
    if (slash == current) current[1] = '\0';
    else *slash = '\0';
  }
  snprintf(out, outsz, "%s", candidate);
  return 0;
}

static int resolve_path(pid_t pid, int dirfd, const char *path, int *existed, char *out, size_t outsz) {
  char candidate[MAX_PATH * 2];
  char base[MAX_PATH];

  if (path[0] == '/') {
    snprintf(candidate, sizeof(candidate), "%s", path);
  } else {
    if (dirfd == AT_FDCWD) {
      char proc[MAX_PATH];
      snprintf(proc, sizeof(proc), "/proc/%d/cwd", (int)pid);
      if (read_link_path(proc, base, sizeof(base)) != 0) return -1;
    } else {
      char proc[MAX_PATH];
      snprintf(proc, sizeof(proc), "/proc/%d/fd/%d", (int)pid, dirfd);
      if (read_link_path(proc, base, sizeof(base)) != 0) return -1;
    }
    snprintf(candidate, sizeof(candidate), "%s/%s", base, path);
  }
  lexical_normalize(candidate);
  /* After pivot_root the target's /tmp is the workspace .tmp bind, and a bind
   * mount hides its source from realpath. Map it explicitly. */
  if (g_tmpbind && g_command_started && !is_bwrap(pid) && (strcmp(candidate, "/tmp") == 0 || starts_with_seg(candidate, "/tmp"))) {
    snprintf(out, outsz, "%s/.tmp%s", g_workspace, candidate + 4);
    *existed = 0;
    return 0;
  }
  return resolve_best(pid, candidate, existed, out, outsz);
}

static int is_dev_path(const char *path) {
  static const char *const devices[] = {
    "/dev/null", "/dev/zero", "/dev/full", "/dev/random", "/dev/urandom",
    "/dev/tty", "/dev/ptmx", "/dev/pts", "/dev/fd", "/dev/stdin", "/dev/stdout", "/dev/stderr"
  };
  for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
    if (starts_with_seg(path, devices[i])) return 1;
  }
  for (int i = 0; i < g_dev_count; i++) {
    if (starts_with_seg(path, g_dev[i])) return 1;
  }
  return 0;
}

static int base_read_allowed(const char *path) {
  if (g_workspace[0] != '\0' && starts_with_seg(path, g_workspace)) return 1;
  if (is_dev_path(path)) return 1;
  if (strcmp(path, "/") == 0) return 1;
  if (starts_with_seg(path, "/proc")) return 1;
  for (int i = 0; i < g_exact_count; i++) {
    if (strcmp(path, g_exact[i]) == 0) return 1;
  }
  for (int i = 0; i < g_read_count; i++) {
    if (starts_with_seg(path, g_read[i])) return 1;
  }
  return 0;
}

static int rules_override_read(const char *path, int initial) {
  int allowed = initial;
  for (int i = 0; i < g_rule_count; i++) {
    if (starts_with_seg(path, g_rules[i].path)) allowed = g_rules[i].allow;
  }
  return allowed;
}

static int path_allowed(const char *path, int write_access) {
  if (g_workspace[0] != '\0' && starts_with_seg(path, g_workspace)) return 1;
  /* bubblewrap builds its new root under /tmp and /newroot before pivot_root.
   * Those transient paths exist only during setup and are removed on exit. */
  if ((!g_command_started || (g_current_pid > 0 && is_bwrap(g_current_pid))) &&
      (starts_with_seg(path, "/newroot") || starts_with_seg(path, "/oldroot") ||
       starts_with_seg(path, "/tmp/newroot") || starts_with_seg(path, "/tmp/oldroot"))) return 1;
  if (is_dev_path(path)) return 1;
  if (write_access) return 0;
  return rules_override_read(path, base_read_allowed(path));
}

static void json_escape(const char *in, char *out, size_t outsz) {
  size_t o = 0;
  if (outsz == 0) return;
  for (size_t i = 0; in[i] != '\0' && o + 2 < outsz; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '"' || c == '\\') {
      out[o++] = '\\';
      out[o++] = (char)c;
    } else if (c == '\n') {
      out[o++] = '\\';
      out[o++] = 'n';
    } else if (c < 0x20) {
      o += (size_t)snprintf(out + o, outsz - o, "\\u%04x", c);
    } else {
      out[o++] = (char)c;
    }
  }
  out[o < outsz ? o : outsz - 1] = '\0';
}

static void audit_record(pid_t pid, const char *syscall_name, const char *path, int write_access) {
  char escaped[MAX_PATH * 2];
  char line[MAX_PATH * 3];
  char ts[64];
  struct timespec now;
  int n;
  if (g_audit_fd < 0) return;
  clock_gettime(CLOCK_REALTIME, &now);
  snprintf(ts, sizeof(ts), "%ld.%03ld", (long)now.tv_sec, now.tv_nsec / 1000000L);
  json_escape(path == NULL ? "" : path, escaped, sizeof(escaped));
  n = snprintf(line, sizeof(line),
               "{\"ts\":%s,\"event\":\"process-denied\",\"pid\":%d,\"syscall\":\"%s\",\"path\":\"%s\",\"access\":\"%s\"}\n",
               ts, (int)pid, syscall_name, escaped, write_access ? "write" : "read");
  if (n > 0) (void)write(g_audit_fd, line, (size_t)n);
}

__attribute__((unused)) static void audit_note(const char *kind, const char *detail) {
  char line[4096];
  char escaped[2048];
  struct timespec now;
  int n;
  if (g_audit_fd < 0) return;
  clock_gettime(CLOCK_REALTIME, &now);
  json_escape(detail == NULL ? "" : detail, escaped, sizeof(escaped));
  n = snprintf(line, sizeof(line),
               "{\"ts\":%ld.%03ld,\"event\":\"%s\",\"detail\":\"%s\"}\n",
               (long)now.tv_sec, now.tv_nsec / 1000000L, kind, escaped);
  if (n > 0) (void)write(g_audit_fd, line, (size_t)n);
}

/* Syscall numbers we intercept. */
__attribute__((unused)) static int should_notify(long nr) {
  switch (nr) {
    case SYS_open:
    case SYS_openat:
    case SYS_openat2:
    case SYS_creat:
    case SYS_unlink:
    case SYS_unlinkat:
    case SYS_rename:
    case SYS_renameat:
#ifdef SYS_renameat2
    case SYS_renameat2:
#endif
    case SYS_mkdir:
    case SYS_mkdirat:
    case SYS_rmdir:
    case SYS_truncate:
    case SYS_chmod:
    case SYS_fchmodat:
    case SYS_chown:
    case SYS_lchown:
    case SYS_fchownat:
    case SYS_stat:
    case SYS_lstat:
    case SYS_newfstatat:
    case SYS_access:
    case SYS_faccessat:
    case SYS_faccessat2:
    case SYS_readlink:
    case SYS_readlinkat:
    case SYS_socket:
    case SYS_connect:
    case SYS_bind:
    case SYS_listen:
    case SYS_accept:
    case SYS_accept4:
    case SYS_sendto:
    case SYS_recvfrom:
#ifdef SYS_io_uring_setup
    case SYS_io_uring_setup:
#endif
#ifdef SYS_open_by_handle_at
    case SYS_open_by_handle_at:
#endif
    case SYS_ptrace:
      return 1;
    default:
      return 0;
  }
}

static int is_network_syscall(long nr) {
  switch (nr) {
    case SYS_socket:
    case SYS_connect:
    case SYS_bind:
    case SYS_listen:
    case SYS_accept:
    case SYS_accept4:
    case SYS_sendto:
    case SYS_recvfrom:
      return 1;
    default:
      return 0;
  }
}

static const char *syscall_name(long nr) {
  switch (nr) {
    case SYS_open: return "open";
    case SYS_openat: return "openat";
    case SYS_openat2: return "openat2";
    case SYS_creat: return "creat";
    case SYS_unlink: return "unlink";
    case SYS_unlinkat: return "unlinkat";
    case SYS_rename: return "rename";
    case SYS_renameat: return "renameat";
#ifdef SYS_renameat2
    case SYS_renameat2: return "renameat2";
#endif
    case SYS_mkdir: return "mkdir";
    case SYS_mkdirat: return "mkdirat";
    case SYS_rmdir: return "rmdir";
    case SYS_truncate: return "truncate";
    case SYS_chmod: return "chmod";
    case SYS_fchmodat: return "fchmodat";
    case SYS_chown: return "chown";
    case SYS_lchown: return "lchown";
    case SYS_fchownat: return "fchownat";
    case SYS_stat: return "stat";
    case SYS_lstat: return "lstat";
    case SYS_newfstatat: return "newfstatat";
    case SYS_access: return "access";
    case SYS_faccessat: return "faccessat";
    case SYS_faccessat2: return "faccessat2";
    case SYS_readlink: return "readlink";
    case SYS_readlinkat: return "readlinkat";
    case SYS_socket: return "socket";
    case SYS_connect: return "connect";
    case SYS_bind: return "bind";
    case SYS_listen: return "listen";
    case SYS_accept: return "accept";
    case SYS_accept4: return "accept4";
    case SYS_sendto: return "sendto";
    case SYS_recvfrom: return "recvfrom";
#ifdef SYS_sendmsg
    case SYS_sendmsg: return "sendmsg";
#endif
#ifdef SYS_recvmsg
    case SYS_recvmsg: return "recvmsg";
#endif
#ifdef SYS_io_uring_setup
    case SYS_io_uring_setup: return "io_uring_setup";
#endif
#ifdef SYS_open_by_handle_at
    case SYS_open_by_handle_at: return "open_by_handle_at";
#endif
    case SYS_ptrace: return "ptrace";
    default: return "unknown";
  }
}

/* Returns 1 when the syscall is allowed, 0 when it must be denied. */
static int check_notification(pid_t pid, const struct seccomp_notif *req, int *write_access, char *path_out) {
  long nr = (long)req->data.nr;
  unsigned long a0 = (unsigned long)req->data.args[0];
  unsigned long a1 = (unsigned long)req->data.args[1];
  unsigned long a2 = (unsigned long)req->data.args[2];
  unsigned long a3 = (unsigned long)req->data.args[3];
  char raw[MAX_PATH];
  char path[MAX_PATH];
  int existed = 0;
  int write = 0;
  int dirfd = AT_FDCWD;
  unsigned long path_ptr = 0;

  path_out[0] = '\0';
  g_current_pid = pid;
  if (nr == SYS_execve || nr == SYS_execveat) {
    char executable[MAX_PATH];
    executable[0] = '\0';
    (void)read_remote_string(pid, nr == SYS_execve ? a0 : a1, executable, sizeof(executable));
    const char *base = strrchr(executable, '/');
    base = base == NULL ? executable : base + 1;
    if (strstr(base, "bwrap") == NULL) g_command_started = 1;
    *write_access = 0;
    snprintf(path_out, MAX_PATH, "<exec>");
    return 1;
  }
  if (nr == SYS_socket) {
    int domain = (int)a0;
    *write_access = 0;
    snprintf(path_out, MAX_PATH, "<network:socket>");
    if (is_bwrap(pid) && (domain == AF_UNIX || domain == AF_NETLINK)) return 1;
    return 0;
  }
  if (is_network_syscall(nr)) {
    *write_access = 0;
    snprintf(path_out, MAX_PATH, "<network:%s>", syscall_name(nr));
    if (is_bwrap(pid)) return 1;
    return 0;
  }
  if (nr == SYS_io_uring_setup || nr == SYS_ptrace || nr == SYS_open_by_handle_at) {
    *write_access = 0;
    snprintf(path_out, MAX_PATH, "<forbidden:%s>", syscall_name(nr));
    if (nr == SYS_io_uring_setup) {
      char proc[64];
      char exe[MAX_PATH];
      snprintf(proc, sizeof(proc), "/proc/%d/exe", (int)pid);
      ssize_t exe_n = readlink(proc, exe, sizeof(exe) - 1);
      if (exe_n > 0) {
        exe[exe_n] = '\0';
        const char *base = strrchr(exe, '/');
        base = base == NULL ? exe : base + 1;
        if (strncmp(base, "node", 4) == 0) return 1;
      }
    }
    return 0;
  }

  switch (nr) {
    case SYS_open:
      path_ptr = a0;
      write = ((int)a1 & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) != 0;
      break;
    case SYS_openat:
      dirfd = (int)a0;
      path_ptr = a1;
      write = ((int)a2 & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) != 0;
      break;
    case SYS_openat2: {
      struct { uint64_t flags; uint64_t mode; uint64_t resolve; } how;
      dirfd = (int)a0;
      path_ptr = a1;
      memset(&how, 0, sizeof(how));
      if ((size_t)a3 >= sizeof(how) && read_remote(pid, a2, &how, sizeof(how)) == 0) {
        write = ((int)how.flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) != 0;
      } else {
        write = 1;
      }
      break;
    }
    case SYS_creat:
      path_ptr = a0;
      write = 1;
      break;
    case SYS_rename:
      /* Both paths are checked by the kernel under the same denial; inspect the source. */
      path_ptr = a0;
      write = 1;
      break;
    case SYS_renameat:
    case SYS_renameat2:
      dirfd = (int)a0;
      path_ptr = a1;
      write = 1;
      break;
    case SYS_unlink:
    case SYS_rmdir:
    case SYS_truncate:
    case SYS_chmod:
    case SYS_chown:
    case SYS_lchown:
    case SYS_mkdir:
    case SYS_stat:
    case SYS_lstat:
    case SYS_access:
    case SYS_readlink:
      path_ptr = a0;
      write = (nr == SYS_unlink || nr == SYS_rmdir || nr == SYS_truncate || nr == SYS_chmod || nr == SYS_chown || nr == SYS_lchown || nr == SYS_mkdir);
      break;
    case SYS_unlinkat:
    case SYS_mkdirat:
    case SYS_newfstatat:
    case SYS_faccessat:
    case SYS_faccessat2:
    case SYS_readlinkat:
    case SYS_fchmodat:
    case SYS_fchownat:
      dirfd = (int)a0;
      path_ptr = a1;
      write = (nr == SYS_unlinkat || nr == SYS_mkdirat || nr == SYS_fchmodat || nr == SYS_fchownat);
      break;
    default:
      *write_access = 0;
      return 1;
  }

  if (read_remote_string(pid, path_ptr, raw, sizeof(raw)) != 0) {
    snprintf(path_out, MAX_PATH, "<unreadable>");
    *write_access = write;
    return 0;
  }
  /* An empty pathname is the fd-only form (AT_EMPTY_PATH): no filesystem path is touched. */
  if (raw[0] == '\0') {
    snprintf(path_out, MAX_PATH, "<fd>");
    *write_access = write;
    return 1;
  }
  if (resolve_path(pid, dirfd, raw, &existed, path, sizeof(path)) != 0) {
    snprintf(path_out, MAX_PATH, "%s", raw);
    *write_access = write;
    return 0;
  }
  (void)existed;
  /* File descriptors whose /proc link is not a filesystem path are already-open objects. */
  if (strncmp(path, "socket:[", 8) == 0 || strncmp(path, "pipe:[", 6) == 0 ||
      strncmp(path, "anon_inode:", 11) == 0 || strncmp(path, "/memfd:", 7) == 0) {
    snprintf(path_out, MAX_PATH, "<fd>");
    *write_access = write;
    return 1;
  }
  snprintf(path_out, MAX_PATH, "%s", path);
  *write_access = write;
  /* bwrap writes the user-namespace uid/gid maps under /proc before it execs
   * the target. Nothing the target runs may write under /proc. */
  if (write && g_current_pid > 0 && is_bwrap(g_current_pid) && starts_with_seg(path, "/proc")) return 1;
  return path_allowed(path, write) ? 1 : 0;
}

static void respond(int fd, const struct seccomp_notif *req, int allowed) {
  struct seccomp_notif_resp resp;
  memset(&resp, 0, sizeof(resp));
  resp.id = req->id;
  if (allowed) {
    resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
  } else {
    resp.error = -EPERM;
  }
  (void)ioctl(fd, SECCOMP_IOCTL_NOTIF_SEND, &resp);
}

static pid_t g_child = -1;
static int g_child_done;
static int g_child_status;

static void reply_loop(int fd) {
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN;
  while (!g_stop) {
    int status;
    pid_t w = waitpid(g_child, &status, WNOHANG);
    if (w == g_child) {
      g_child_done = 1;
      g_child_status = status;
      return;
    }
    int pr = poll(&pfd, 1, 200);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) continue;
    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
      /* Child is gone; the next waitpid check settles the status. */
      continue;
    }
    if (pfd.revents & POLLIN) {
      struct seccomp_notif req;
      memset(&req, 0, sizeof(req));
      if (ioctl(fd, SECCOMP_IOCTL_NOTIF_RECV, &req) != 0) {
        if (errno == EINTR || errno == ENOENT) continue;
        break;
      }
      if (ioctl(fd, SECCOMP_IOCTL_NOTIF_ID_VALID, &req.id) != 0) continue;
      int write_access = 0;
      char path[MAX_PATH];
      int allowed = check_notification((pid_t)req.pid, &req, &write_access, path);
      if (!allowed) {
        char syscall_buf[64];
        snprintf(syscall_buf, sizeof(syscall_buf), "%s", syscall_name((long)req.data.nr));
        audit_record((pid_t)req.pid, syscall_buf, path, write_access);
        char debug_exe[512]; char debug_proc[64];
        snprintf(debug_proc, sizeof(debug_proc), "/proc/%d/exe", (int)req.pid);
        ssize_t debug_n = readlink(debug_proc, debug_exe, sizeof(debug_exe) - 1);
        if (debug_n > 0) debug_exe[debug_n] = '\0'; else debug_exe[0] = '\0';
        fprintf(stderr, "[eval-guard] blocked %s on %s (pid %d exe %s); killing evaluation process group\n", syscall_buf, path, (int)req.pid, debug_exe);
        fflush(stderr);
        if (g_child > 0) kill(-g_child, SIGKILL);
        g_stop = 1;
      }
      respond(fd, &req, allowed);
      if (!allowed) break;
    }
  }
}

static void parse_policy(const char *file) {
  FILE *fp = fopen(file, "r");
  char line[MAX_PATH + 64];
  if (fp == NULL) die("cannot open policy");
  while (fgets(line, sizeof(line), fp) != NULL) {
    char *p = line;
    char *kind;
    char *value;
    size_t len;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '\n' || *p == '\0') continue;
    len = strlen(p);
    while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) p[--len] = '\0';
    kind = p;
    value = strchr(p, ' ');
    if (value == NULL) continue;
    *value++ = '\0';
    while (*value == ' ' || *value == '\t') value++;
    if (strcmp(kind, "workspace") == 0) {
      snprintf(g_workspace, sizeof(g_workspace), "%s", value);
    } else if (strcmp(kind, "read") == 0) {
      if (g_read_count < MAX_RULES) snprintf(g_read[g_read_count++], MAX_PATH, "%s", value);
    } else if (strcmp(kind, "exact") == 0) {
      if (g_exact_count < MAX_RULES) snprintf(g_exact[g_exact_count++], MAX_PATH, "%s", value);
    } else if (strcmp(kind, "dev") == 0) {
      if (g_dev_count < MAX_RULES) snprintf(g_dev[g_dev_count++], MAX_PATH, "%s", value);
    } else if (strcmp(kind, "tmpbind") == 0) {
      if (strcmp(value, g_workspace) != 0) g_tmpbind = 1;
    } else if (strcmp(kind, "rule") == 0) {
      char *sub = value;
      char *arg = strchr(sub, ' ');
      if (arg == NULL) continue;
      *arg++ = '\0';
      while (*arg == ' ' || *arg == '\t') arg++;
      if (g_rule_count < MAX_RULES) {
        snprintf(g_rules[g_rule_count].path, MAX_PATH, "%s", arg);
        g_rules[g_rule_count].allow = (strcmp(sub, "allow") == 0);
        g_rule_count++;
      }
    }
  }
  fclose(fp);
  if (g_workspace[0] == '\0') die("policy has no workspace");
}

int main(int argc, char **argv) {
  const char *policy = NULL;
  const char *audit = NULL;
  int i;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--") == 0) { i++; break; }
    if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc) { policy = argv[++i]; continue; }
    if (strcmp(argv[i], "--audit") == 0 && i + 1 < argc) { audit = argv[++i]; continue; }
    if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) { i++; continue; }
    fprintf(stderr, "dsh-eval-supervisor: unknown argument %s\n", argv[i]);
    return 125;
  }
  if (policy == NULL || i >= argc) {
    fprintf(stderr, "dsh-eval-supervisor: usage: --policy <file> --audit <file> -- <program> [args...]\n");
    return 125;
  }
  parse_policy(policy);
  /* Keep bwrap's own temporary files inside the workspace; the target gets its
   * own TMPDIR/HOME through bwrap --setenv. */
  {
    char tmpdir[MAX_PATH];
    char homedir[MAX_PATH];
    snprintf(tmpdir, sizeof(tmpdir), "%s/.tmp", g_workspace);
    snprintf(homedir, sizeof(homedir), "%s/.home", g_workspace);
    setenv("TMPDIR", tmpdir, 1);
    setenv("HOME", homedir, 1);
    unsetenv("PWD");
    unsetenv("OLDPWD");
  }
  if (audit != NULL) {
    g_audit_fd = open(audit, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);

  int sp[2];
  if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sp) != 0) die("socketpair");
  pid_t pid = fork();
  if (pid < 0) die("fork");
  if (pid == 0) {
    int listener;
    struct sock_filter filter[128];
    struct sock_fprog prog;
    int n = 0;
    char fdmsg[8];
    struct msghdr msg;
    struct iovec iov;
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg;

    close(sp[0]);
    setsid();
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(125);

    /* arch gate */
    filter[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
    filter[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0);
    filter[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    filter[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
    /* One comparison per intercepted syscall; long but far below the 4096 limit. */
#define ADD_SYSCALL(nr) do { \
      filter[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1); \
      filter[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF); \
    } while (0)
    ADD_SYSCALL(SYS_open);
    ADD_SYSCALL(SYS_openat);
    ADD_SYSCALL(SYS_openat2);
    ADD_SYSCALL(SYS_creat);
    ADD_SYSCALL(SYS_unlink);
    ADD_SYSCALL(SYS_unlinkat);
    ADD_SYSCALL(SYS_rename);
    ADD_SYSCALL(SYS_renameat);
#ifdef SYS_renameat2
    ADD_SYSCALL(SYS_renameat2);
#endif
    ADD_SYSCALL(SYS_mkdir);
    ADD_SYSCALL(SYS_mkdirat);
    ADD_SYSCALL(SYS_rmdir);
    ADD_SYSCALL(SYS_truncate);
    ADD_SYSCALL(SYS_chmod);
    ADD_SYSCALL(SYS_fchmodat);
    ADD_SYSCALL(SYS_chown);
    ADD_SYSCALL(SYS_lchown);
    ADD_SYSCALL(SYS_fchownat);
    ADD_SYSCALL(SYS_stat);
    ADD_SYSCALL(SYS_lstat);
    ADD_SYSCALL(SYS_newfstatat);
    ADD_SYSCALL(SYS_access);
    ADD_SYSCALL(SYS_faccessat);
    ADD_SYSCALL(SYS_faccessat2);
    ADD_SYSCALL(SYS_readlink);
    ADD_SYSCALL(SYS_readlinkat);
    ADD_SYSCALL(SYS_socket);
    ADD_SYSCALL(SYS_connect);
    ADD_SYSCALL(SYS_bind);
    ADD_SYSCALL(SYS_listen);
    ADD_SYSCALL(SYS_accept);
    ADD_SYSCALL(SYS_accept4);
    ADD_SYSCALL(SYS_sendto);
    ADD_SYSCALL(SYS_recvfrom);
    ADD_SYSCALL(SYS_execve);
#ifdef SYS_execveat
    ADD_SYSCALL(SYS_execveat);
#endif
#ifdef SYS_io_uring_setup
    ADD_SYSCALL(SYS_io_uring_setup);
#endif
#ifdef SYS_open_by_handle_at
    ADD_SYSCALL(SYS_open_by_handle_at);
#endif
    ADD_SYSCALL(SYS_ptrace);
#undef ADD_SYSCALL
    filter[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

    prog.len = (unsigned short)n;
    prog.filter = filter;
    listener = (int)syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
    if (listener < 0) {
      fprintf(stderr, "dsh-eval-supervisor: seccomp filter failed: %s\n", strerror(errno));
      _exit(125);
    }
    snprintf(fdmsg, sizeof(fdmsg), "%d", listener);
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = fdmsg;
    iov.iov_len = strlen(fdmsg) + 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &listener, sizeof(int));
    (void)sendmsg(sp[1], &msg, 0);
    close(sp[1]);
    /* bwrap pivots into a fresh root and would leave the target at /. Preserve
     * the cwd the consumer chose (the tool's workdir) with bwrap --chdir. */
    {
      char cwd[MAX_PATH];
      const char *use_cwd = g_workspace;
      if (getcwd(cwd, sizeof(cwd)) != NULL && starts_with_seg(cwd, g_workspace)) use_cwd = cwd;
      int sep = -1;
      for (int k = i + 1; k < argc; k++) if (strcmp(argv[k], "--") == 0) sep = k;
      if (sep < 0) sep = argc;
      char **next = (char **)calloc((size_t)(argc + 3), sizeof(char *));
      if (next == NULL) _exit(125);
      int out = 0;
      for (int k = i; k < sep; k++) next[out++] = argv[k];
      next[out++] = (char *)"--chdir";
      next[out++] = (char *)use_cwd;
      for (int k = sep; k < argc; k++) next[out++] = argv[k];
      next[out] = NULL;
      execvp(next[0], next);
    }
    fprintf(stderr, "dsh-eval-supervisor: exec failed: %s\n", strerror(errno));
    _exit(127);
  }

  close(sp[1]);
  int listener = -1;
  {
    char buf[16];
    struct msghdr msg;
    struct iovec iov;
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg;
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);
    if (recvmsg(sp[0], &msg, 0) < 0) die("recvmsg listener");
    cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == NULL || cmsg->cmsg_type != SCM_RIGHTS) die("no listener fd");
    memcpy(&listener, CMSG_DATA(cmsg), sizeof(int));
  }
  close(sp[0]);
  g_child = pid;
  reply_loop(listener);

  if (!g_stop) {
    /* The notifier stopped without a violation (child exited or listener closed). */
  }
  if (g_child > 0) {
    int status = g_child_status;
    if (!g_child_done) {
      if (waitpid(g_child, &status, 0) != g_child) status = 0;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  }
  return 1;
}
