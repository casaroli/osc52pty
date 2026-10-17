// Unit tests for osc52pty.c.  Run: make test

#define main osc52pty_main
#include "osc52pty.c"
#undef main

struct str {
  const char *p;
  size_t n;
};
#define S(lit) ((struct str){lit, sizeof(lit) - 1})

static int pass, fail;
static struct buf clips;  // payloads joined by "|"
static int nclips;

static void record(const char *s, size_t n) {
  if (nclips++) buf_add(&clips, "|", 1);
  buf_add(&clips, s, n);
}

static void show(const char *p, size_t n) {
  putchar('"');
  for (size_t i = 0; i < n; i++) {
    unsigned char c = p[i];
    if (c == '\033') printf("\\e");
    else if (c == '\a') printf("\\a");
    else if (c == '\\') printf("\\\\");
    else if (c < 32 || c >= 127) printf("\\x%02x", c);
    else putchar(c);
  }
  printf("\"\n");
}

static void check(const char *name, struct str exp, const char *got, size_t n) {
  if (exp.n == n && memcmp(exp.p, got, n) == 0) {
    pass++;
    return;
  }
  fail++;
  printf("FAIL %s\n  expected: ", name);
  show(exp.p, exp.n);
  printf("  actual:   ");
  show(got, n);
}

static void check_int(const char *name, long exp, long got) {
  char e[32], g[32];
  check(name, (struct str){e, snprintf(e, sizeof e, "%ld", exp)}, g, snprintf(g, sizeof g, "%ld", got));
}

static struct osc52 p = {.max = OSC52_MAX, .on_seq = record};
static struct buf got;  // output of each feed call joined by "|"

// feed each chunk; $got, $clips and what p still holds are left to look at
static void feed(const struct str *chunks, size_t n) {
  osc52_reset(&p);
  got.len = clips.len = nclips = 0;
  struct buf out = {0};
  for (size_t i = 0; i < n; i++) {
    out.len = 0;
    osc52_feed(&p, chunks[i].p, chunks[i].n, &out);
    if (i) buf_add(&got, "|", 1);
    buf_add(&got, out.p, out.len);
  }
  free(out.p);
}

static void held(struct buf *b) {
  b->len = 0;
  struct osc52 copy = p;
  osc52_flush(&copy, b);
}

static void t(const char *name, struct str exp_out, struct str exp_clips, const struct str *chunks, size_t n) {
  char label[256];
  struct buf h = {0};
  feed(chunks, n);
  snprintf(label, sizeof label, "%s: output", name);
  check(label, exp_out, got.p, got.len);
  snprintf(label, sizeof label, "%s: clips", name);
  check(label, exp_clips, clips.p, clips.len);
  held(&h);
  snprintf(label, sizeof label, "%s: nothing held", name);
  check(label, S(""), h.p, h.len);

  // the same bytes one at a time must come out the same, minus the "|"s
  struct buf all = {0}, want = {0};
  for (size_t i = 0; i < n; i++) buf_add(&all, chunks[i].p, chunks[i].n);
  for (size_t i = 0; i < got.len; i++)
    if (got.p[i] != '|') buf_add(&want, &got.p[i], 1);
  struct buf want_clips = {0};
  buf_add(&want_clips, clips.p, clips.len);
  struct str *bytes = malloc((all.len + 1) * sizeof *bytes);
  for (size_t i = 0; i < all.len; i++) bytes[i] = (struct str){all.p + i, 1};
  feed(bytes, all.len);
  struct buf joined = {0};
  for (size_t i = 0; i < got.len; i++)
    if (got.p[i] != '|') buf_add(&joined, &got.p[i], 1);
  snprintf(label, sizeof label, "%s: byte by byte output", name);
  check(label, (struct str){want.p, want.len}, joined.p, joined.len);
  snprintf(label, sizeof label, "%s: byte by byte clips", name);
  check(label, (struct str){want_clips.p, want_clips.len}, clips.p, clips.len);
  free(bytes), free(all.p), free(want.p), free(want_clips.p), free(joined.p), free(h.p);
}

#define CHUNKS(...) (const struct str[]){__VA_ARGS__}, sizeof((struct str[]){__VA_ARGS__}) / sizeof(struct str)
#define T(name, out, clips, ...) t(name, out, clips, CHUNKS(__VA_ARGS__))

static char tmpdir[] = "/tmp/osc52pty-test.XXXXXX";
static char clipfile[64];

static void clip_to_file(struct str payload, struct buf *result) {
  unlink(clipfile);
  clip(payload.p, payload.n);
  result->len = 0;
  FILE *f = fopen(clipfile, "r");
  if (!f) return;
  char tmp[4096];
  size_t k;
  while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) buf_add(result, tmp, k);
  fclose(f);
}

int main(void) {
  struct buf h = {0};
  setvbuf(stdout, NULL, _IOLBF, 0);
  signal(SIGPIPE, SIG_IGN);  // as osc52pty's main does

  printf("== osc52_feed\n");
  T("plain text", S("hello world"), S(""), S("hello world"));
  T("empty chunk", S(""), S(""), S(""));
  T("BEL terminated", S("AB"), S("c;aGk="), S("A\033]52;c;aGk=\aB"));
  T("ST terminated", S("AB"), S("c;aGk="), S("A\033]52;c;aGk=\033\\B"));
  T("two in one chunk", S("ABC"), S("c;MQ==|p;Mg=="), S("A\033]52;c;MQ==\aB\033]52;p;Mg==\033\\C"));
  T("back to back", S(""), S("c;MQ==|c;Mg=="), S("\033]52;c;MQ==\a\033]52;c;Mg==\a"));
  T("BEL before ST", S("B\033\\"), S("c;QQ=="), S("\033]52;c;QQ==\aB\033\\"));
  T("ST before BEL", S("B\a"), S("c;QQ=="), S("\033]52;c;QQ==\033\\B\a"));
  T("query is passed on", S(""), S("c;?"), S("\033]52;c;?\a"));
  T("ESC inside payload", S(""), S("c;a\033b"), S("\033]52;c;a\033b\a"));

  T("split after ESC", S("X|||Y"), S("c;aGk="), S("X\033"), S("]5"), S("2;c;aGk="), S("\aY"));
  T("split byte by byte", S("X|||||||||||||Y"), S("c;aGk="), S("X"), S("\033"), S("]"), S("5"), S("2"),
    S(";"), S("c"), S(";"), S("a"), S("G"), S("k"), S("="), S("\a"), S("Y"));
  T("split in payload", S("|B"), S("c;aGk="), S("\033]52;c;aG"), S("k=\aB"));
  T("split inside ST", S("A|B"), S("c;aGk="), S("A\033]52;c;aGk=\033"), S("\\B"));
  T("chunk ends on start", S("A|B"), S("c;aGk="), S("A\033]52;"), S("c;aGk=\aB"));

  T("other OSC untouched", S("\033]0;title\a"), S(""), S("\033]0;title\a"));
  T("near miss \\e]5x", S("\033]5x"), S(""), S("\033]5x"));
  T("near miss \\e]520", S("a|\033]520;x"), S(""), S("a\033]52"), S("0;x"));
  T("held ESC was CSI", S("a|\033[1mb"), S(""), S("a\033"), S("[1mb"));
  T("ESC ESC ] 52", S("\033"), S("c;aGk="), S("\033\033]52;c;aGk=\a"));
  T("NUL bytes", S("a\0b"), S(""), S("a\0b"));
  T("UTF-8 split", S("\xc3|\xa9"), S(""), S("\xc3"), S("\xa9"));

  feed(CHUNKS(S("\033]52;\a")));
  check_int("empty payload: one clip", 1, nclips);
  check("empty payload: is empty", S(""), clips.p, clips.len);

  feed(CHUNKS(S("a\033]52;c;aGk=")));
  check("unterminated: output", S("a"), got.p, got.len);
  held(&h);
  check("unterminated: held", S("\033]52;c;aGk="), h.p, h.len);
  check_int("unterminated: no clip", 0, nclips);

  feed(CHUNKS(S("a\033]52;c;aGk=\033")));
  held(&h);
  check("unterminated after ESC: held", S("\033]52;c;aGk=\033"), h.p, h.len);

  feed(CHUNKS(S("a\033]5")));
  check("partial start: output", S("a"), got.p, got.len);
  held(&h);
  check("partial start: held", S("\033]5"), h.p, h.len);

  p.max = 10;
  feed(CHUNKS(S("\033]52;c;aGVsbG8gd29ybGQ="), S("x")));
  check("too long: given up", S("\033]52;c;aGVsbG8gd29ybGQ=|x"), got.p, got.len);
  held(&h);
  check("too long: nothing held", S(""), h.p, h.len);
  p.max = OSC52_MAX;

  printf("== b64_decode\n");
  struct {
    const char *name;
    struct str in, out;
  } b64[] = {
      {"hello", S("aGVsbG8="), S("hello")},
      {"no padding", S("aGVsbG8"), S("hello")},
      {"empty", S(""), S("")},
      {"whitespace", S("aGVs\nbG8=\n"), S("hello")},
      {"binary", S("AP8A/w=="), S("\0\xff\0\xff")},
      {"UTF-8", S("d8O2cmxkIOKckw=="), S("w\xc3\xb6rld \xe2\x9c\x93")},
      {"invalid", S("!!!"), S("")},
      {"invalid at end", S("aGk=!"), S("hi")},
      {"invalid in middle", S("aG!k"), S("")},
  };
  for (size_t i = 0; i < sizeof b64 / sizeof *b64; i++) {
    struct buf out = {0};
    b64_decode(b64[i].in.p, b64[i].in.n, &out);
    check(b64[i].name, b64[i].out, out.p, out.len);
    free(out.p);
  }

  printf("== clip\n");
  if (!mkdtemp(tmpdir)) return perror("mkdtemp"), 1;
  snprintf(clipfile, sizeof clipfile, "%s/clip", tmpdir);
  char cmd[256];
  snprintf(cmd, sizeof cmd, "printf '<%%s>' \"$(cat)\" > %s", clipfile);
  setenv("OSC52PTY_COPY", cmd, 1);
  struct buf r = {0};
  clip_to_file(S("c;aGVsbG8="), &r), check("decodes", S("<hello>"), r.p, r.len);
  clip_to_file(S("p;aGk="), &r), check("primary selection", S("<hi>"), r.p, r.len);
  clip_to_file(S("cp;aGk="), &r), check("multi selection", S("<hi>"), r.p, r.len);
  clip_to_file(S(";aGk="), &r), check("empty selection", S("<hi>"), r.p, r.len);
  clip_to_file(S("aGk="), &r), check("no selection", S("<hi>"), r.p, r.len);
  clip_to_file(S("c;?"), &r), check("query not copied", S(""), r.p, r.len);
  clip_to_file(S("c;"), &r), check("empty clears", S("<>"), r.p, r.len);
  clip_to_file(S("c;!!!"), &r), check("bad base64 is quiet", S("<>"), r.p, r.len);
  setenv("OSC52PTY_COPY", "exit 0", 1);
  struct buf big = {0};
  for (int i = 0; i < 200000; i++) buf_add(&big, "QUFB", 4);
  clip(big.p, big.len);  // 600KB into a command that reads none of it
  pass++;                // ...and we didn't hang
  unlink(clipfile);
  rmdir(tmpdir);

  printf("== streams\n");
  srandom(52);
  struct buf rnd = {0}, out = {0};
  for (int i = 0; i < 3000000; i++) {
    char c = random();
    buf_add(&rnd, &c, 1);
  }
  osc52_reset(&p);
  nclips = clips.len = 0;
  for (size_t off = 0; off < rnd.len;) {  // random chunk sizes, like real reads
    size_t k = 1 + random() % 9000;
    if (k > rnd.len - off) k = rnd.len - off;
    osc52_feed(&p, rnd.p + off, k, &out);
    off += k;
  }
  osc52_flush(&p, &out);
  if (nclips == 0)  // 1 in ~300000 that random data has a real sequence
    check_int("3MB random bytes pass unchanged", 1, out.len == rnd.len && !memcmp(out.p, rnd.p, out.len));

  struct buf mixed = {0};
  for (size_t i = 0; i < 100000; i++)
    if (rnd.p[i] != '\033') buf_add(&mixed, &rnd.p[i], 1);
  buf_add(&mixed, "\033]52;c;aGk=\a", 12);
  for (size_t i = 0; i < 100000; i++)
    if (rnd.p[i] != '\033') buf_add(&mixed, &rnd.p[i], 1);
  osc52_reset(&p);
  out.len = nclips = clips.len = 0;
  for (size_t off = 0; off < mixed.len;) {
    size_t k = 1 + random() % 9000;
    if (k > mixed.len - off) k = mixed.len - off;
    osc52_feed(&p, mixed.p + off, k, &out);
    off += k;
  }
  check("sequence in 200KB of noise: clip", S("c;aGk="), clips.p, clips.len);
  check_int("sequence in 200KB of noise: size", mixed.len - 12, out.len);

  printf("\n%d passed, %d failed\n", pass, fail);
  return fail != 0;
}
