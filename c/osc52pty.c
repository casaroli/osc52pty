// osc52pty - run a command (default: $SHELL) in a pty, strip OSC 52 clipboard
// sequences from its output and send their payload to the local clipboard.
// For terminals without OSC 52 support (e.g. Terminal.app).
//
//   osc52pty [command [args...]]
//
// The clipboard command is pbcopy, or $OSC52PTY_COPY (run by /bin/sh -c).
// Started as a login shell (argv[0] beginning with "-"), it starts $SHELL as
// a login shell too.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>

#define OSC52_MAX (16 << 20)  // give up on an unterminated sequence past this

static const char start[] = "\033]52;";
enum { START_LEN = sizeof start - 1 };

struct buf {
  char *p;
  size_t len, cap;
};

static void buf_add(struct buf *b, const void *p, size_t n) {
  if (n == 0) return;
  if (b->len + n > b->cap) {
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + n) cap *= 2;
    if (!(b->p = realloc(b->p, cap))) abort();
    b->cap = cap;
  }
  memcpy(b->p + b->len, p, n);
  b->len += n;
}

struct osc52 {
  size_t matched;  // bytes of start[] seen; START_LEN: inside a sequence
  int esc;         // inside a sequence, the last byte was ESC
  size_t max;
  struct buf payload;
  void (*on_seq)(const char *payload, size_t len);
};

static void osc52_reset(struct osc52 *p) {
  p->matched = 0;
  p->esc = 0;
  p->payload.len = 0;
}

// Feed one chunk of output: what should reach the terminal is appended to out,
// and an incomplete sequence (or what may be the start of one) is held in p
// for the next call. Each ESC ] 52 ; ... (BEL | ESC \) goes to p->on_seq.
static void osc52_feed(struct osc52 *p, const char *s, size_t n, struct buf *out) {
  const char *end = s + n;
  while (s < end) {
    if (p->matched == 0) {
      const char *e = memchr(s, '\033', end - s);
      if (!e) {
        buf_add(out, s, end - s);
        return;
      }
      buf_add(out, s, e - s);
      s = e + 1;
      p->matched = 1;
    } else if (p->matched < START_LEN) {
      if (*s == start[p->matched]) {
        s++;
        p->matched++;
      } else {  // not ours after all; *s is looked at again from scratch
        buf_add(out, start, p->matched);
        p->matched = 0;
      }
    } else if (p->esc) {
      p->esc = 0;
      if (*s == '\\') {
        s++;
        p->on_seq(p->payload.p, p->payload.len);
        osc52_reset(p);
      } else {
        buf_add(&p->payload, "\033", 1);
      }
    } else {
      const char *e = s;
      while (e < end && *e != '\a' && *e != '\033') e++;
      buf_add(&p->payload, s, e - s);
      s = e;
      if (s < end) {
        if (*s++ == '\a') {
          p->on_seq(p->payload.p, p->payload.len);
          osc52_reset(p);
          continue;
        }
        p->esc = 1;
      }
      if (START_LEN + p->payload.len + p->esc > p->max) {
        buf_add(out, start, START_LEN);
        buf_add(out, p->payload.p, p->payload.len);
        if (p->esc) buf_add(out, "\033", 1);
        osc52_reset(p);
      }
    }
  }
}

// End of output: release whatever is still held back.
static void osc52_flush(struct osc52 *p, struct buf *out) {
  buf_add(out, start, p->matched);
  buf_add(out, p->payload.p, p->payload.len);
  if (p->esc) buf_add(out, "\033", 1);
  osc52_reset(p);
}

static int b64_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Decode base64, skipping whitespace; anything else invalid decodes to nothing.
static void b64_decode(const char *s, size_t n, struct buf *out) {
  size_t mark = out->len;
  unsigned acc = 0;
  int bits = 0;
  for (size_t i = 0; i < n && s[i] != '='; i++) {
    unsigned char c = s[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    int v = b64_value(c);
    if (v < 0) {
      out->len = mark;
      return;
    }
    acc = acc << 6 | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      char byte = acc >> bits;
      buf_add(out, &byte, 1);
      acc &= (1u << bits) - 1;
    }
  }
}

static int write_all(int fd, const char *p, size_t n) {
  while (n > 0) {
    ssize_t k = write(fd, p, n);
    if (k < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    p += k;
    n -= k;
  }
  return 0;
}

static void copy_to_clipboard(const char *data, size_t len) {
  int fd[2];
  if (pipe(fd) < 0) return;
  pid_t pid = fork();
  if (pid == 0) {
    dup2(fd[0], 0);
    close(fd[0]);
    close(fd[1]);
    signal(SIGPIPE, SIG_DFL);
    const char *cmd = getenv("OSC52PTY_COPY");
    if (cmd && *cmd)
      execl("/bin/sh", "sh", "-c", cmd, (char *)0);
    else
      execl("/usr/bin/pbcopy", "pbcopy", (char *)0);
    _exit(127);
  }
  close(fd[0]);
  if (pid > 0) write_all(fd[1], data, len);
  close(fd[1]);
  if (pid > 0)
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

// OSC 52 payload is "<selection>;<base64>"; "?" is a read request, ignore it.
static void clip(const char *s, size_t n) {
  const char *semi = memchr(s, ';', n);
  if (semi) {
    n -= semi + 1 - s;
    s = semi + 1;
  }
  if (n == 1 && *s == '?') return;
  struct buf data = {0};
  b64_decode(s, n, &data);
  copy_to_clipboard(data.p, data.len);
  free(data.p);
}

static int sigfd[2];
static struct termios saved_tty;
static int tty_saved;

static void on_signal(int sig) {
  int e = errno;
  char c = sig;
  if (write(sigfd[1], &c, 1) < 0) {}
  errno = e;
}

static void restore_tty(void) {
  if (tty_saved) tcsetattr(0, TCSANOW, &saved_tty);
}

static void cloexec(int fd) {
  fcntl(fd, F_SETFD, FD_CLOEXEC);
}

int main(int argc, char **argv) {
  char *shell = getenv("SHELL");
  if (!shell || !*shell) shell = "/bin/sh";
  char *login[2] = {0};
  char **cmd = argv + 1;
  if (argc < 2) {
    const char *name = strrchr(shell, '/');
    name = name ? name + 1 : shell;
    if (asprintf(&login[0], "%s%s", argv[0][0] == '-' ? "-" : "", name) < 0) return 1;
    cmd = login;
  }
  const char *path = argc < 2 ? shell : cmd[0];

  struct winsize ws;
  int tty = tcgetattr(0, &saved_tty) == 0 && ioctl(0, TIOCGWINSZ, &ws) == 0;
  setenv("OSC52PTY", "1", 1);  // lets a .zshrc that execs us tell it's inside

  int master;
  pid_t child = forkpty(&master, NULL, tty ? &saved_tty : NULL, tty ? &ws : NULL);
  if (child < 0) {
    perror("osc52pty: forkpty");
    return 1;
  }
  if (child == 0) {
    execvp(path, cmd);
    fprintf(stderr, "osc52pty: %s: %s\n", path, strerror(errno));
    _exit(127);
  }

  if (pipe(sigfd) < 0) return 1;
  cloexec(master);
  cloexec(sigfd[0]);
  cloexec(sigfd[1]);
  fcntl(sigfd[1], F_SETFL, O_NONBLOCK);
  fcntl(master, F_SETFL, O_NONBLOCK);
  struct sigaction sa = {.sa_handler = on_signal, .sa_flags = SA_RESTART};
  sigemptyset(&sa.sa_mask);
  int sigs[] = {SIGWINCH, SIGCHLD, SIGTERM, SIGHUP};
  for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
  signal(SIGPIPE, SIG_IGN);

  if (tty) {
    struct termios raw = saved_tty;
    cfmakeraw(&raw);
    // not TCSAFLUSH: that waits for output to drain and drops typeahead
    tcsetattr(0, TCSANOW, &raw);
    tty_saved = 1;
    atexit(restore_tty);
  }

  struct osc52 p = {.max = OSC52_MAX, .on_seq = clip};
  struct buf in = {0}, out = {0};  // in: keystrokes the pty hasn't taken yet
  size_t in_off = 0;
  static char chunk[65536];
  int status = 0, child_done = 0, stdin_open = 1, quit = 0, hup = 0;

  while (!quit) {
    struct pollfd fds[3] = {
        {.fd = master, .events = POLLIN | (in_off < in.len ? POLLOUT : 0)},
        {.fd = sigfd[0], .events = POLLIN},
        {.fd = stdin_open && in_off == in.len ? 0 : -1, .events = POLLIN},
    };
    if (poll(fds, 3, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (fds[1].revents & POLLIN) {
      char got[64];
      ssize_t k = read(sigfd[0], got, sizeof got);
      for (ssize_t i = 0; i < k; i++) {
        if (got[i] == SIGWINCH && ioctl(0, TIOCGWINSZ, &ws) == 0) ioctl(master, TIOCSWINSZ, &ws);
        if (got[i] == SIGTERM || got[i] == SIGHUP) quit = hup = 1;
      }
      if (!child_done && waitpid(child, &status, WNOHANG) == child) child_done = 1;
    }

    if (fds[2].revents & (POLLIN | POLLHUP)) {
      ssize_t k = read(0, chunk, sizeof chunk);
      if (k <= 0) {
        stdin_open = 0;
      } else {
        in.len = in_off = 0;
        buf_add(&in, chunk, k);
      }
    }
    if (in_off < in.len) {
      ssize_t k = write(master, in.p + in_off, in.len - in_off);
      if (k > 0) in_off += k;
    }

    // after the child is gone, take what output is left and stop
    for (int draining = child_done; !quit;) {
      if (!draining && !(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) break;
      ssize_t k = read(master, chunk, sizeof chunk);
      if (k < 0 && errno == EINTR) continue;
      if (k <= 0) {
        if (k < 0 && errno == EAGAIN && !draining) break;
        quit = 1;
        break;
      }
      osc52_feed(&p, chunk, k, &out);
      if (write_all(1, out.p, out.len) < 0) quit = 1;
      out.len = 0;
      if (!draining) break;
    }
  }

  osc52_flush(&p, &out);
  write_all(1, out.p, out.len);
  restore_tty();
  if (!child_done) {
    if (hup) kill(child, SIGHUP);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  }
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return WEXITSTATUS(status);
}
