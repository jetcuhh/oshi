/*** includes ***/

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)
#define ALT_KEY(k) (2000 + (k)) /* an Alt-key reads back as this */
#ifndef OSHI_VERSION
#define OSHI_VERSION "1.0.0"
#endif
#define OSHI_TAB_STOP 8
#define OSHI_CLICK_MS 400   /* a second click lands within this to count */
#define OSHI_CLICK_SLOP 2   /* cells the clicks may drift and still count */
#define OSHI_UNDO_MAX 1000  /* undo steps kept (each one is a small diff) */
#define OSHI_OSC52_MAX 74994 /* biggest copy we hand the terminal clipboard */
#define OSHI_STATUS_MS 1500 /* how long a bar message lives, in milliseconds */
#define OSHI_PASTE_MAX (64 * 1024 * 1024)
#define OSHI_PROMPT_MAX 200
#define OSHI_CLIP_TIMEOUT 1   /* seconds a clipboard tool may run before we try the next */
#define OSHI_CLIP_SETTLE_US 20000 /* ms to let a clipboard daemon catch up with an OSC 52 push */

enum editorKey {
  ESC_AGAIN = -1,
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN,
  CTRL_LEFT,
  CTRL_RIGHT,
  SHIFT_UP,
  SHIFT_DOWN,
  SHIFT_LEFT,
  SHIFT_RIGHT,
  CSHIFT_LEFT,  /* ctrl+shift: the far end jumps whole words */
  CSHIFT_RIGHT,
  CTRL_BACKSPACE, /* ctrl-backspace, sent as ^H */
  MOUSE_KEY,
  UTF8_KEY,   /* a whole multi-byte character, bytes in E.utf8 */
  PASTE_KEY,  /* the terminal is about to send a bracketed paste */
  RESIZE_KEY, /* the window changed size */
  NOKEY       /* unknown: swallowed, does nothing */
};

/* The commands the config may move to a different key. */
enum editorCommand {
  CMD_NONE = -1,
  CMD_SAVE,
  CMD_QUIT,
  CMD_FIND,
  CMD_SELECT,
  CMD_COPY,
  CMD_CUT,
  CMD_PASTE,
  CMD_KILL,
  CMD_UNDO,
  CMD_REDO,
  CMD_REPLACE,
  CMD_FIND_NEXT,
  CMD_FIND_PREV,
  CMD_GOTO,
  CMD_HELP,
  CMD_DELETE_WORD,
  CMD_COUNT
};

/*** colors ***/

/* configurable color roles, each one ANSI escape */
enum editorColor {
  COL_STATUS_BG,       /* status bar background                  */
  COL_STATUS_FG,       /* status bar foreground                */
  COL_GUTTER_FG,       /* line-number gutter, non-current line */
  COL_GUTTER_CURSOR_FG,/* line-number gutter, current line     */
  COL_GUTTER_BG,        /* background behind the gutter, non-current line */
  COL_GUTTER_CURSOR_BG, /* background behind the gutter, current line     */
  COL_TILDE_FG,        /* the ~ fringe below the buffer        */
  COL_TEXT_FG,         /* buffer text foreground             */
  COL_SCROLLMARK_BG,   /* the < / > "more this way" marks    */
  COL_SCROLLMARK_FG,
  COL_FIND,             /* yellow background behind find hits  */
  COL_COUNT
};

static int colIsFg[COL_COUNT] = { 0, 1, 1, 1, 0, 0, 1, 1, 0, 1, 0 };

/*** data ***/

typedef struct erow {
  int size;
  char *chars;
} erow;

struct editorConfig {
  int cx, cy;      /* cursor: byte offset in the row, and the row */
  int rx;          /* cursor: display column in the row */
  int goal_rx;     /* the column up/down try to keep */
  int rowoff;
  int coloff;
  int sel; /* a live selection: anchor -> cursor */
  int sel_cx;
  int sel_cy;
  int mouse_button;  /* SGR button code of the last mouse event */
  int mouse_release; /* that event was a release */
  int mouse_x;
  int mouse_y;
  int mouse_click_n;   /* clicks in a row: 1, 2 for a double, 3 for a triple */
  int mouse_click_x;
  int mouse_click_y;
  long mouse_click_ms; /* when the last one landed */
  int screenrows;
  int screencols;
  int numrows;
  int rowcap;
  erow *row;
  int dirty;
  int crlf;         /* the file used CRLF line endings: save them back */
  int redraw;       /* clear the whole screen on the next refresh */
  int ruler;
  int ruler_width;
  int gutter;
  int cursor_style; /* DECSCUSR code (2 = block, 6 = beam) */
  int binds[CMD_COUNT]; /* which key runs each command */
  char col_str[COL_COUNT][32]; /* cached ANSI escapes per color role */
  char *filename;
  char statusmsg[256];
  long statusmsg_time; /* ms on the monotonic clock, when the message was set */
  int notice_pending;  /* 1 while the bar shows a notice that lapses */
  long notice_deadline; /* ms: when that notice comes off the bar */
  char config_err[160]; /* shown on the bar until the first key or a timeout */
  long config_err_time; /* ms: when the config error was set */
  int prompt_col;   /* where the caret sits on the bar while prompting */
  int in_prompt; /* 1 while editorPrompt is open: caret on the bar */
  int quit_confirm; /* 1 after the first quit key on a dirty buffer */
  long quit_time;   /* ms: when that quit warning went up */
  char utf8[8];     /* the bytes behind the last UTF8_KEY */
  int utf8len;
  char *clip;       /* oshi's own copy of the last copy/cut */
  int clip_len;
  int clip_line;    /* whole-line copy: paste goes above */
  int clip_shared;  /* 1 if the system clipboard matched E.clip at copy time */
  struct termios orig_termios;
};

struct editorConfig E;

static volatile sig_atomic_t got_winch;
static volatile sig_atomic_t got_term;
static int raw_enabled;

/* commands with config name and help text */
struct editorCmd { const char *name; const char *desc; };
struct editorCmd editorCmds[CMD_COUNT] = {
  { "save", "save the file (root via sudo/doas if refused)" },
  { "quit", "quit (one warning on the bar if modified, then quits)" },
  { "find", "find in the file" },
  { "select", "select everything" },
  { "copy", "copy selection, else the line" },
  { "cut", "cut selection, else the line" },
  { "paste", "paste what oshi last copied" },
  { "delete-line", "delete the line" },
  { "undo", "undo a change" },
  { "redo", "redo it" },
  { "replace", "find and replace" },
  { "find-next", "jump to the next match" },
  { "find-prev", "jump to the previous match" },
  { "goto", "go to a line (N, +N, -N or N:col)" },
  { "help", "show this help screen (ctrl-e; ctrl-h is ctrl-backspace)" },
  { "delete-word", "delete the word behind the cursor (ctrl-backspace)" }
};

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen(void);
char *editorPrompt(char *prompt, void (*callback)(char *, int),
                   void (*decorate)(char *));
int editorCommandForKey(int c);
void editorKeyName(int key, char *buf, size_t len);
char *editorIstrstr(const char *hay, const char *needle);
int editorFindCount(int *cur);
int editorFindNextMatch(int dir);
void editorHelp(void);
int editorSaveElevated(const char *buf, int len);
void editorColorSet(int role, int code);
void editorColorSetRGB(int role, int r, int g, int b);
void editorApplyDefaultColors(void);
int editorParseColorRole(const char *name);
void editorSyncGoal(void);
void editorScroll(void);
long editorNowMs(void);
char *editorRowsToString(int *buflen);
void editorDeleteWordBackward(void);
static char *editorClipboardRead(int *len);
static int editorInPath(const char *name);
static int prompt_empty_ok;
void editorLeave(void);

/*** terminal ***/

/* write a string; async-signal-safe */
static void editorWrite(const char *s) {
  size_t len = strlen(s);
  ssize_t r = write(STDOUT_FILENO, s, len);
  (void)r;
}

/* use the alt screen so frames stay out of scrollback */
void editorUseAltScreen(void) {
  editorWrite("\x1b[?1049h");
}

/* mouse reports, I-beam pointer, bracketed paste, no auto-wrap */
void editorTermSetup(void) {
  editorWrite("\x1b[?1002h\x1b[?1006h"
              "\x1b]22;text\x1b\\"
              "\x1b[?2004h"
              "\x1b[?7l");
}

/* restore the shell's screen, cursor, pointer and wrapping; signal-safe */
void editorTermReset(void) {
  editorWrite("\x1b[?2004l\x1b[?7h"
              "\x1b]22;default\x1b\\"
              "\x1b[?1006l\x1b[?1002l"
              "\x1b[?1049l\x1b[?25h\x1b[0 q");
}

/* set cursor shape: 2 steady block, 6 steady bar */
void editorSetCursorStyle(void) {
  char seq[16];
  int len = snprintf(seq, sizeof(seq), "\x1b[%d q", E.cursor_style);
  ssize_t r = write(STDOUT_FILENO, seq, len);
  (void)r;
}

void disableRawMode(void) {
  if (raw_enabled) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
    raw_enabled = 0;
  }
}

void die(const char *s) {
  int e = errno;
  editorTermReset();
  disableRawMode(); /* so perror's newline gets its carriage return back */
  errno = e;
  perror(s);
  exit(1);
}

void editorQuit(void) {
  editorTermReset();
  exit(0);
}

void enableRawMode(void) {
  static int registered;
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  if (!registered) { atexit(disableRawMode); registered = 1; }

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
  raw_enabled = 1;
}

/*** signals ***/

static void editorOnWinch(int sig) { (void)sig; got_winch = 1; }
static void editorOnTerm(int sig) { got_term = sig; }

/* crash: restore the terminal; async-signal-safe calls only */
static void editorOnCrash(int sig) {
  editorTermReset();
  if (raw_enabled) tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
  signal(sig, SIG_DFL);
  raise(sig);
}

/* no SA_RESTART so resize and quit interrupt read() */
static void editorInstallSignals(void) {
  struct sigaction sa;
  int i;
  static const int term_sigs[] = { SIGTERM, SIGHUP, SIGINT, SIGQUIT };
  static const int crash_sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };

  memset(&sa, 0, sizeof(sa));
  sigemptyset(&sa.sa_mask);

  sa.sa_handler = editorOnWinch;
  sigaction(SIGWINCH, &sa, NULL);

  /* ignore SIGPIPE so a closed popen() pipe fails the write instead */
  signal(SIGPIPE, SIG_IGN);

  sa.sa_handler = editorOnTerm;
  for (i = 0; i < (int)(sizeof(term_sigs) / sizeof(term_sigs[0])); i++)
    sigaction(term_sigs[i], &sa, NULL);

  sa.sa_handler = editorOnCrash;
  sa.sa_flags = SA_RESETHAND | SA_NODEFER;
  for (i = 0; i < (int)(sizeof(crash_sigs) / sizeof(crash_sigs[0])); i++)
    sigaction(crash_sigs[i], &sa, NULL);
}

/* The window changed: read the new size. */
static int editorResize(void) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0 ||
      ws.ws_row == 0)
    return 0;
  E.screencols = ws.ws_col;
  E.screenrows = ws.ws_row - 1; /* the status bar takes one */
  if (E.screenrows < 1) E.screenrows = 1;
  E.redraw = 1;
  return 1;
}

/*** input: keys ***/

/* read the rest of an escape sequence; unknown ones give NOKEY */
int editorReadEscape(void) {
  char seq[32];
  int len = 1;

  if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b'; /* lone ESC */
  if (seq[0] == '\x1b') return ESC_AGAIN; /* ESC glued to the next key's ESC */
  if (seq[0] != '[' && seq[0] != 'O')
    return ALT_KEY((unsigned char)seq[0]); /* ESC + Alt-key */
  while (len < (int)sizeof(seq) - 1) {
    if (read(STDIN_FILENO, &seq[len], 1) != 1) break;
    len++;
    if (seq[len - 1] >= 0x40 && seq[len - 1] <= 0x7e) break; /* final byte */
  }
  if (len == 1) return ALT_KEY((unsigned char)seq[0]); /* Alt-[ or Alt-O */
  seq[len] = '\0';

  char final = seq[len - 1];
  if (seq[0] == '[' && final == 'M' && len == 2) { /* X10 mouse report */
    /* X10 mouse report: swallow the three bytes, not decoded */
    unsigned char m[3];
    int got = 0, idle = 0;
    while (got < 3) {
      ssize_t r = read(STDIN_FILENO, m + got, sizeof(m) - (size_t)got);
      if (r > 0) {
        got += (int)r;
        idle = 0;
        continue;
      }
      if (r == 0 && ++idle <= 10) continue; /* a beat between the bytes */
      if (r == -1 && errno == EINTR && !got_term) continue;
      break;
    }
    return NOKEY;
  }
  if (len > 2 && seq[0] == '[' && seq[1] == '<' &&
      sscanf(&seq[2], "%d;%d;%d", &E.mouse_button, &E.mouse_x, &E.mouse_y) == 3) {
    E.mouse_release = final == 'm';
    return MOUSE_KEY;
  }
  switch (final) {
  case 'A': case 'B': case 'C': case 'D': {
    /* xterm [1;N: N-1 is the modifier mask (bit0 shift, bit2 ctrl) */
    int m = (len > 4 && seq[1] == '1' && seq[2] == ';') ? atoi(&seq[3]) - 1 : 0;
    int shift = m & 1, ctrl = m & 4;
    if (final == 'A') return shift ? SHIFT_UP : ARROW_UP;
    if (final == 'B') return shift ? SHIFT_DOWN : ARROW_DOWN;
    if (ctrl) return final == 'C'
                    ? (shift ? CSHIFT_RIGHT : CTRL_RIGHT)
                    : (shift ? CSHIFT_LEFT : CTRL_LEFT);
    if (shift) return final == 'C' ? SHIFT_RIGHT : SHIFT_LEFT;
    return final == 'C' ? ARROW_RIGHT : ARROW_LEFT;
  }
  case 'H': return HOME_KEY;
  case 'F': return END_KEY;
  case '~':
    switch (atoi(&seq[1])) { /* the number in front of the '~' */
    case 1: case 7: return HOME_KEY;
    case 3: return DEL_KEY;
    case 4: case 8: return END_KEY;
    case 5: return PAGE_UP;
    case 6: return PAGE_DOWN;
    case 200: return PASTE_KEY;
    }
    break;
  }
  return NOKEY;
}

int editorReadKey(void) {
  int nread;
  char c;
  static int instant_reads;
  for (;;) {
    if (got_term) { /* terminated: restore the terminal */
      editorTermReset();
      exit(128 + (int)got_term);
    }
    if (got_winch) {
      got_winch = 0;
      if (editorResize()) return RESIZE_KEY;
    }
    long t0 = editorNowMs();
    nread = read(STDIN_FILENO, &c, 1);
    if (nread == 1) {
      instant_reads = 0;
      break;
    }
    if (nread == 0 && editorNowMs() - t0 < 20) {
      /* an instant read returning again and again means the terminal is gone */
      if (++instant_reads > 200) {
        editorTermReset();
        exit(1);
      }
    } else {
      instant_reads = 0;
    }
    if (nread == -1 && errno != EAGAIN && errno != EINTR) die("read");
    /* use the idle read to lapse bar notices on time */
    if (E.notice_pending && editorNowMs() >= E.notice_deadline) {
      E.notice_pending = 0; /* repaint once, then it is gone */
      return NOKEY; /* nothing to act on: just a redraw */
    }
  }
  if (c == '\x1b') {
    int key = editorReadEscape();
    while (key == ESC_AGAIN) key = editorReadEscape();
    return key;
  }
  unsigned char u = (unsigned char)c;
  if (u == 0x08) return CTRL_BACKSPACE; /* what a terminal sends for ctrl-backspace */
  if (u < 0x80) return u;
  if (u >= 0xC2 && u <= 0xF4) {
    /* multi-byte lead: take the whole character */
    int need = u >= 0xF0 ? 3 : u >= 0xE0 ? 2 : 1;
    int total = need + 1;
    E.utf8[0] = c;
    E.utf8len = 1;
    while (need-- > 0) {
      char cc;
      if (read(STDIN_FILENO, &cc, 1) != 1) break;
      if (((unsigned char)cc & 0xC0) != 0x80) return NOKEY;
      E.utf8[E.utf8len++] = cc;
    }
    return E.utf8len == total ? UTF8_KEY : NOKEY;
  }
  return NOKEY; /* a stray continuation byte, or not UTF-8 at all */
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** memory ***/

/* out of memory is fatal */
void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (p == NULL) die("malloc");
  return p;
}

void *xrealloc(void *p, size_t n) {
  void *np = realloc(p, n ? n : 1);
  if (np == NULL) die("realloc");
  return np;
}

/* read a bracketed paste up to its terminator; never NULL */
static char *editorReadPasteBuf(int *len);
static void editorSkipPaste(void) {
  int n;
  free(editorReadPasteBuf(&n));
}

/* find endseq in buf[0..n) from `from`; returns n if absent */
static size_t editorFindPasteEnd(const char *buf, size_t n, size_t from) {
  static const char endseq[] = "\x1b[201~";
  const size_t elen = sizeof(endseq) - 1;
  size_t i;
  if (n < elen) return n;
  for (i = from; i + elen <= n; i++)
    if (memcmp(buf + i, endseq, elen) == 0) return i;
  return n;
}

static void editorDrainPaste(const char *tail, size_t n) {
  static const char endseq[] = "\x1b[201~";
  const size_t elen = sizeof(endseq) - 1;
  char win[8192 + 8];
  size_t have = n < elen - 1 ? n : elen - 1;
  int idle = 0;
  memcpy(win, tail + n - have, have);
  for (;;) {
    ssize_t r = read(STDIN_FILENO, win + have, 8192);
    if (r == 0) { if (++idle > 100) return; continue; }
    if (r == -1) {
      if (errno == EINTR && !got_term) continue;
      return;
    }
    idle = 0;
    size_t total = have + (size_t)r;
    if (editorFindPasteEnd(win, total, 0) < total) return;
    have = total < elen - 1 ? total : elen - 1;
    memmove(win, win + total - have, have);
  }
}

static char *editorReadPasteBuf(int *len) {
  static const char endseq[] = "\x1b[201~";
  const size_t elen = sizeof(endseq) - 1;
  const size_t step = 8192; /* read this much at a time, not one byte at a time */
  size_t cap = step, n = 0;
  char *buf = xmalloc(cap);
  int idle = 0;

  for (;;) {
    if (n + step + 1 > cap) {
      if (cap >= OSHI_PASTE_MAX) { editorDrainPaste(buf, n); break; }
      size_t newcap = cap * 2;
      if (newcap > OSHI_PASTE_MAX) newcap = OSHI_PASTE_MAX;
      if (newcap <= cap) break; /* already at the cap */
      buf = xrealloc(buf, newcap);
      cap = newcap;
    }
    size_t want = cap - n;
    if (want > step) want = step;
    if (want == 0) break;
    ssize_t r = read(STDIN_FILENO, buf + n, want);
    if (r == 0) { /* a pause in the stream: wait a while for the rest */
      if (++idle > 100) break;
      continue;
    }
    if (r == -1) {
      if (errno == EINTR && !got_term) continue;
      break;
    }
    idle = 0;
    /* the terminator may straddle two reads: back up before scanning */
    size_t scan_from = n >= elen - 1 ? n - (elen - 1) : 0;
    n += (size_t)r;
    size_t hit = editorFindPasteEnd(buf, n, scan_from);
    if (hit < n) {
      n = hit;
      break;
    }
  }
  *len = (int)n;
  return buf;
}

/*** utf-8 ***/

/* decode one code point; invalid bytes give U+FFFD, one byte long */
static int utf8Decode(const unsigned char *s, int n, int *len) {
  unsigned char c = s[0];
  int need, cp, i;
  if (c < 0x80) { *len = 1; return c; }
  if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; }
  else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; }
  else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; }
  else { *len = 1; return 0xFFFD; }
  if (n < need + 1) { *len = 1; return 0xFFFD; }
  for (i = 1; i <= need; i++) {
    if ((s[i] & 0xC0) != 0x80) { *len = 1; return 0xFFFD; }
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) ||
      cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    *len = 1;
    return 0xFFFD;
  }
  *len = need + 1;
  return cp;
}

/* terminal cells for a code point: 0, 1 or 2 */
static int cpWidth(int cp) {
  static const struct { int lo, hi; } zero[] = {
    {0x0300,0x036F},{0x0483,0x0489},{0x0591,0x05BD},{0x05BF,0x05BF},
    {0x05C1,0x05C2},{0x05C4,0x05C5},{0x05C7,0x05C7},{0x0610,0x061A},
    {0x064B,0x065F},{0x0670,0x0670},{0x06D6,0x06DC},{0x06DF,0x06E4},
    {0x06E7,0x06E8},{0x06EA,0x06ED},{0x0E31,0x0E31},{0x0E34,0x0E3A},
    {0x0E47,0x0E4E},{0x1AB0,0x1AFF},{0x1DC0,0x1DFF},{0x200B,0x200F},
    {0x202A,0x202E},{0x2060,0x2064},{0x20D0,0x20FF},{0xFE00,0xFE0F},
    {0xFE20,0xFE2F},{0xFEFF,0xFEFF},{0x1F3FB,0x1F3FF},{0xE0100,0xE01EF},
    {0x0900,0x0902},{0x093A,0x093A},{0x093C,0x093C},{0x0941,0x0948},
    {0x094D,0x094D},{0x0951,0x0957},{0x0962,0x0963}
  };
  static const struct { int lo, hi; } wide[] = {
    {0x1100,0x115F},{0x2E80,0x303E},{0x3041,0x33FF},{0x3400,0x4DBF},
    {0x4E00,0x9FFF},{0xA000,0xA4CF},{0xA960,0xA97F},{0xAC00,0xD7A3},
    {0xF900,0xFAFF},{0xFE10,0xFE19},{0xFE30,0xFE6F},{0xFF00,0xFF60},
    {0xFFE0,0xFFE6},{0x1F300,0x1F64F},{0x1F680,0x1F6FF},{0x1F900,0x1F9FF},
    {0x1FA70,0x1FAFF},{0x20000,0x3FFFD},
    {0x231A,0x231B},{0x23E9,0x23EC},{0x23F0,0x23F0},{0x23F3,0x23F3},
    {0x25FD,0x25FE},{0x2614,0x2615},{0x2648,0x2653},{0x267F,0x267F},
    {0x2693,0x2693},{0x26A1,0x26A1},{0x26AA,0x26AB},{0x26BD,0x26BE},
    {0x26C4,0x26C5},{0x26CE,0x26CE},{0x26D4,0x26D4},{0x26EA,0x26EA},
    {0x26F2,0x26F3},{0x26F5,0x26F5},{0x26FA,0x26FA},{0x26FD,0x26FD},
    {0x2705,0x2705},{0x270A,0x270B},{0x2728,0x2728},{0x274C,0x274C},
    {0x274E,0x274E},{0x2753,0x2755},{0x2757,0x2757},{0x2795,0x2797},
    {0x27B0,0x27B0},{0x27BF,0x27BF},{0x2B1B,0x2B1C},{0x2B50,0x2B50},
    {0x2B55,0x2B55},{0x1F004,0x1F004},{0x1F0CF,0x1F0CF},{0x1F18E,0x1F18E},
    {0x1F191,0x1F19A},{0x1F200,0x1F2FF}
  };
  size_t i;
  if (cp < 0x300) return 1; /* includes control bytes: drawn as one '?' */
  for (i = 0; i < sizeof(zero) / sizeof(zero[0]); i++)
    if (cp >= zero[i].lo && cp <= zero[i].hi) return 0;
  for (i = 0; i < sizeof(wide) / sizeof(wide[0]); i++)
    if (cp >= wide[i].lo && cp <= wide[i].hi) return 2;
  return 1;
}

/* one character is a base plus its combining marks and ZWJ sequences */
static int editorCharEnd(const erow *row, int b) {
  int len, cp, zwj;
  if (b >= row->size) return row->size;
  cp = utf8Decode((const unsigned char *)row->chars + b, row->size - b, &len);
  b += len;
  zwj = cp == 0x200D;
  while (b < row->size) {
    int l2, c2 = utf8Decode((const unsigned char *)row->chars + b,
                            row->size - b, &l2);
    if (!zwj && cpWidth(c2) != 0) break;
    zwj = c2 == 0x200D;
    b += l2;
  }
  return b;
}

/* Where the character before byte b starts. */
static int editorPrevChar(const erow *row, int b) {
  int s = 0, e;
  if (b > row->size) b = row->size;
  while (s < b) {
    e = editorCharEnd(row, s);
    if (e >= b) return s;
    s = e;
  }
  return b > 0 ? b - 1 : 0;
}

/* cells the character at byte b takes, tabs included */
static int editorCellWidth(const erow *row, int b, int col) {
  int len, cp, w;
  if (row->chars[b] == '\t') return OSHI_TAB_STOP - (col % OSHI_TAB_STOP);
  cp = utf8Decode((const unsigned char *)row->chars + b, row->size - b, &len);
  w = cpWidth(cp);
  return w > 0 ? w : 1;
}

/* The same for plain strings (the status bar). */
static int editorStrWidth(const char *s, int n) {
  int w = 0, i = 0;
  while (i < n) {
    int l, cp = utf8Decode((const unsigned char *)s + i, n - i, &l);
    w += cpWidth(cp);
    i += l;
  }
  return w;
}

/* bytes of the longest prefix fitting maxcols cells */
static int editorFitHead(const char *s, int n, int maxcols) {
  int w = 0, i = 0;
  while (i < n) {
    int l, cp = utf8Decode((const unsigned char *)s + i, n - i, &l);
    int cw = cpWidth(cp);
    if (w + cw > maxcols) break;
    w += cw;
    i += l;
  }
  return i;
}

/* start of the longest suffix fitting maxcols cells */
static int editorFitTail(const char *s, int n, int maxcols) {
  int total = editorStrWidth(s, n), i = 0;
  while (i < n && total > maxcols) {
    int l, cp = utf8Decode((const unsigned char *)s + i, n - i, &l);
    total -= cpWidth(cp);
    i += l;
  }
  return i;
}

/* count characters in s, skipping UTF-8 continuation bytes */
int editorCharCount(const char *s, int len) {
  int n = 0, i;
  for (i = 0; i < len; i++)
    if ((unsigned char)(s[i] & 0xC0) != 0x80) n++;
  return n;
}

/* count lines in s; a last line without newline counts */
int editorLineCount(const char *s, int len) {
  int n = 0, i;
  for (i = 0; i < len; i++)
    if (s[i] == '\n') n++;
  if (len > 0 && s[len - 1] != '\n') n++;
  return n;
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
  int rx = 0, b = 0;
  if (cx > row->size) cx = row->size;
  while (b < cx) {
    rx += editorCellWidth(row, b, rx);
    b = editorCharEnd(row, b);
  }
  return rx;
}

int editorRowRxToCx(erow *row, int rx) {
  int cur = 0, b = 0;
  while (b < row->size) {
    int w = editorCellWidth(row, b, cur);
    if (cur + w > rx) return b;
    cur += w;
    b = editorCharEnd(row, b);
  }
  return row->size;
}

/* cursor column, which up/down try to keep */
void editorSyncGoal(void) {
  E.goal_rx = E.cy < E.numrows ? editorRowCxToRx(&E.row[E.cy], E.cx) : 0;
}

/* keep the cursor on a valid row and column */
static void editorClampCursor(void) {
  if (E.numrows == 0) { E.cx = 0; E.cy = 0; return; }
  if (E.cy >= E.numrows) E.cy = E.numrows - 1;
  if (E.cy < 0) E.cy = 0;
  if (E.cx > E.row[E.cy].size) E.cx = E.row[E.cy].size;
  if (E.cx < 0) E.cx = 0;
}

static void editorRowsReserve(int n) {
  int cap;
  if (n <= E.rowcap) return;
  cap = E.rowcap ? E.rowcap : 64;
  while (cap < n) cap *= 2;
  E.row = xrealloc(E.row, sizeof(erow) * cap);
  E.rowcap = cap;
}

void editorInsertRow(int at, const char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;
  editorRowsReserve(E.numrows + 1);
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  E.row[at].size = (int)len;
  E.row[at].chars = xmalloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.numrows++;
}

void editorFreeRow(erow *row) {
  free(row->chars);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
}

/* swap ndel rows at `at` for nins new ones in one move */
static void editorReplaceRows(int at, int ndel, char **s, int *sz, int nins) {
  int i, tail = E.numrows - at - ndel;
  for (i = 0; i < ndel; i++) editorFreeRow(&E.row[at + i]);
  editorRowsReserve(E.numrows - ndel + nins);
  if (tail > 0)
    memmove(&E.row[at + nins], &E.row[at + ndel], sizeof(erow) * tail);
  for (i = 0; i < nins; i++) {
    E.row[at + i].size = sz[i];
    E.row[at + i].chars = xmalloc(sz[i] + 1);
    memcpy(E.row[at + i].chars, s[i], sz[i]);
    E.row[at + i].chars[sz[i]] = '\0';
  }
  E.numrows += nins - ndel;
}

void editorRowInsertBytes(erow *row, int at, const char *s, int n) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = xrealloc(row->chars, row->size + n + 1);
  memmove(&row->chars[at + n], &row->chars[at], row->size - at + 1);
  memcpy(&row->chars[at], s, n);
  row->size += n;
}

void editorRowAppendString(erow *row, const char *s, size_t len) {
  row->chars = xrealloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += (int)len;
  row->chars[row->size] = '\0';
}

/* Delete the bytes [from,to) in a row. */
void editorRowDeleteRange(erow *row, int from, int to) {
  if (from < 0) from = 0;
  if (to > row->size) to = row->size;
  if (from >= to) return;
  memmove(&row->chars[from], &row->chars[to], row->size - to + 1);
  row->size -= (to - from);
}

/*** undo ***/

/* undo records are small diffs; swapping one is also redo */
typedef struct editorRec {
  int at;        /* first row the change touched */
  char **old;    /* the rows that used to be there, each on its own */
  int *osize;
  int nold;      /* how many of them */
  int nnew;      /* how many rows sit there now */
  int cx0, cy0;  /* where the cursor goes when this record is applied */
  int cx1, cy1;  /* and where it goes when it is applied back again */
} editorRec;

static editorRec *undo_stack;
static int undo_count, undo_cap;
static long undo_dropped;   /* steps that fell off the old end */
static editorRec *redo_stack;
static int redo_count, redo_cap;
static editorRec undo_pend;  /* the change being made right now */
static int undo_pending;
static int undo_rows_before;
static int undo_run;         /* plain typing keeps running: one undo step */
static int undo_run_cx, undo_run_cy; /* where the next key of that run lands */
static char *saved_text;     /* the buffer bytes the file on disk holds */
static int saved_len;        /* how long that is */

static void editorFreeRec(editorRec *r) {
  int i;
  for (i = 0; i < r->nold; i++) free(r->old[i]);
  free(r->old);
  free(r->osize);
  r->old = NULL;
  r->osize = NULL;
  r->nold = 0;
}

static void editorCaptureRows(editorRec *r, int at, int n) {
  int i;
  r->at = at;
  r->nold = n;
  r->old = xmalloc(sizeof(char *) * (n > 0 ? n : 1));
  r->osize = xmalloc(sizeof(int) * (n > 0 ? n : 1));
  for (i = 0; i < n; i++) {
    r->osize[i] = E.row[at + i].size;
    r->old[i] = xmalloc(E.row[at + i].size + 1);
    memcpy(r->old[i], E.row[at + i].chars, E.row[at + i].size);
    r->old[i][E.row[at + i].size] = '\0';
  }
}

/* modified means the bytes differ from the file on disk */
static void editorSnapshot(void) {
  free(saved_text);
  saved_text = editorRowsToString(&saved_len);
}

static void editorUpdateDirty(void) {
  long total = 0;
  int j, len;
  char *cur;
  for (j = 0; j < E.numrows; j++)
    total += E.row[j].size + (E.crlf ? 2 : 1);
  if (total != saved_len) {
    E.dirty = 1;
    return;
  }
  cur = editorRowsToString(&len);
  E.dirty = saved_text == NULL || len != saved_len ||
            memcmp(cur, saved_text, len) != 0;
  free(cur);
}

static void editorPushUndo(editorRec r) {
  if (undo_count == OSHI_UNDO_MAX) { /* the oldest falls off the end */
    editorFreeRec(&undo_stack[0]);
    memmove(undo_stack, undo_stack + 1, sizeof(editorRec) * (undo_count - 1));
    undo_count--;
    undo_dropped++;
  }
  if (undo_count == undo_cap) {
    undo_cap = undo_cap ? undo_cap * 2 : 64;
    undo_stack = xrealloc(undo_stack, sizeof(editorRec) * undo_cap);
  }
  undo_stack[undo_count++] = r;
}

static void editorPushRedo(editorRec r) {
  if (redo_count == redo_cap) {
    redo_cap = redo_cap ? redo_cap * 2 : 64;
    redo_stack = xrealloc(redo_stack, sizeof(editorRec) * redo_cap);
  }
  redo_stack[redo_count++] = r;
}

/* call before a change: rows [at, at+nold) may be touched */
void editorUndoBegin(int at, int nold) {
  if (at < 0) at = 0;
  if (at > E.numrows) at = E.numrows;
  if (nold < 0) nold = 0;
  if (at + nold > E.numrows) nold = E.numrows - at;
  editorCaptureRows(&undo_pend, at, nold);
  undo_pend.cx0 = E.cx;
  undo_pend.cy0 = E.cy;
  undo_rows_before = E.numrows;
  undo_pending = 1;
}

/* call after a change: files the step and clears redo */
void editorUndoCommit(void) {
  if (!undo_pending) return;
  undo_pending = 0;
  undo_pend.nnew = undo_pend.nold + (E.numrows - undo_rows_before);
  undo_pend.cx1 = E.cx;
  undo_pend.cy1 = E.cy;
  if (redo_count > 0) {
    while (redo_count > 0) editorFreeRec(&redo_stack[--redo_count]);
  }
  editorPushUndo(undo_pend);
  undo_run = 0;
  editorUpdateDirty();
}

/* swap the record with the buffer */
static void editorRecApply(editorRec *r) {
  editorRec cur;
  int oldn = r->nold, t;
  if (r->at > E.numrows) r->at = E.numrows;
  if (r->at + r->nnew > E.numrows) r->nnew = E.numrows - r->at;
  editorCaptureRows(&cur, r->at, r->nnew);
  editorReplaceRows(r->at, r->nnew, r->old, r->osize, r->nold);
  editorFreeRec(r);
  r->old = cur.old;
  r->osize = cur.osize;
  r->nold = cur.nold;
  r->nnew = oldn;
  E.cx = r->cx0;
  E.cy = r->cy0;
  t = r->cx0; r->cx0 = r->cx1; r->cx1 = t;
  t = r->cy0; r->cy0 = r->cy1; r->cy1 = t;
  editorClampCursor();
}

void editorUndo(void) {
  if (undo_count == 0) {
    editorSetStatusMessage("Nothing to undo");
    return;
  }
  editorRec r = undo_stack[--undo_count];
  editorRecApply(&r);
  editorPushRedo(r);
  undo_run = 0;
  editorUpdateDirty();
}

void editorRedo(void) {
  if (redo_count == 0) {
    editorSetStatusMessage("Nothing to redo");
    return;
  }
  editorRec r = redo_stack[--redo_count];
  editorRecApply(&r);
  editorPushUndo(r);
  undo_run = 0;
  editorUpdateDirty();
}

/*** selection ***/

static void editorClampPos(int *y, int *x) {
  if (E.numrows == 0) { *y = 0; *x = 0; return; }
  if (*y < 0) *y = 0;
  if (*y >= E.numrows) { *y = E.numrows - 1; *x = E.row[*y].size; }
  if (*x > E.row[*y].size) *x = E.row[*y].size;
  if (*x < 0) *x = 0;
}

/* selection runs anchor to cursor, either way round */
void editorSelectionRange(int *sy, int *sx, int *ey, int *ex) {
  int forward = E.sel_cy < E.cy || (E.sel_cy == E.cy && E.sel_cx <= E.cx);
  *sy = forward ? E.sel_cy : E.cy;
  *sx = forward ? E.sel_cx : E.cx;
  *ey = forward ? E.cy : E.sel_cy;
  *ex = forward ? E.cx : E.sel_cx;
  editorClampPos(sy, sx);
  editorClampPos(ey, ex);
}

/* Which part of a row the selection covers, in bytes. */
int editorRowSelection(erow *row, int filerow, int *from, int *to) {
  int sy, sx, ey, ex;
  if (!E.sel) return 0;
  editorSelectionRange(&sy, &sx, &ey, &ex);
  if (filerow < sy || filerow > ey) return 0;
  *from = filerow == sy ? sx : 0;
  *to = filerow == ey ? ex : row->size;
  return 1;
}

/* cursor to the start or end of the file */
void editorMoveToEnd(int dir) {
  if (dir > 0) {
    E.cy = E.numrows > 0 ? E.numrows - 1 : 0;
    E.cx = E.numrows > 0 ? E.row[E.cy].size : 0;
  } else {
    E.cy = 0;
    E.cx = 0;
  }
}

/* true when the selection covers the whole buffer */
int editorAllSelected(void) {
  int sy, sx, ey, ex;
  if (!E.sel) return 0;
  if (E.numrows == 0) return 1;
  editorSelectionRange(&sy, &sx, &ey, &ex);
  return sy == 0 && sx == 0 && ey == E.numrows - 1 && ex == E.row[ey].size;
}

void editorSelectAll(void) {
  E.sel_cy = 0;
  E.sel_cx = 0;
  E.sel = 1;
  editorMoveToEnd(1);
}

/* selected text as on disk: rows joined by newlines */
char *editorSelectionToString(int *len) {
  int sy, sx, ey, ex, y, total = 0, at = 0;
  char *buf;
  if (E.numrows == 0) { *len = 0; return xmalloc(1); }
  editorSelectionRange(&sy, &sx, &ey, &ex);

  for (y = sy; y <= ey; y++) {
    erow *row = &E.row[y];
    int from = y == sy ? sx : 0;
    int to = y == ey ? ex : row->size;
    if (to > from) total += to - from;
    if (y < ey) total++;
  }

  buf = xmalloc(total + 1);
  for (y = sy; y <= ey; y++) {
    erow *row = &E.row[y];
    int from = y == sy ? sx : 0;
    int to = y == ey ? ex : row->size;
    if (to > from) {
      memcpy(buf + at, row->chars + from, to - from);
      at += to - from;
    }
    if (y < ey) buf[at++] = '\n';
  }

  *len = at;
  return buf;
}

/*** editor operations ***/

/* raw mutators change the buffer only; public ones add undo */

/* delete the selection, cursor to its start */
static void editorRawDeleteSelection(void) {
  int sy, sx, ey, ex;
  editorSelectionRange(&sy, &sx, &ey, &ex);
  E.sel = 0;
  if (E.numrows == 0) return;

  erow *first = &E.row[sy];
  if (sy == ey) {
    if (ex > sx) editorRowDeleteRange(first, sx, ex);
  } else {
    /* last row's tail joins the first, the rest goes away */
    erow *last = &E.row[ey];
    int tail = last->size - ex, y;
    first->chars = xrealloc(first->chars, sx + tail + 1);
    memcpy(&first->chars[sx], &last->chars[ex], tail);
    first->size = sx + tail;
    first->chars[first->size] = '\0';
    for (y = ey; y > sy; y--) editorDelRow(y);
  }
  E.cy = sy;
  E.cx = sx;
}

static void editorRawInsertBytes(const char *s, int n) {
  while (E.numrows <= E.cy) editorInsertRow(E.numrows, "", 0);
  if (E.cx > E.row[E.cy].size) E.cx = E.row[E.cy].size;
  editorRowInsertBytes(&E.row[E.cy], E.cx, s, n);
  E.cx += n;
}

static void editorRawNewline(void) {
  while (E.numrows <= E.cy) editorInsertRow(E.numrows, "", 0);
  erow *row = &E.row[E.cy];
  if (E.cx > row->size) E.cx = row->size;
  editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
  row = &E.row[E.cy];
  row->size = E.cx;
  row->chars[row->size] = '\0';
  E.cy++;
  E.cx = 0;
}

void editorDeleteSelection(void) {
  int sy, sx, ey, ex;
  if (E.numrows == 0) { E.sel = 0; return; }
  editorSelectionRange(&sy, &sx, &ey, &ex);
  editorUndoBegin(sy, ey - sy + 1);
  editorRawDeleteSelection();
  editorUndoCommit();
}

/* type bytes; replaces a selection; typing merges into one undo step */
void editorTypeBytes(const char *s, int n) {
  int sy, sx, ey, ex;
  if (E.sel && E.numrows > 0) {
    editorSelectionRange(&sy, &sx, &ey, &ex);
    editorUndoBegin(sy, ey - sy + 1);
    editorRawDeleteSelection();
    editorRawInsertBytes(s, n);
    editorUndoCommit();
    return;
  }
  E.sel = 0;
  if (undo_run && undo_count > 0 && E.cy == undo_run_cy && E.cx == undo_run_cx) {
    editorRawInsertBytes(s, n);
    undo_stack[undo_count - 1].cx1 = E.cx;
    undo_stack[undo_count - 1].cy1 = E.cy;
  } else {
    editorUndoBegin(E.cy, E.cy < E.numrows ? 1 : 0);
    editorRawInsertBytes(s, n);
    editorUndoCommit();
    undo_run = 1;
  }
  undo_run_cy = E.cy;
  undo_run_cx = E.cx;
}

void editorInsertNewline(void) {
  int sy, sx, ey, ex;
  if (E.sel && E.numrows > 0) {
    editorSelectionRange(&sy, &sx, &ey, &ex);
    editorUndoBegin(sy, ey - sy + 1);
    editorRawDeleteSelection();
  } else {
    E.sel = 0;
    editorUndoBegin(E.cy, E.cy < E.numrows ? 1 : 0);
  }
  editorRawNewline();
  editorUndoCommit();
}

/* backspace (forward=0) or delete (forward=1) one whole character */
void editorDeleteChar(int forward) {
  if (E.numrows == 0) return;
  if (E.cy >= E.numrows) E.cy = E.numrows - 1;
  erow *row = &E.row[E.cy];
  if (E.cx > row->size) E.cx = row->size;
  if (!forward) {
    if (E.cx > 0) {
      int p = editorPrevChar(row, E.cx);
      editorUndoBegin(E.cy, 1);
      editorRowDeleteRange(row, p, E.cx);
      E.cx = p;
      editorUndoCommit();
    } else if (E.cy > 0) {
      editorUndoBegin(E.cy - 1, 2);
      E.cx = E.row[E.cy - 1].size;
      editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
      editorDelRow(E.cy);
      E.cy--;
      editorUndoCommit();
    }
  } else {
    if (E.cx < row->size) {
      int e = editorCharEnd(row, E.cx);
      editorUndoBegin(E.cy, 1);
      editorRowDeleteRange(row, E.cx, e);
      editorUndoCommit();
    } else if (E.cy + 1 < E.numrows) {
      editorUndoBegin(E.cy, 2);
      editorRowAppendString(row, E.row[E.cy + 1].chars, E.row[E.cy + 1].size);
      editorDelRow(E.cy + 1);
      editorUndoCommit();
    }
  }
}

/* ctrl-backspace: delete the word behind the cursor in two stages */
void editorDeleteWordBackward(void) {
  if (E.numrows == 0) return;
  if (E.cy >= E.numrows) E.cy = E.numrows - 1;
  erow *row = &E.row[E.cy];
  if (E.cx > row->size) E.cx = row->size;

  if (E.cx == 0) {
    if (E.cy == 0) return;
    editorUndoBegin(E.cy - 1, 2);
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
    editorUndoCommit();
    return;
  }

  int from = E.cx;
  while (from > 0 && isspace((unsigned char)row->chars[from - 1])) from--;
  if (from == E.cx) { /* no spaces to clear: go on into the word before them */
    while (from > 0 && !isspace((unsigned char)row->chars[from - 1])) from--;
  }
  if (from == E.cx) return; /* nothing behind us */

  editorUndoBegin(E.cy, 1);
  editorRowDeleteRange(row, from, E.cx);
  E.cx = from;
  editorUndoCommit();
}

/* Delete the whole line the cursor sits on. */
void editorDeleteLine(void) {
  if (E.numrows == 0) return; /* nothing on the page yet */
  if (E.cy >= E.numrows) E.cy = E.numrows - 1;
  editorUndoBegin(E.cy, 1);
  editorDelRow(E.cy);
  if (E.numrows == 0) {
    E.cx = E.cy = 0;
  } else {
    if (E.cy >= E.numrows) E.cy = E.numrows - 1;
    E.cx = editorRowRxToCx(&E.row[E.cy], E.goal_rx);
  }
  editorUndoCommit();
}

/* insert text as one undo step; returns lines inserted, 0 if none */
static int editorInsertText(const char *s, int n) {
  int i, m = 0, start = 0, sy, sx, ey, ex;
  char *clean;
  if (n <= 0) return 0;
  clean = xmalloc(n + 1);
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '\r') {
      clean[m++] = '\n';
      if (i + 1 < n && s[i + 1] == '\n') i++;
    } else if (c == '\n' || c == '\t' || (c >= 32 && c != 127)) {
      clean[m++] = (char)c;
    }
  }
  if (m == 0) { free(clean); return 0; }

  if (E.sel && E.numrows > 0) {
    editorSelectionRange(&sy, &sx, &ey, &ex);
    editorUndoBegin(sy, ey - sy + 1);
    editorRawDeleteSelection();
  } else {
    E.sel = 0;
    editorUndoBegin(E.cy, E.cy < E.numrows ? 1 : 0);
  }
  for (i = 0; i <= m; i++) {
    if (i == m || clean[i] == '\n') {
      if (i > start) editorRawInsertBytes(clean + start, i - start);
      if (i < m) editorRawNewline();
      start = i + 1;
    }
  }
  editorUndoCommit();
  int nlines = editorLineCount(clean, m);
  free(clean);
  return nlines;
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + (E.crlf ? 2 : 1);
  *buflen = totlen;
  char *buf = xmalloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    if (E.crlf) *p++ = '\r';
    *p++ = '\n';
  }
  return buf;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);
  if (E.filename == NULL) die("strdup");

  struct stat sb;
  if (stat(filename, &sb) == 0 && S_ISDIR(sb.st_mode)) {
    errno = EISDIR;
    die(filename);
  }

  FILE *fp = fopen(filename, "r");
  if (!fp) {
    if (errno == ENOENT) return; /* a new file: start empty, keep the name */
    die("fopen");
  }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  int crlf_lines = 0, lf_lines = 0;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    if (linelen > 0 && line[linelen - 1] == '\n') {
      if (linelen > 1 && line[linelen - 2] == '\r') crlf_lines++;
      else lf_lines++;
    }
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) linelen--;
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.crlf = crlf_lines > lf_lines; /* whichever the file mostly uses */
  editorSnapshot(); /* this content is what the file holds */
  E.dirty = 0;
}

/* write() can stop short or be interrupted */
int editorWriteAll(int fd, const char *buf, int len) {
  int written = 0;
  while (written < len) {
    ssize_t n = write(fd, buf + written, len - written);
    if (n == -1) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return -1;
    written += (int)n;
  }
  return 0;
}

/* save via temp file and rename; fall back to in-place write */
static int editorWriteFile(const char *name, const char *buf, int len) {
  char *real = realpath(name, NULL);
  const char *path = real ? real : name;
  struct stat st;
  int have = stat(path, &st) == 0;
  int rc = -1, err = 0, fd;

  if (!have || S_ISREG(st.st_mode)) {
    size_t n = strlen(path) + 32;
    char *tmp = xmalloc(n);
    snprintf(tmp, n, "%s.oshi-tmp-%ld", path, (long)getpid());
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd != -1) {
      if (have) {
        int r = fchmod(fd, st.st_mode & 07777);
        (void)r;
        r = fchown(fd, st.st_uid, st.st_gid);
        (void)r;
      }
      if (editorWriteAll(fd, buf, len) == 0 && fsync(fd) == 0) rc = 0;
      else err = errno;
      if (close(fd) == -1 && rc == 0) { rc = -1; err = errno; }
      if (rc == 0 && rename(tmp, path) == -1) { rc = -1; err = errno; }
      if (rc == -1) unlink(tmp);
      free(tmp);
      free(real);
      errno = err;
      return rc;
    }
    free(tmp);
  }

  fd = open(path, O_WRONLY | O_CREAT, 0644);
  if (fd == -1) {
    err = errno;
    free(real);
    errno = err;
    return -1;
  }
  if (editorWriteAll(fd, buf, len) == 0 && ftruncate(fd, len) == 0) rc = 0;
  else err = errno;
  if (close(fd) == -1 && rc == 0) { rc = -1; err = errno; }
  free(real);
  errno = err;
  return rc;
}

/* buffer now matches disk: record it and start a fresh undo step */
static void editorMarkSaved(void) {
  editorSnapshot();
  undo_run = 0; /* typing after a save starts a new step */
  editorUpdateDirty();
}

/* directory part of a path (malloc'd), or NULL if none */
static char *editorParentDir(const char *path) {
  const char *slash = strrchr(path, '/');
  size_t n;
  char *d;
  if (!slash) return NULL;
  n = (size_t)(slash - path);
  if (n == 0) return NULL;
  d = xmalloc(n + 1);
  memcpy(d, path, n);
  d[n] = '\0';
  while (n > 1 && d[n - 1] == '/') d[--n] = '\0'; /* "a//b" */
  return d;
}

static int editorIsDir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* mkdir -p; -1 with errno on failure */
static int editorMkdirP(const char *dir) {
  char *tmp = strdup(dir);
  size_t n, i;
  if (tmp == NULL) die("strdup");
  n = strlen(tmp);
  for (i = 1; i <= n; i++) {
    if (tmp[i] == '/' || tmp[i] == '\0') {
      char keep = tmp[i];
      tmp[i] = '\0';
      if (mkdir(tmp, 0777) == -1 && errno != EEXIST) {
        int e = errno;
        free(tmp);
        errno = e;
        return -1;
      }
      tmp[i] = keep;
    }
  }
  free(tmp);
  if (!editorIsDir(dir)) { errno = ENOTDIR; return -1; }
  return 0;
}

/* yes/no on the bar: y or enter yes; n, esc or ctrl-q no */
static int editorConfirm(const char *fmt, const char *arg) {
  for (;;) {
    int k;
    editorSetStatusMessage(fmt, arg);
    editorRefreshScreen();
    k = editorReadKey();
    if (k == 'y' || k == 'Y' || k == '\r') return 1;
    if (k == 'n' || k == 'N' || k == '\x1b' || k == CTRL_KEY('q')) return 0;
    if (k == PASTE_KEY) editorSkipPaste();
  }
}

void editorSave(void) {
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s ", NULL, NULL);
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }

  int len;
  char *buf = editorRowsToString(&len);
  int rc = editorWriteFile(E.filename, buf, len);
  int err = errno;
  if (rc != 0 && err == ENOENT) {
    /* missing directory: offer to create it, then retry */
    char *dir = editorParentDir(E.filename);
    if (dir && !editorIsDir(dir)) {
      if (!editorConfirm("Directory '%s' doesn't exist. Create it? (y/n)", dir)) {
        free(dir);
        free(buf);
        editorSetStatusMessage("Save cancelled");
        return;
      }
      if (editorMkdirP(dir) == 0) {
        rc = editorWriteFile(E.filename, buf, len);
        err = errno;
      } else {
        err = errno;
      }
    }
    free(dir);
  }
  if (rc == 0) {
    free(buf);
    editorMarkSaved();
    editorSetStatusMessage("%d bytes written to disk", len);
    return;
  }
  /* refused write: offer to redo it as root; other errors just report */
  if (err == EACCES || err == EPERM) {
    if (editorSaveElevated(buf, len) == 0) editorMarkSaved();
    free(buf);
    return;
  }
  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(err));
}

/*** clipboard ***/

/* copy to the terminal clipboard via one OSC 52 sequence */
void editorClipboardPut(const char *buf, int len) {
  static const char table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  char *b64 = xmalloc(4 * ((len + 2) / 3) + 1);

  int i, out = 0;
  for (i = 0; i + 2 < len; i += 3) {
    unsigned int n = ((unsigned char)buf[i] << 16) |
                     ((unsigned char)buf[i + 1] << 8) |
                      (unsigned char)buf[i + 2];
    b64[out++] = table[(n >> 18) & 63];
    b64[out++] = table[(n >> 12) & 63];
    b64[out++] = table[(n >> 6) & 63];
    b64[out++] = table[n & 63];
  }
  if (i < len) { /* one or two bytes left, padded with '=' */
    unsigned int n = (unsigned char)buf[i] << 16;
    b64[out++] = table[(n >> 18) & 63];
    if (i + 1 < len) {
      n |= (unsigned char)buf[i + 1] << 8;
      b64[out++] = table[(n >> 12) & 63];
      b64[out++] = table[(n >> 6) & 63];
      b64[out++] = '=';
    } else {
      b64[out++] = table[(n >> 12) & 63];
      b64[out++] = '=';
      b64[out++] = '=';
    }
  }

  editorWriteAll(STDOUT_FILENO, "\x1b]52;c;", 7);
  editorWriteAll(STDOUT_FILENO, b64, out);
  editorWriteAll(STDOUT_FILENO, "\x07", 1);

  free(b64);
}

/* fall back to an external clipboard tool; 0 ok, -1 none worked */
static int editorClipboardExternal(const char *text, int len) {
  static const char *cmds[] = {
    "wl-copy",
    "xclip -selection clipboard",
    "xsel --clipboard --input",
    "pbcopy",
    "clip.exe",
    NULL
  };
  int i;
  for (i = 0; cmds[i]; i++) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "%s >/dev/null 2>/dev/null", cmds[i]);
    FILE *fp = popen(cmd, "w");
    if (!fp) continue;
    size_t written = fwrite(text, 1, len, fp);
    int rc = pclose(fp);
    if (written == (size_t)len && rc == 0) return 0;
  }
  return -1;
}

/* selection, else the cursor line with its newline; malloc'd or NULL */
static char *editorCopyGrab(int *len, int *isline) {
  if (E.sel) {
    *isline = 0;
    return editorSelectionToString(len);
  }
  if (E.numrows == 0) return NULL;
  erow *row = &E.row[E.cy < E.numrows ? E.cy : E.numrows - 1];
  char *text = xmalloc(row->size + 2);
  memcpy(text, row->chars, row->size);
  text[row->size] = '\n';
  *len = row->size + 1;
  *isline = 1;
  return text;
}

/* keep our own copy and push to the terminal; settle newest-wins here */
static void editorClipSet(char *text, int len, int isline, const char *verb) {
  int n = editorLineCount(text, len);
  int pushed = 0, shared = 0;
  free(E.clip);
  E.clip = text;
  E.clip_len = len;
  E.clip_line = isline;
  E.clip_shared = 0;
  if (len <= OSHI_OSC52_MAX) {
    editorClipboardPut(text, len);
    pushed = 1;
  } else if (editorClipboardExternal(text, len) == 0) {
    pushed = 1;
  }
  if (pushed) {
    /* compare against the text the reader would produce, not raw bytes */
    int tlen = len, tnl = (len > 0 && text[len - 1] == '\n');
    char *trim = NULL;
    if (tnl) {
      trim = xmalloc((size_t)len);
      memcpy(trim, text, (size_t)len - 1);
      trim[len - 1] = '\0';
      tlen = len - 1;
    }
    int tries;
    for (tries = 0; tries < 3 && !shared; tries++) {
      int got = 0;
      char *back = editorClipboardRead(&got);
      if (back) {
        if (got == tlen && memcmp(back, trim ? trim : text, (size_t)tlen) == 0)
          shared = 1;
        /* also accept the un-stripped text */
        else if (got == len && memcmp(back, text, (size_t)len) == 0)
          shared = 1;
        /* a foreign readable answer settles it; no retry */
        else if (got > 0)
          break;
        free(back);
      } else {
        /* nothing readable: stop retrying */
        break;
      }
      if (!shared && tries < 2) usleep(OSHI_CLIP_SETTLE_US);
    }
    free(trim);
    E.clip_shared = shared;
  }
  if (len <= OSHI_OSC52_MAX)
    editorSetStatusMessage("%s %d line%s", verb, n, n == 1 ? "" : "s");
  else if (pushed)
    editorSetStatusMessage("%s %d lines", verb, n);
  else {
    /* worded to fit the status bar */
    editorSetStatusMessage("%s %d lines: clipboard failed, ctrl-v ok", verb, n);
  }
}

void editorCopySelection(void) {
  int len = 0, isline = 0;
  char *text = editorCopyGrab(&len, &isline);
  if (text == NULL || len == 0) {
    free(text);
    editorSetStatusMessage("Nothing to copy");
  } else {
    editorClipSet(text, len, isline, "Copied");
  }
  /* selection stays up; the next ordinary key drops it */
}

void editorCut(void) {
  int len = 0, isline = 0;
  char *text = editorCopyGrab(&len, &isline);
  if (text == NULL || len == 0) {
    free(text);
    editorSetStatusMessage("Nothing to cut");
    E.sel = 0;
    return;
  }
  editorClipSet(text, len, isline, "Cut");
  if (isline) editorDeleteLine();
  else editorDeleteSelection();
}

static int editorInPath(const char *name) {
  const char *p = getenv("PATH");
  if (!p) return 0;
  for (;;) {
    const char *end = strchr(p, ':');
    size_t dirlen = end ? (size_t)(end - p) : strlen(p);
    char full[4096];
    if (dirlen == 0) snprintf(full, sizeof(full), "./%s", name);
    else snprintf(full, sizeof(full), "%.*s/%s", (int)dirlen, p, name);
    if (access(full, X_OK) == 0) return 1;
    if (!end) return 0;
    p = end + 1;
  }
}

/* read the system clipboard with whichever tool exists; malloc'd or NULL */
static char *editorClipboardRead(int *len) {
  static const struct { const char *cmd; int risky; } cmds[] = {
    { "wl-paste -n", 1 },
    { "xclip -selection clipboard -o", 1 },
    { "xsel --clipboard --output", 1 },
    { "pbpaste", 0 },
    { "powershell.exe -NoProfile -Command \"Get-Clipboard -Raw\"", 0 },
    { NULL, 0 }
  };
  static const char *tmo;
  static int tmo_known;
  int i;
  if (!tmo_known) {
    tmo = editorInPath("timeout") ? "timeout"
        : editorInPath("gtimeout") ? "gtimeout" : NULL;
    tmo_known = 1;
  }
  for (i = 0; cmds[i].cmd; i++) {
    char cmd[200];
    if (cmds[i].risky && !tmo) continue;
    if (tmo)
      snprintf(cmd, sizeof(cmd), "%s %d %s 2>/dev/null", tmo,
               OSHI_CLIP_TIMEOUT, cmds[i].cmd);
    else
      snprintf(cmd, sizeof(cmd), "%s 2>/dev/null", cmds[i].cmd);
    FILE *fp = popen(cmd, "r");
    if (!fp) continue;
    size_t have = 0, cap = 4096;
    char *out = xmalloc(cap + 1);
    while (1) {
      size_t n, room;
      if (have + 512 > cap) {
        cap = have + 4096;
        out = xrealloc(out, cap + 1);
      }
      room = cap - have;
      n = fread(out + have, 1, room, fp);
      have += n;
      if (n < room) break; /* short read: end of output, or an error */
    }
    out[have] = '\0';
    int rc = pclose(fp);
    if (rc == 0 && have > 0) { *len = (int)have; return out; }
    free(out);
  }
  *len = 0;
  return NULL;
}

/* ctrl-v: paste the newest of E.clip and the system clipboard */
void editorPasteInternal(void) {
  char *text = E.clip;
  int len = E.clip_len;
  int isline = E.clip_line;
  int free_text = 0;

  if (text != NULL && len > 0 && E.clip_shared) {
    /* clipboard differs from our copy: it is newer, so it wins */
    int got = 0;
    char *ext = editorClipboardRead(&got);
    int same = 0;
    if (ext) {
      if (got == len && memcmp(ext, text, (size_t)len) == 0)
        same = 1;
      else if (len > 0 && text[len - 1] == '\n' && got == len - 1 &&
               memcmp(ext, text, (size_t)got) == 0)
        same = 1; /* our own text, minus the newline the tool trimmed */
      if (!same && got > 0) {
        text = ext;
        len = got;
        isline = 0; /* someone else's text is not a "whole line" cut */
        free_text = 1;
      } else {
        free(ext); /* same text, or unreadable: ours is the one to use */
      }
    }
  }

  if (text == NULL || len == 0) {
    text = editorClipboardRead(&len);
    free_text = 1; /* only our own malloc'd buffer, not E.clip */
    if (text == NULL || len == 0) {
      editorSetStatusMessage("Nothing to paste");
      free(text);
      return;
    }
    isline = 0; /* external text pastes in-line */
  }

  /* same wording and line count as copy/cut */
  int n = editorLineCount(text, len);
  if (isline && !E.sel) { /* a whole line goes in above this one */
    int at = E.cy < E.numrows ? E.cy : 0;
    int had_rows = E.numrows > 0;
    editorUndoBegin(at, 0);
    editorInsertRow(at, text, len - 1);
    if (had_rows) E.cy++; /* the cursor stays on the line it was on */
    editorUndoCommit();
  } else {
    n = editorInsertText(text, len);
    if (n == 0) {
      editorSetStatusMessage("Nothing to paste");
      if (free_text) free(text);
      return;
    }
  }
  editorSetStatusMessage("Pasted %d line%s", n, n == 1 ? "" : "s");
  if (free_text) free(text); /* free external text only; E.clip is not ours to free */
}

/*** find ***/

/* case-insensitive prefix compare of n bytes */
int editorIcmp(const char *s1, const char *s2, size_t n) {
  size_t i;
  for (i = 0; i < n; i++)
    if (tolower((unsigned char)s1[i]) != tolower((unsigned char)s2[i])) return 0;
  return 1;
}

/* case-insensitive strstr (strcasestr is GNU-only) */
char *editorIstrstr(const char *hay, const char *needle) {
  size_t hl = strlen(hay), nl = strlen(needle);
  size_t i;
  for (i = 0; i + nl <= hl; i++) {
    size_t j;
    for (j = 0; j < nl; j++)
      if (tolower((unsigned char)hay[i + j]) !=
          tolower((unsigned char)needle[j])) break;
    if (j == nl) return (char *)hay + i;
  }
  return NULL;
}

/* what find is hunting for, as byte offsets into row text */
static char find_q[256];
static int find_on;
static int find_row = -1;
static int find_col;
static long find_time; /* ms: when the match counter last changed */

void editorFindCallback(char *query, int key) {
  static int last_match = -1;
  static int direction = 1;

  snprintf(find_q, sizeof(find_q), "%s", query);
  find_on = query[0] != '\0';

  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    find_on = 0;
    /* keep find_row/find_col so the search stays live after the prompt */
    return;
  } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
    direction = 1;
  } else if (key == ARROW_LEFT || key == ARROW_UP) {
    direction = -1;
  } else {
    last_match = -1;
    direction = 1;
  }
  if (last_match == -1) direction = 1;
  int current = last_match;
  int i;
  if (!query[0]) { /* empty query: stay put */
    last_match = -1;
    direction = 1;
    return;
  }
  find_row = -1; /* rescan: drop the stale position */
  for (i = 0; i < E.numrows; i++) {
    current += direction;
    if (current == -1) current = E.numrows - 1;
    else if (current == E.numrows) current = 0;
    erow *row = &E.row[current];
    char *match = editorIstrstr(row->chars, query);
    if (match) {
      last_match = current;
      E.cy = current;
      E.cx = (int)(match - row->chars);
      find_row = current;
      find_col = E.cx;
      break;
    }
  }
}

/* prompt stays clean; the cur/total counter shows on the bar after a hit */
void editorFindStatus(char *query) {
  (void)query;
}

/* count find_q matches; *cur is the current hit's 1-based index */
int editorFindCount(int *cur) {
  int total = 0, before = 0, y;
  *cur = 0;
  if (!find_on || !find_q[0] || E.numrows == 0) return 0;
  size_t qlen = strlen(find_q);
  for (y = 0; y < E.numrows; y++) {
    erow *row = &E.row[y];
    char *p = row->chars;
    char *m;
    while ((m = editorIstrstr(p, find_q)) != NULL) {
      total++;
      if (y < find_row || (y == find_row && (m - row->chars) < find_col)) before++;
      p = m + qlen;
    }
  }
  if (find_row >= 0) *cur = before + 1;
  return total;
}

void editorFind(void) {
  int saved_cx = E.cx;
  int saved_cy = E.cy;
  int saved_coloff = E.coloff;
  int saved_rowoff = E.rowoff;
  char *query = editorPrompt("Find: %s", editorFindCallback, editorFindStatus);
  if (query) {
    /* enter: keep the search live and highlighted */
    find_on = 1;
    find_time = editorNowMs(); /* the counter is a toast: it starts now */
    free(query);
  } else {
    /* Esc/cancel: clear the search and its highlights. */
    find_on = 0;
    find_q[0] = '\0';
    find_row = -1;
    E.cx = saved_cx;
    E.cy = saved_cy;
    E.coloff = saved_coloff;
    E.rowoff = saved_rowoff;
  }
}

/* step to the next (dir>0) or previous match, wrapping */
int editorFindNextMatch(int dir) {
  if (!find_on || !find_q[0] || E.numrows == 0) return 0;
  find_time = editorNowMs(); /* stepping through hits re-ups the counter */
  size_t qlen = strlen(find_q);
  int fy = -1, fcol = -1;   /* first match (wrap target) */
  int ly = -1, lcol = -1;   /* last match  (wrap target) */
  int ay = -1, acol = -1;   /* first hit strictly after the current one */
  int by = -1, bcol = -1;   /* last hit strictly before the current one */
  int y;
  for (y = 0; y < E.numrows; y++) {
    erow *row = &E.row[y];
    char *p = row->chars;
    char *m;
    while ((m = editorIstrstr(p, find_q)) != NULL) {
      int col = (int)(m - row->chars);
      if (fy < 0) { fy = y; fcol = col; }
      ly = y; lcol = col;
      if (y > find_row || (y == find_row && col > find_col)) {
        if (ay < 0) { ay = y; acol = col; }
      }
      if (y < find_row || (y == find_row && col < find_col)) {
        by = y; bcol = col;
      }
      p = m + qlen;
    }
  }
  int ny, nx;
  if (dir > 0) {
    if (ay >= 0) { ny = ay; nx = acol; }
    else if (fy >= 0) { ny = fy; nx = fcol; }   /* wrapped */
    else return 0;
  } else {
    if (by >= 0) { ny = by; nx = bcol; }
    else if (ly >= 0) { ny = ly; nx = lcol; }   /* wrapped */
    else return 0;
  }
  find_row = ny;
  find_col = nx;
  E.cy = ny;
  E.cx = nx;
  editorScroll();
  editorRefreshScreen();
  return 1;
}

/*** replace ***/

/* next match at or after (srow,scol), no wrapping; 1 on match */
int editorFindNextReplace(char *query, int srow, int scol, int *fr, int *fc) {
  int qlen = (int)strlen(query);
  int y, c = scol;
  if (qlen == 0 || srow < 0) return 0;
  for (y = srow; y < E.numrows; y++, c = 0) {
    erow *row = &E.row[y];
    int cc;
    for (cc = c; cc + qlen <= row->size; cc++) {
      if (editorIcmp(row->chars + cc, query, qlen)) {
        *fr = y;
        *fc = cc;
        return 1;
      }
    }
  }
  return 0;
}

/* prompt find and replace, confirm each, one undo step */
void editorReplace(void) {
  int scx = E.cx, scy = E.cy;
  char *find = editorPrompt("Replace find: %s", editorFindCallback, editorFindStatus);
  find_on = 0;
  if (!find) { E.cx = scx; E.cy = scy; return; } /* esc */
  if (!find[0]) { free(find); editorSetStatusMessage(""); return; }

  prompt_empty_ok = 1;
  char *repl = editorPrompt("Replace: %s", NULL, NULL);
  prompt_empty_ok = 0;
  if (!repl) {
    E.cx = scx; E.cy = scy;
    editorSetStatusMessage("Replace aborted");
    free(find);
    return;
  }

  int qlen = (int)strlen(find);
  int rlen = (int)strlen(repl);
  int replace_all = 0;
  int replaced = 0;
  int begun = 0;
  int row = E.cy, col = E.cx;

  for (;;) {
    int fr, fc;
    if (!editorFindNextReplace(find, row, col, &fr, &fc)) {
      if (replaced == 0)
        editorSetStatusMessage("Replace: no matches found");
      else
        editorSetStatusMessage("Replace: %d occurrence(s) swapped", replaced);
      break;
    }
    E.cy = fr;
    E.cx = fc;
    if (!replace_all) {
      int k;
      editorSetStatusMessage("Replace? (y)es (n)o (a)ll (r)est (esc): ");
      do {
        editorRefreshScreen();
        k = editorReadKey();
        if (k == PASTE_KEY) editorSkipPaste();
      } while (k == RESIZE_KEY || k == MOUSE_KEY || k == NOKEY || k == PASTE_KEY);
      if (k == 'a' || k == 'A' || k == 'r' || k == 'R') {
        replace_all = 1; /* fall through to the replacement below */
      } else if (k == 'n' || k == 'N') {
        row = fr;
        col = fc + qlen; /* skip past this occurrence */
        continue;
      } else if (k != 'y' && k != 'Y') {
        editorSetStatusMessage("Replace canceled");
        break;
      }
    }

    /* swap the match for the replacement */
    if (!begun) {
      editorUndoBegin(0, E.numrows);
      begun = 1;
    }
    editorRowDeleteRange(&E.row[fr], fc, fc + qlen);
    editorRowInsertBytes(&E.row[fr], fc, repl, rlen);
    /* keep searching just past the text we just wrote */
    row = fr;
    col = fc + rlen;
    E.cx = col;
    replaced++;
  }

  if (begun) editorUndoCommit();
  free(find);
  free(repl);
  editorClampCursor();
  editorRefreshScreen();
}

/*** goto ***/

/* ctrl-g: N goes to line N, +N/-N relative, N:C picks a column */
void editorGoto(void) {
  char *q = editorPrompt("Goto: %s", NULL, NULL);
  if (!q) return;
  char *end;
  long n = strtol(q, &end, 10);
  if (end == q) {
    editorSetStatusMessage("Not a line number");
    free(q);
    return;
  }
  int col = 0;
  if (*end == ':') col = atoi(end + 1);
  if (q[0] == '+' || q[0] == '-') n = E.cy + 1 + n; /* relative to here */
  free(q);
  if (E.numrows == 0) return;
  if (n < 1) n = 1;
  if (n > E.numrows) n = E.numrows;
  E.cy = (int)n - 1;
  E.cx = col > 1 ? editorRowRxToCx(&E.row[E.cy], col - 1) : 0;
  E.rowoff = E.cy - E.screenrows / 2; /* put the line mid-screen... */
  if (E.rowoff > E.numrows + 1 - E.screenrows) /* ...but not past the end */
    E.rowoff = E.numrows + 1 - E.screenrows;
  if (E.rowoff < 0) E.rowoff = 0;
  editorSetStatusMessage("Line %ld of %d", n, E.numrows);
}

/*** append buffer ***/

struct abuf {
  char *b;
  int len;
  int cap;
};

#define ABUF_INIT {NULL, 0, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  if (ab->len + len > ab->cap) {
    int cap = ab->cap ? ab->cap * 2 : 4096;
    while (cap < ab->len + len) cap *= 2;
    ab->b = xrealloc(ab->b, cap);
    ab->cap = cap;
  }
  if (len > 0) memcpy(&ab->b[ab->len], s, len);
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

/*** output ***/

int editorGutterWidth(void) {
  if (!E.ruler) return 0;
  int digits = 1; /* how many digits the line count needs */
  int n = E.numrows;
  while (n >= 10) {
    n /= 10;
    digits++;
  }
  int w = E.ruler_width > 0 ? E.ruler_width : digits;
  if (w < digits) w = digits; /* a fixed width must still fit the numbers */
  if (w > 64) w = 64;
  if (w + 2 > E.screencols) w = E.screencols - 2;
  if (w < 1) return 0; /* no room for a gutter at all */
  return w + 1; /* number field + one separator space */
}

void editorScroll(void) {
  int width = E.screencols - E.gutter;
  if (width < 1) width = 1; /* keep at least one text column */

  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  /* reserve a column each side for the < and > marks */
  int inner = width > 2 ? width - 2 : 1;
  if (E.rx < E.coloff + 1) {
    E.coloff = E.rx > 0 ? E.rx - 1 : 0;
  }
  if (E.rx > E.coloff + inner) {
    E.coloff = E.rx - inner;
  }
  if (E.coloff < 0) E.coloff = 0;
}

/*** syntax highlighting ***/

/* static highlighting by file name or #! line; row state carried down */

enum { CL_PLAIN = 0, CL_COMMENT, CL_KW1, CL_KW2, CL_STR, CL_NUM, CL_PRE,
       CL_FUNC, CL_COUNT };

static const char *cl_esc[CL_COUNT] = {
  "",                           /* plain: the text color */
  "\x1b[38;2;152;160;174m",   /* comment: light black #98A0AE */
  "\x1b[38;2;165;138;168m",   /* keyword: magenta #A58AA8 */
  "\x1b[38;2;128;157;158m",   /* type / builtin / variable: cyan #809D9E */
  "\x1b[38;2;141;168;130m",   /* string: green #8DA882 */
  "\x1b[38;2;224;165;104m",   /* number: yellow #E0A568 */
  "\x1b[38;2;227;123;102m",   /* preprocessor / directive: red #E37B66 */
  "\x1b[38;2;137;153;170m"    /* function / label: blue #8999AA */
};

enum { ST_NONE = 0, ST_BLOCK, ST_TDQ, ST_TSQ, ST_BT };

#define SF_NUM     (1 << 0)  /* highlight numbers */
#define SF_DQ      (1 << 1)  /* "strings" */
#define SF_SQ      (1 << 2)  /* 'strings' */
#define SF_CHAR    (1 << 3)  /* 'c' char literals only */
#define SF_BT      (1 << 4)  /* `multi-line strings` */
#define SF_TRIPLE  (1 << 5)  /* """python""" and '''strings''' */
#define SF_PRE     (1 << 6)  /* #include-style directives */
#define SF_ICASE   (1 << 7)  /* keywords match in any case */
#define SF_HASHWS  (1 << 8)  /* a # comment must start a word */
#define SF_DOLLAR  (1 << 9)  /* $var and ${var} */
#define SF_MAKEVAR (1 << 10) /* $(var) runs to its ) */
#define SF_FUNC    (1 << 11) /* name( and name!( are calls */
#define SF_DOTDIR  (1 << 12) /* .directive and .label: */
#define SF_PCT     (1 << 13) /* %reg and %macro */
#define SF_LABEL   (1 << 14) /* label: at the start of a line */
#define SF_SQRAW   (1 << 15) /* no backslash escapes in 'strings' */
#define SF_SQB     (1 << 16) /* a ' only opens a string at a word start */

struct syn {
  const char *name;
  const char *match; /* space-separated ".ext" suffixes and exact file names */
  const char *kw1, *kw2, *kw3;
  const char *lc, *lc2; /* line comment starters */
  const char *bs, *be;  /* block comment start and end */
  int flags;
};

static const struct syn syntaxes[] = {
  { "c", ".c .h .cc .cpp .cxx .hpp .hh .ino",
    "auto break case const continue default do else enum extern for goto if "
    "inline register restrict return sizeof static struct switch typedef "
    "union volatile while _Alignas _Alignof _Atomic _Bool _Generic _Noreturn "
    "_Static_assert _Thread_local class namespace template typename public "
    "private protected virtual new delete this try catch throw using operator "
    "constexpr nullptr override final noexcept explicit mutable friend "
    "static_assert true false NULL",
    "int long short char float double void unsigned signed bool size_t "
    "ssize_t off_t pid_t int8_t int16_t int32_t int64_t uint8_t uint16_t "
    "uint32_t uint64_t uintptr_t intptr_t ptrdiff_t FILE wchar_t string "
    "vector map",
    NULL, "//", NULL, "/*", "*/",
    SF_NUM | SF_DQ | SF_CHAR | SF_PRE | SF_FUNC },

  { "python", ".py .pyw .pyi",
    "False None True and as assert async await break class continue def del "
    "elif else except finally for from global if import in is lambda "
    "nonlocal not or pass raise return try while with yield match case",
    "int float str bool list dict set tuple bytes object type self cls print "
    "len range open super isinstance enumerate zip map filter",
    NULL, "#", NULL, NULL, NULL,
    SF_NUM | SF_DQ | SF_SQ | SF_TRIPLE | SF_FUNC },

  { "javascript", ".js .mjs .cjs .jsx .ts .tsx",
    "async await break case catch class const continue debugger default "
    "delete do else export extends finally for function if import in "
    "instanceof let new of return static super switch this throw try typeof "
    "var void while with yield from as get set interface type enum "
    "implements namespace declare readonly abstract public private "
    "protected null undefined true false NaN",
    "string number boolean any unknown never object Array Map Set Promise "
    "Object console JSON Math Date Error Symbol",
    NULL, "//", NULL, "/*", "*/",
    SF_NUM | SF_DQ | SF_SQ | SF_BT | SF_FUNC },

  { "go", ".go",
    "break case chan const continue default defer else fallthrough for func "
    "go goto if import interface map package range return select struct "
    "switch type var true false nil iota",
    "bool byte complex64 complex128 error float32 float64 int int8 int16 "
    "int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr any "
    "make new len cap append copy delete panic recover print println",
    NULL, "//", NULL, "/*", "*/",
    SF_NUM | SF_DQ | SF_CHAR | SF_BT | SF_FUNC },

  { "rust", ".rs",
    "as async await break const continue crate dyn else enum extern false fn "
    "for if impl in let loop match mod move mut pub ref return self Self "
    "static struct super trait true type unsafe use where while",
    "bool char f32 f64 i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize "
    "str String Vec Option Result Some None Ok Err Box Rc Arc",
    NULL, "//", NULL, "/*", "*/",
    SF_NUM | SF_DQ | SF_CHAR | SF_FUNC },

  { "shell", ".sh .bash .zsh .ksh .bashrc .zshrc .profile .bash_profile "
    "PKGBUILD",
    "if then else elif fi case esac for while until do done in function "
    "select time return break continue exit local export readonly declare "
    "unset shift source alias set",
    "echo printf cd pwd test read eval exec trap true false",
    NULL, "#", NULL, NULL, NULL,
    SF_DQ | SF_SQ | SF_SQRAW | SF_DOLLAR | SF_HASHWS },

  { "make", "Makefile makefile GNUmakefile .mk",
    "ifeq ifneq ifdef ifndef else endif include sinclude define endef "
    "export unexport override vpath",
    NULL, NULL, "#", NULL, NULL, NULL,
    SF_DQ | SF_DOLLAR | SF_MAKEVAR | SF_HASHWS },

  { "json", ".json .jsonc .webmanifest",
    NULL, "true false null", NULL, NULL, NULL, NULL, NULL,
    SF_NUM | SF_DQ },

  { "conf", ".toml .yaml .yml .ini .conf .cfg .service .desktop .env",
    NULL, "true false null yes no on off True False", NULL,
    "#", NULL, NULL, NULL,
    SF_NUM | SF_DQ | SF_SQ | SF_SQB | SF_HASHWS },

  { "kdl", ".kdl",
    NULL, /* no reserved words: node names are bare identifiers */
    "true false null inf nan", /* literals (also #true/#false/#null/#inf/#nan in v2) */
    NULL, "//", NULL, "/*", "*/",
    SF_NUM | SF_DQ },

  { "asm", ".asm .s .S .nasm .inc",
    "mov movzx movsx movsxd movabs lea push pop pushf popf pushq popq call "
    "ret retq callq jmp jmpq je jne jz jnz ja jae jb jbe jg jge jl jle js "
    "jns jo jno jc jnc jp jnp jcxz jecxz jrcxz loop loope loopne cmp test "
    "add sub mul imul div idiv inc dec neg not and or xor shl shr sal sar "
    "rol ror rcl rcr adc sbb xchg cmove cmovne cmovz cmovnz cmova cmovae "
    "cmovb cmovbe cmovg cmovge cmovl cmovle cmovs cmovns sete setne setz "
    "setnz seta setae setb setbe setg setge setl setle nop hlt syscall "
    "sysret sysenter int int3 iret iretq leave enter cld std cli sti rep "
    "repe repne repz repnz lock movsb movsw movsd movsq stosb stosw stosd "
    "stosq lodsb lodsw lodsd lodsq scasb scasw scasd scasq cmpsb cmpsw "
    "cmpsd cmpsq cpuid rdtsc rdmsr wrmsr xadd cmpxchg bsf bsr bt bts btr "
    "btc popcnt lzcnt tzcnt bswap cdq cqo cwd cbw cwde cdqe sahf lahf in "
    "out movaps movups movdqa movdqu movq movd movss movsd addps addss "
    "addpd addsd subps subss mulps mulss divps divss sqrtss pxor paddd "
    "paddq psubd pshufb pcmpeqb pmovmskb vmovdqa vmovdqu vpxor movl movb "
    "movw addl addq subl subq cmpl cmpq xorl xorq andl andq orl orq testl "
    "testq shll shlq shrl shrq leaq incl incq decl decq negq notq imulq "
    "ud2 pause mfence lfence sfence",
    "rax rbx rcx rdx rsi rdi rbp rsp rip r8 r9 r10 r11 r12 r13 r14 r15 "
    "r8d r9d r10d r11d r12d r13d r14d r15d r8w r9w r10w r11w r12w r13w r14w "
    "r15w r8b r9b r10b r11b r12b r13b r14b r15b eax ebx ecx edx esi edi ebp "
    "esp eip ax bx cx dx si di bp sp al bl cl dl ah bh ch dh sil dil bpl "
    "spl cs ds es fs gs ss cr0 cr2 cr3 cr4 xmm0 xmm1 xmm2 xmm3 xmm4 xmm5 "
    "xmm6 xmm7 xmm8 xmm9 xmm10 xmm11 xmm12 xmm13 xmm14 xmm15 ymm0 ymm1 "
    "ymm2 ymm3 ymm4 ymm5 ymm6 ymm7 st0 st1 st2 st3 st4 st5 st6 st7 "
    "byte word dword qword tword oword yword ptr offset short near far "
    "rel abs",
    "section segment global globl extern db dw dd dq dt resb resw resd resq "
    "equ times bits default align org use16 use32 use64 absolute struc "
    "endstruc istruc iend incbin .text .data .bss .rodata .globl .global "
    ".section .byte .word .long .quad .ascii .asciz .string .align .balign "
    ".p2align .equ .set .type .size .file .ident .intel_syntax .att_syntax "
    ".zero .space .skip .rept .endr .macro .endm .include .org .code16 "
    ".code32 .code64 .cfi_startproc .cfi_endproc .weak .hidden .comm",
    ";", "#", "/*", "*/",
    SF_NUM | SF_DQ | SF_SQ | SF_ICASE | SF_DOTDIR | SF_PCT | SF_LABEL },
};

static const struct syn *syn_cur;   /* the language of the file, or NULL */
static unsigned char *hl_buf;       /* class of each byte of the row being drawn */
static int hl_cap;
static const unsigned char *hl_cls; /* hl_buf while a row is drawn, else NULL */

static int synWordIn(const char *list, const char *w, int wl) {
  const char *p = list;
  if (!list) return 0;
  while (*p) {
    const char *e;
    while (*p == ' ') p++;
    e = p;
    while (*e && *e != ' ') e++;
    if ((int)(e - p) == wl && memcmp(p, w, (size_t)wl) == 0) return 1;
    p = e;
  }
  return 0;
}

/* does base match a ".ext" suffix or a whole name? */
static int synMatches(const char *match, const char *base) {
  size_t bl = strlen(base);
  const char *p = match;
  while (*p) {
    const char *e;
    size_t tl;
    while (*p == ' ') p++;
    e = p;
    while (*e && *e != ' ') e++;
    tl = (size_t)(e - p);
    if (tl > 0) {
      if (tl == bl && memcmp(p, base, tl) == 0) return 1;
      if (p[0] == '.' && bl > tl && memcmp(base + bl - tl, p, tl) == 0) return 1;
    }
    p = e;
  }
  return 0;
}

static const struct syn *synByName(const char *name) {
  size_t i;
  for (i = 0; i < sizeof(syntaxes) / sizeof(syntaxes[0]); i++)
    if (!strcmp(syntaxes[i].name, name)) return &syntaxes[i];
  return NULL;
}

static const struct syn *synFor(void) {
  size_t i;
  if (E.filename) {
    const char *slash = strrchr(E.filename, '/');
    const char *base = slash ? slash + 1 : E.filename;
    for (i = 0; i < sizeof(syntaxes) / sizeof(syntaxes[0]); i++)
      if (synMatches(syntaxes[i].match, base)) return &syntaxes[i];
  }
  /* no telling name: a #! line on the first row may say */
  if (E.numrows > 0 && E.row[0].size > 2 &&
      E.row[0].chars[0] == '#' && E.row[0].chars[1] == '!') {
    char line[128];
    int n = E.row[0].size < 127 ? E.row[0].size : 127;
    memcpy(line, E.row[0].chars, (size_t)n);
    line[n] = '\0';
    if (strstr(line, "python")) return synByName("python");
    if (strstr(line, "node") || strstr(line, "deno")) return synByName("javascript");
    if (strstr(line, "sh")) return synByName("shell");
  }
  return NULL;
}

static int synStarts(const char *p, int n, int i, const char *s) {
  int l = (int)strlen(s);
  return l > 0 && i + l <= n && memcmp(p + i, s, (size_t)l) == 0;
}

static int synIdent(int c) { return isalnum(c) || c == '_' || c >= 0x80; }

/* where the text after `term` starts, from i; -1 if absent */
static int synFindEnd(const char *p, int n, int i, const char *term, int esc) {
  int tl = (int)strlen(term);
  for (; i < n; i++) {
    if (esc && p[i] == '\\') { i++; continue; }
    if (i + tl <= n && memcmp(p + i, term, (size_t)tl) == 0) return i + tl;
  }
  return -1;
}

static void synMark(unsigned char *out, int from, int to, int cls) {
  if (out && to > from) memset(out + from, cls, (size_t)(to - from));
}

/* Only blanks before byte i? */
static int synAtLineStart(const char *p, int i) {
  int k;
  for (k = 0; k < i; k++)
    if (p[k] != ' ' && p[k] != '\t') return 0;
  return 1;
}

/* classify one row; *state carries open comment/string to the next */
static void synRow(const struct syn *s, const erow *row, int *state,
                   unsigned char *out) {
  const char *p = row->chars;
  int n = row->size, i = 0, st = *state, f = s->flags;

  if (out) memset(out, CL_PLAIN, (size_t)n);

  if (st != ST_NONE) { /* still inside what an earlier row opened */
    const char *term = st == ST_BLOCK ? s->be : st == ST_TDQ ? "\"\"\"" :
                       st == ST_TSQ ? "'''" : "`";
    int cls = st == ST_BLOCK ? CL_COMMENT : CL_STR;
    int e = synFindEnd(p, n, 0, term, st != ST_BLOCK);
    if (e < 0) { synMark(out, 0, n, cls); return; }
    synMark(out, 0, e, cls);
    i = e;
    st = ST_NONE;
  }

  while (i < n) {
    unsigned char c = (unsigned char)p[i];

    /* comments */
    if ((s->lc && synStarts(p, n, i, s->lc)) ||
        (s->lc2 && synStarts(p, n, i, s->lc2))) {
      if (!(f & SF_HASHWS) || i == 0 || p[i - 1] == ' ' || p[i - 1] == '\t' ||
          p[i - 1] == ';' || p[i - 1] == '&' || p[i - 1] == '|' ||
          p[i - 1] == '(' || p[i - 1] == ')') {
        synMark(out, i, n, CL_COMMENT);
        break;
      }
    }
    if (s->bs && synStarts(p, n, i, s->bs)) {
      int e = synFindEnd(p, n, i + (int)strlen(s->bs), s->be, 0);
      if (e < 0) { synMark(out, i, n, CL_COMMENT); st = ST_BLOCK; break; }
      synMark(out, i, e, CL_COMMENT);
      i = e;
      continue;
    }

    /* #include and friends */
    if ((f & SF_PRE) && c == '#' && synAtLineStart(p, i)) {
      int j = i + 1, ws, wl;
      while (j < n && (p[j] == ' ' || p[j] == '\t')) j++;
      ws = j;
      while (j < n && synIdent((unsigned char)p[j])) j++;
      wl = j - ws;
      synMark(out, i, j, CL_PRE);
      i = j;
      if ((wl == 7 && !memcmp(p + ws, "include", 7)) ||
          (wl == 12 && !memcmp(p + ws, "include_next", 12))) {
        int k = i;
        while (k < n && (p[k] == ' ' || p[k] == '\t')) k++;
        if (k < n && p[k] == '<') {
          int e = k + 1;
          while (e < n && p[e] != '>') e++;
          if (e < n) e++;
          synMark(out, k, e, CL_STR);
          i = e;
        }
      }
      continue;
    }

    /* strings */
    if ((f & SF_TRIPLE) &&
        (synStarts(p, n, i, "\"\"\"") || synStarts(p, n, i, "'''"))) {
      int dq = c == '"';
      int e = synFindEnd(p, n, i + 3, dq ? "\"\"\"" : "'''", 1);
      if (e < 0) { synMark(out, i, n, CL_STR); st = dq ? ST_TDQ : ST_TSQ; break; }
      synMark(out, i, e, CL_STR);
      i = e;
      continue;
    }
    if ((f & SF_DQ) && c == '"') {
      int j = i + 1;
      while (j < n && p[j] != '"') {
        if (p[j] == '\\') j++;
        j++;
      }
      if (j < n) j++; else j = n;
      synMark(out, i, j, CL_STR);
      i = j;
      continue;
    }
    if ((f & SF_BT) && c == '`') {
      int e = synFindEnd(p, n, i + 1, "`", 1);
      if (e < 0) { synMark(out, i, n, CL_STR); st = ST_BT; break; }
      synMark(out, i, e, CL_STR);
      i = e;
      continue;
    }
    if (c == '\'') {
      if (f & SF_CHAR) {
        int j = i + 1;
        if (j < n && p[j] == '\\') {
          j += 2;
          while (j < n && p[j] != '\'' && j < i + 12) j++;
        } else if (j < n) {
          int l;
          utf8Decode((const unsigned char *)p + j, n - j, &l);
          j += l;
        }
        if (j < n && p[j] == '\'') {
          synMark(out, i, j + 1, CL_STR);
          i = j + 1;
          continue;
        }
        /* no closing quote nearby: a lifetime or apostrophe, plain */
      } else if ((f & SF_SQ) &&
                 (!(f & SF_SQB) || i == 0 || !synIdent((unsigned char)p[i - 1]))) {
        int j = i + 1;
        while (j < n && p[j] != '\'') {
          if (!(f & SF_SQRAW) && p[j] == '\\') j++;
          j++;
        }
        if (j < n) j++; else j = n;
        synMark(out, i, j, CL_STR);
        i = j;
        continue;
      }
    }

    /* $var, ${var}, $(var) */
    if ((f & SF_DOLLAR) && c == '$' && i + 1 < n) {
      unsigned char d = (unsigned char)p[i + 1];
      int j = i + 1;
      if (d == '{' || (d == '(' && (f & SF_MAKEVAR))) {
        char close = d == '{' ? '}' : ')';
        while (j < n && p[j] != close) j++;
        if (j < n) j++;
      } else if (d == '(') {
        j = i + 2;
      } else if (isalnum(d) || d == '_') {
        while (j < n && (isalnum((unsigned char)p[j]) || p[j] == '_')) j++;
      } else if (d && strchr("@*#?$!-<^%", d)) {
        j = i + 2;
      }
      if (j > i + 1) {
        synMark(out, i, j, CL_KW2);
        i = j;
        continue;
      }
    }

    /* %rax and %define */
    if ((f & SF_PCT) && c == '%' && i + 1 < n &&
        synIdent((unsigned char)p[i + 1])) {
      char lw[40];
      int j = i + 1, wl, k, cls = CL_PRE;
      while (j < n && synIdent((unsigned char)p[j])) j++;
      wl = j - i - 1;
      if (wl < (int)sizeof(lw)) {
        for (k = 0; k < wl; k++) lw[k] = (char)tolower((unsigned char)p[i + 1 + k]);
        if (synWordIn(s->kw2, lw, wl)) cls = CL_KW2;
      }
      synMark(out, i, j, cls);
      i = j;
      continue;
    }

    /* .globl, .text, .loop: */
    if ((f & SF_DOTDIR) && c == '.' && i + 1 < n && isalpha((unsigned char)p[i + 1]) &&
        (i == 0 || !synIdent((unsigned char)p[i - 1]))) {
      char lw[40];
      int j = i + 1, wl, k, cls = CL_PLAIN;
      while (j < n && (synIdent((unsigned char)p[j]) || p[j] == '.')) j++;
      wl = j - i;
      if (j < n && p[j] == ':' && synAtLineStart(p, i)) cls = CL_FUNC;
      else if (wl < (int)sizeof(lw)) {
        for (k = 0; k < wl; k++) lw[k] = (char)tolower((unsigned char)p[i + k]);
        if (synWordIn(s->kw3, lw, wl)) cls = CL_PRE;
        else if (wl > 2 && p[i + 1] == 'L') cls = CL_FUNC; /* .L1 local label */
      }
      synMark(out, i, j, cls);
      i = j;
      continue;
    }

    /* numbers */
    if ((f & SF_NUM) && isdigit(c) && (i == 0 || !synIdent((unsigned char)p[i - 1]))) {
      int j = i;
      while (j < n) {
        if (isalnum((unsigned char)p[j]) || p[j] == '_') j++;
        else if (p[j] == '.' && j + 1 < n && isdigit((unsigned char)p[j + 1])) j++;
        else break;
      }
      synMark(out, i, j, CL_NUM);
      i = j;
      continue;
    }

    /* words */
    if (isalpha(c) || c == '_' || c >= 0x80) {
      char lw[40];
      const char *w = p + i;
      int j = i, wl, cls = CL_PLAIN;
      while (j < n && synIdent((unsigned char)p[j])) j++;
      wl = j - i;
      if ((f & SF_ICASE) && wl < (int)sizeof(lw)) {
        int k;
        for (k = 0; k < wl; k++) lw[k] = (char)tolower((unsigned char)p[i + k]);
        w = lw;
      }
      if (synWordIn(s->kw1, w, wl)) cls = CL_KW1;
      else if (synWordIn(s->kw2, w, wl)) cls = CL_KW2;
      else if (synWordIn(s->kw3, w, wl)) cls = CL_PRE;
      else if ((f & SF_FUNC) && j < n &&
               (p[j] == '(' || (p[j] == '!' && j + 1 < n &&
                                (p[j + 1] == '(' || p[j + 1] == '[' || p[j + 1] == '{'))))
        cls = CL_FUNC;
      else if ((f & SF_LABEL) && j < n && p[j] == ':' && synAtLineStart(p, i))
        cls = CL_FUNC;
      synMark(out, i, j, cls);
      i = j;
      continue;
    }

    i++;
  }
  *state = st;
}

/* switch foreground to class `want`; plain uses the text color */
static void editorHlSwitch(struct abuf *ab, int *cur, int want) {
  if (*cur == want) return;
  if (want == CL_PLAIN) {
    abAppend(ab, "\x1b[39m", 5);
    abAppend(ab, E.col_str[COL_TEXT_FG], (int)strlen(E.col_str[COL_TEXT_FG]));
  } else {
    abAppend(ab, cl_esc[want], (int)strlen(cl_esc[want]));
  }
  *cur = want;
}

/* paint one cell as a reverse-video block caret */
void editorDrawCursorCell(struct abuf *ab, char ch) {
  abAppend(ab, "\x1b[7m", 4);
  abAppend(ab, &ch, 1);
  abAppend(ab, "\x1b[27m", 5);
}

/* < and > edge marks: each takes one column of `avail` */
static void editorDrawScrollMark(struct abuf *ab, char mark, int covered) {
  if (covered) {
    /* let the cursor cover the mark instead of reversing it */
    return;
  }
  abAppend(ab, E.col_str[COL_SCROLLMARK_BG],
           (int)strlen(E.col_str[COL_SCROLLMARK_BG]));
  abAppend(ab, E.col_str[COL_SCROLLMARK_FG],
           (int)strlen(E.col_str[COL_SCROLLMARK_FG]));
  abAppend(ab, &mark, 1);
  abAppend(ab, "\x1b[0m", 4);
  abAppend(ab, E.col_str[COL_TEXT_FG], (int)strlen(E.col_str[COL_TEXT_FG]));
}

static void editorDrawRowText(struct abuf *ab, erow *row, int filerow, int avail) {
  int sel_from = 0, sel_to = 0, sel_eol = 0;
  int sel_on = editorRowSelection(row, filerow, &sel_from, &sel_to);
  if (sel_on) {
    int sy, sx, ey, ex;
    editorSelectionRange(&sy, &sx, &ey, &ex);
    /* selection continues below: its newline is selected too */
    sel_eol = filerow < ey;
  }
  int block = filerow == E.cy && !E.in_prompt && E.cursor_style == 2;
  int qlen = (find_on && find_q[0]) ? (int)strlen(find_q) : 0;
  char *m = qlen ? editorIstrstr(row->chars, find_q) : NULL;

  int roww = row->size > 0 ? editorRowCxToRx(row, row->size) : 0;
  int has_right = avail > 0 && roww > E.coloff + avail;
  int has_left = E.coloff > 0 && roww > 0;
  int left = E.coloff + (has_left ? 1 : 0);   /* first column of real text */
  int right = E.coloff + avail - (has_right ? 1 : 0); /* one past the last */
  if (right < left) right = left;
  int col = 0, b = 0;
  int cur_cls = CL_PLAIN;
  const unsigned char *cls = hl_cls;
  int hit_right = 0; /* does the selection/cursor reach the '>' mark's cell? */
  int firstvis_cx = editorRowRxToCx(row, E.coloff); /* byte of first shown char */

  if (has_left) editorDrawScrollMark(ab, '<',
    (filerow == E.cy && E.rx == E.coloff) ||
    (sel_on && sel_from <= firstvis_cx && sel_to > firstvis_cx));

  while (b < row->size) {
    int e = editorCharEnd(row, b);
    int w = editorCellWidth(row, b, col);
    if (col >= right) break;
    if (col + w > left) { /* at least part of it is on screen */
      int in_sel, on_cur, in_match, rev;
      while (m && (m - row->chars) + qlen <= b)
        m = editorIstrstr(m + qlen, find_q);
      in_sel = sel_on && b >= sel_from && b < sel_to;
      on_cur = block && b == E.cx;
      in_match = qlen && m && b >= (m - row->chars) &&
                 b < (m - row->chars) + qlen;
      rev = in_sel || on_cur; /* selection/cursor: reverse block */
      int hl = in_match && !rev; /* find hit: yellow bg (selection wins) */
      if (rev && col + w >= right) hit_right = 1; /* cursor/selection on the '> cell */

      editorHlSwitch(ab, &cur_cls,
                     (!rev && !in_match && cls && b < row->size) ? cls[b] : CL_PLAIN);
      if (hl && E.col_str[COL_FIND][0])
        abAppend(ab, E.col_str[COL_FIND], (int)strlen(E.col_str[COL_FIND]));
      if (rev) abAppend(ab, "\x1b[7m", 4);

      if (row->chars[b] == '\t' || col < left || col + w > right) {
        /* paint a cut tab or wide char as spaces over the visible cells */
        int k;
        for (k = 0; k < w; k++)
          if (col + k >= left && col + k < right) abAppend(ab, " ", 1);
      } else if ((unsigned char)row->chars[b] < 32 ||
                 (unsigned char)row->chars[b] == 127) {
        abAppend(ab, "?", 1); /* a control byte must not reach the terminal */
      } else {
        int len, cp = utf8Decode((const unsigned char *)row->chars + b,
                                 row->size - b, &len);
        if (cp >= 0x80 && cp < 0xA0)
          abAppend(ab, "?", 1); /* C1 controls: some terminals obey them */
        else if (cp == 0xFFFD && len == 1)
          abAppend(ab, "\xEF\xBF\xBD", 3); /* not valid UTF-8: show the replacement mark */
        else
          abAppend(ab, row->chars + b, e - b);
      }
      if (rev) abAppend(ab, "\x1b[27m", 5); /* reverse: reverse off */
      if (hl) abAppend(ab, "\x1b[49m", 5); /* back to the terminal's own bg */
    }
    col += w;
    b = e;
  }
  editorHlSwitch(ab, &cur_cls, CL_PLAIN);
  /* cell past the last char: block caret or selected-newline stand-in */
  if ((sel_eol || (block && E.cx >= row->size)) && col >= left && col < right)
    editorDrawCursorCell(ab, ' ');

  if (has_right) editorDrawScrollMark(ab, '>',
    hit_right || ((sel_eol || (block && E.cx >= row->size)) && col >= right - 1 && col < right));
}

void editorDrawRows(struct abuf *ab) {
  static erow blank = { 0, "" };
  int y;
  int avail = E.screencols - E.gutter;
  if (avail < 0) avail = 0;
  int nvis = E.numrows > 0 ? E.numrows : 1; /* an empty buffer still shows line 1 */
  int hstate = ST_NONE;
  syn_cur = synFor();
  if (syn_cur && (syn_cur->bs || (syn_cur->flags & (SF_TRIPLE | SF_BT)))) {
    /* state left open by the rows above the screen */
    int r, from = E.rowoff > 3000 ? E.rowoff - 3000 : 0;
    for (r = from; r < E.rowoff && r < E.numrows; r++)
      synRow(syn_cur, &E.row[r], &hstate, NULL);
  }
  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if (filerow > nvis) {
      abAppend(ab, "\x1b[K", 3);
      abAppend(ab, "\r\n", 2);
      continue;
    }
    if (filerow == nvis) { /* the ~ fringe: the one row past the end */
      abAppend(ab, E.col_str[COL_TILDE_FG],
               (int)strlen(E.col_str[COL_TILDE_FG]));
      abAppend(ab, "~", 1);
      abAppend(ab, "\x1b[0m", 4);
      abAppend(ab, "\x1b[K", 3);
      abAppend(ab, "\r\n", 2);
      continue;
    }
    if (E.gutter > 0) {
      char num[80];
      int n = snprintf(num, sizeof(num), "%*d", E.gutter - 1, filerow + 1);
      int cur = filerow == E.cy;
      const char *bg = cur ? E.col_str[COL_GUTTER_CURSOR_BG] : E.col_str[COL_GUTTER_BG];
      const char *fg = cur ? E.col_str[COL_GUTTER_CURSOR_FG] : E.col_str[COL_GUTTER_FG];
      if (bg[0]) abAppend(ab, bg, (int)strlen(bg)); /* unset: use the terminal's own background */
      abAppend(ab, fg, (int)strlen(fg));
      abAppend(ab, num, n);
      abAppend(ab, " ", 1); /* the separator column too, so the panel reads as one */
      abAppend(ab, "\x1b[0m", 4);
    }
    /* text color set once; cursor/selection reverse it; finds use yellow */
    abAppend(ab, E.col_str[COL_TEXT_FG], (int)strlen(E.col_str[COL_TEXT_FG]));
    if (syn_cur && filerow < E.numrows) {
      erow *hr = &E.row[filerow];
      if (hr->size + 1 > hl_cap) {
        hl_cap = hr->size + 64;
        hl_buf = xrealloc(hl_buf, (size_t)hl_cap);
      }
      synRow(syn_cur, hr, &hstate, hl_buf);
      hl_cls = hl_buf;
    }
    editorDrawRowText(ab, filerow < E.numrows ? &E.row[filerow] : &blank,
                      filerow, avail);
    hl_cls = NULL;
    abAppend(ab, "\x1b[0m", 4);
    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, E.col_str[COL_STATUS_BG], (int)strlen(E.col_str[COL_STATUS_BG]));
  abAppend(ab, E.col_str[COL_STATUS_FG], (int)strlen(E.col_str[COL_STATUS_FG]));
  char name[512], left[1024], right[512];
  char suffix[300]; /* what trails the name: dirty flag and/or a notice */
  suffix[0] = '\0';
  const char *fn = E.filename ? E.filename : "[No Name]";
  int fnlen = (int)strlen(fn);

  int total = E.numrows > 0 ? E.numrows : 1;
  int cur = E.cy + 1 > total ? total : E.cy + 1;
  /* unified bar: prompt, live search, or name + notice + line:col */
  int prompt = E.in_prompt;
  long now = editorNowMs();
  int quit_shown = E.quit_confirm && now - E.quit_time < OSHI_STATUS_MS;
  int find_shown = find_on && find_q[0] && now - find_time < OSHI_STATUS_MS;
  int cfg_shown = E.config_err[0] && now - E.config_err_time < OSHI_STATUS_MS;
  int hasmsg = !quit_shown && !find_shown && !cfg_shown && E.statusmsg[0] &&
               (now - E.statusmsg_time < OSHI_STATUS_MS);
  long notice_t = -1; /* which notice this is, so editorReadKey can lapse it */
  int len = 0, rlen = 0, slen = 0;
  if (prompt) {
    /* keep the name's tail (basename) and leave room for the prompt */
    int cap = E.screencols - 24; /* margin left for the prompt text */
    if (cap > 127) cap = 127;    /* widest UTF-8 spelling of name[], "<" included */
    if (cap < 9) cap = 9;        /* "<" plus a toehold */
    int skip = editorFitTail(fn, fnlen, cap - 1); /* -1: "<" costs a cell too */
    snprintf(name, sizeof(name), "%s%s", skip > 0 ? "<" : "", fn + skip);
    rlen = snprintf(right, sizeof(right), "%s", name);
    len = snprintf(left, sizeof(left), "%s", E.statusmsg);
  } else if (quit_shown) {
    char keyname[16];
    editorKeyName(E.binds[CMD_QUIT], keyname, sizeof(keyname));
    slen = snprintf(suffix, sizeof(suffix), "  Modified press %s again", keyname);
    rlen = snprintf(right, sizeof(right), "%d/%d", cur, E.rx + 1);
    notice_t = E.quit_time;
  } else if (find_shown) {
    int current, found = editorFindCount(&current);
    slen = snprintf(suffix, sizeof(suffix), "%s", E.dirty ? " (modified)" : "");
    rlen = snprintf(right, sizeof(right), "%d/%d", current, found);
    notice_t = find_time;
  } else if (cfg_shown) {
    slen = snprintf(suffix, sizeof(suffix), "  %s", E.config_err);
    rlen = snprintf(right, sizeof(right), "%d/%d", cur, E.rx + 1);
    notice_t = E.config_err_time;
  } else if (hasmsg) {
    slen = snprintf(suffix, sizeof(suffix), "%s  %s", E.dirty ? " (modified)" : "",
                    E.statusmsg);
    rlen = snprintf(right, sizeof(right), "%d/%d", cur, E.rx + 1);
    notice_t = E.statusmsg_time;
  } else {
    slen = snprintf(suffix, sizeof(suffix), "%s", E.dirty ? " (modified)" : "");
    rlen = snprintf(right, sizeof(right), "%d/%d", cur, E.rx + 1);
  }
  if (notice_t >= 0) {
    E.notice_pending = 1;
    E.notice_deadline = notice_t + OSHI_STATUS_MS;
  } else {
    E.notice_pending = 0; /* a prompt, or a plain bar: nothing to lapse */
  }
  /* snprintf reports what it wanted, not what fit */
  if (slen >= (int)sizeof(suffix)) slen = (int)sizeof(suffix) - 1;
  if (len >= (int)sizeof(left)) len = (int)sizeof(left) - 1;
  if (rlen >= (int)sizeof(right)) rlen = (int)sizeof(right) - 1;

  int rw = editorStrWidth(right, rlen);
  if (rw > E.screencols) {
    rlen = editorFitHead(right, rlen, E.screencols);
    rw = editorStrWidth(right, rlen);
  }
  int block = prompt && E.cursor_style == 2;
  int avail = E.screencols - rw - (block ? 1 : 0);
  if (avail < 0) avail = 0;
  if (!prompt) {
    /* name gets the leftover width; truncated from the front with < */
    int budget = avail - editorStrWidth(suffix, slen);
    if (budget < 16) budget = 16;
    if (budget > 127) budget = 127;
    int skip = editorFitTail(fn, fnlen, budget - 1); /* -1: "<" costs a cell too */
    snprintf(name, sizeof(name), "%s%s", skip > 0 ? "<" : "", fn + skip);
    len = snprintf(left, sizeof(left), "%s%s", name, suffix);
    if (len >= (int)sizeof(left)) len = (int)sizeof(left) - 1;
  }
  const char *lp = left;
  if (editorStrWidth(left, len) > avail) {
    if (prompt) { /* keep the end: that's where you are typing */
      int s = editorFitTail(left, len, avail);
      lp = left + s;
      len -= s;
    } else {
      len = editorFitHead(left, len, avail);
    }
  }
  int lw = editorStrWidth(lp, len);
  abAppend(ab, lp, len);
  int used = lw;
  if (block) { /* block caret at end of the prompt text */
    editorDrawCursorCell(ab, ' ');
    used++;
  }
  E.prompt_col = lw + 1;
  for (; used < E.screencols - rw; used++)
    abAppend(ab, " ", 1);
  abAppend(ab, right, rlen);
  abAppend(ab, "\x1b[0m", 4);
}

void editorRefreshScreen(void) {
  E.gutter = editorGutterWidth();
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  if (E.redraw) { /* the window changed shape: start from a clean slate */
    abAppend(&ab, "\x1b[2J", 4);
    E.redraw = 0;
  }
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  /* bar: file name left, notice middle, row/col right */
  editorDrawStatusBar(&ab);

  /* block caret is drawn by editorDrawRows; beam uses the terminal's cursor */
  if (E.cursor_style == 6) {
    char buf[32];
    int row, col;
    if (E.in_prompt) {
      row = E.screenrows + 1;
      col = E.prompt_col;
      if (col > E.screencols) col = E.screencols;
    } else {
      row = E.cy - E.rowoff + 1;
      col = E.rx - E.coloff + E.gutter + 1;
      if (col < 1) col = 1;
    }
    snprintf(buf, sizeof(buf), "\x1b[?25h\x1b[%d;%dH", row, col);
    abAppend(&ab, buf, (int)strlen(buf));
  }

  ssize_t r = write(STDOUT_FILENO, ab.b, ab.len);
  (void)r;
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = editorNowMs();
}

/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int),
                   void (*decorate)(char *)) {
  size_t bufsize = OSHI_PROMPT_MAX + 8;
  char *buf = xmalloc(bufsize);
  size_t buflen = 0;
  buf[0] = '\0';
  E.in_prompt = 1;
  while (1) {
    editorSetStatusMessage(prompt, buf);
    if (decorate) decorate(buf);
    editorRefreshScreen();
    int c = editorReadKey();
    if (c == RESIZE_KEY || c == MOUSE_KEY || c == NOKEY) continue;
    if (c == CTRL_BACKSPACE) {
      /* ctrl-backspace erases a word, else one character */
      size_t p = buflen;
      while (p > 0 && isspace((unsigned char)buf[p - 1])) p--;
      if (p < buflen) {
        while (p > 0 && !isspace((unsigned char)buf[p - 1])) p--;
      } else {
        do { p--; } while (p > 0 && ((unsigned char)buf[p] & 0xC0) == 0x80);
      }
      buflen = p;
      buf[buflen] = '\0';
    } else if (c == DEL_KEY || c == BACKSPACE) {
      if (buflen != 0) { /* a whole character, not one byte of it */
        do {
          buflen--;
        } while (buflen > 0 && ((unsigned char)buf[buflen] & 0xC0) == 0x80);
        buf[buflen] = '\0';
      }
    } else if (c == '\x1b') {
      editorSetStatusMessage("");
      if (callback) callback(buf, c);
      free(buf);
      E.in_prompt = 0;
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0 || prompt_empty_ok) {
        editorSetStatusMessage("");
        if (callback) callback(buf, c);
        E.in_prompt = 0;
        return buf;
      }
    } else if (c == UTF8_KEY) {
      if (buflen + E.utf8len <= OSHI_PROMPT_MAX) {
        memcpy(buf + buflen, E.utf8, E.utf8len);
        buflen += E.utf8len;
        buf[buflen] = '\0';
      }
    } else if (c == PASTE_KEY) { /* the first line of it, without control bytes */
      int n, i;
      char *t = editorReadPasteBuf(&n);
      for (i = 0; i < n && t[i] != '\n' && t[i] != '\r'; i++) {
        unsigned char u = (unsigned char)t[i];
        if (u < 32 || u == 127) continue;
        if (buflen + 1 > OSHI_PROMPT_MAX) break;
        buf[buflen++] = t[i];
      }
      buf[buflen] = '\0';
      free(t);
    } else if (c >= 32 && c < 127) {
      if (buflen + 1 <= OSHI_PROMPT_MAX) {
        buf[buflen++] = c;
        buf[buflen] = '\0';
      }
    }

    if (callback) callback(buf, c);
  }
}

void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
      if (row && E.cx > 0) {
        E.cx = editorPrevChar(row, E.cx);
      } else if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
        E.cx = editorCharEnd(row, E.cx);
      } else if (E.cy + 1 < E.numrows) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if (E.cy > 0) {
        E.cy--;
        E.cx = editorRowRxToCx(&E.row[E.cy], E.goal_rx);
      }
      break;
    case ARROW_DOWN:
      /* the last line is the last stop */
      if (E.cy + 1 < E.numrows) {
        E.cy++;
        E.cx = editorRowRxToCx(&E.row[E.cy], E.goal_rx);
      } else if (row) {
        /* down on the last line goes to end of line */
        E.cx = row->size;
        E.goal_rx = E.rx;
      }
      break;
  }
}

/* ctrl-left/right: previous word start or next word end */
void editorMoveWord(int dir) {
  erow *row = E.cy < E.numrows ? &E.row[E.cy] : NULL;
  int size = row ? row->size : 0;
  int i = E.cx;

  if (dir > 0) {
    if (i >= size) { /* at the end of the line: carry on below */
      if (E.cy + 1 < E.numrows) {
        E.cy++;
        E.cx = 0;
      }
      return;
    }
    while (i < size && !isspace((unsigned char)row->chars[i])) i++;
    while (i < size && isspace((unsigned char)row->chars[i])) i++;
  } else {
    if (i == 0) { /* at the start of the line: back up to the one above */
      if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      return;
    }
    while (i > 0 && isspace((unsigned char)row->chars[i - 1])) i--;
    while (i > 0 && !isspace((unsigned char)row->chars[i - 1])) i--;
  }
  E.cx = i;
}

/* Milliseconds from a clock that never jumps. */
long editorNowMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* the whitespace-delimited word under the cursor */
void editorWordAt(int *from, int *to) {
  erow *row = E.cy < E.numrows ? &E.row[E.cy] : NULL;
  int i, sp, a, b;
  if (!row || row->size == 0) {
    *from = *to = 0;
    return;
  }
  i = E.cx;
  if (i >= row->size) i = row->size - 1; /* clicked past the text: last word */
  sp = isspace((unsigned char)row->chars[i]) ? 1 : 0;
  a = i;
  b = i + 1;
  while (a > 0 && (isspace((unsigned char)row->chars[a - 1]) ? 1 : 0) == sp) a--;
  while (b < row->size && (isspace((unsigned char)row->chars[b]) ? 1 : 0) == sp) b++;
  *from = a;
  *to = b;
}

/* mouse: click places, drag selects, double picks a word, triple a line */
void editorMouse(int button, int release, int x, int y) {
  if (y > E.screenrows) y = E.screenrows; /* the status bar is not text */
  if (y < 1) y = 1;
  int filerow = y - 1 + E.rowoff;

  if (button & 64) {
    int dir = button & 3; /* 0=up 1=down 2=left 3=right */
    int i;
    if (dir >= 2) {
      for (i = 0; i < 3; i++)
        editorMoveCursor(dir == 3 ? ARROW_RIGHT : ARROW_LEFT);
      editorSyncGoal();
    } else {
      for (i = 0; i < 3; i++)
        editorMoveCursor(dir == 1 ? ARROW_DOWN : ARROW_UP);
    }
    return;
  }
  if ((button & 3) != 0) return; /* only the left button selects */

  int last = E.numrows > 0 ? E.numrows - 1 : 0;
  if (filerow < 0) filerow = 0;
  if (filerow > last) filerow = last;

  int rx = x - 1 - E.gutter + E.coloff; /* screen column -> display column */
  if (rx < 0) rx = 0;

  /* release on the press spot keeps a word/line selection; else drag ends */
  if (!(release && E.sel && x == E.mouse_click_x && y == E.mouse_click_y)) {
    E.cy = filerow;
    E.cx = filerow < E.numrows ? editorRowRxToCx(&E.row[filerow], rx) : 0;
    editorSyncGoal();
  }

  if (release) return; /* the selection stays put, ready for Ctrl-C */
  if (button & 32) {   /* dragging: stretch it from the anchor */
    E.sel = 1;
    return;
  }

  /* A press: count it against the last one. */
  long now = editorNowMs();
  int near = abs(x - E.mouse_click_x) <= OSHI_CLICK_SLOP &&
             abs(y - E.mouse_click_y) <= OSHI_CLICK_SLOP;
  E.mouse_click_n = (now - E.mouse_click_ms <= OSHI_CLICK_MS && near)
                      ? E.mouse_click_n + 1 : 1;
  E.mouse_click_ms = now;
  E.mouse_click_x = x;
  E.mouse_click_y = y;

  E.sel_cy = E.cy;
  E.sel_cx = E.cx;
  E.sel = 0;
  if (E.numrows == 0) return;

  if (E.mouse_click_n >= 3) { /* the whole line */
    E.sel_cx = 0;
    E.cx = E.row[E.cy].size;
    E.sel = 1;
    E.mouse_click_n = 0; /* the next click starts over */
  } else if (E.mouse_click_n == 2) { /* the word */
    int from, to;
    editorWordAt(&from, &to);
    E.sel_cx = from;
    E.cx = to;
    E.sel = 1;
  }
}

/* centered bordered box with title and body rows; caller flushes */
void editorDrawBox(struct abuf *ab, const char *title,
                   const char **body, int nbody, int width) {
  char tmp[256];
  int height = nbody + 2;
  int top = (E.screenrows - height) / 2;
  if (top < 0) top = 0;
  int left = (E.screencols - (width + 2)) / 2;   /* +2 for the | borders */
  if (left < 0) left = 0;

  int tlen = (int)strlen(title);
  int pre = (width - tlen) > 0 ? (width - tlen) / 2 : 0;
  int post = width - tlen - pre;                 /* pre+title+post == width */
  if (post < 0) post = 0;
  int i;

  abAppend(ab, "\x1b[?25l", 6);              /* the box has no text field to point a cursor at */
  abAppend(ab, "\x1b[2J\x1b[H", 7);          /* clear + home the alt screen */

  /* top border: +-<pre>title<post>-+ */
  int n = snprintf(tmp, sizeof(tmp), "\x1b[%d;%dH+", top + 1, left + 1);
  abAppend(ab, tmp, n);
  for (i = 0; i < pre; i++) abAppend(ab, "-", 1);
  abAppend(ab, title, tlen);
  for (i = 0; i < post; i++) abAppend(ab, "-", 1);
  abAppend(ab, "+", 1);

  for (i = 0; i < nbody; i++) {
    int blen = (int)strlen(body[i]);
    int n2 = snprintf(tmp, sizeof(tmp), "\x1b[%d;%dH", top + 2 + i, left + 1);
    abAppend(ab, tmp, n2);
    abAppend(ab, "| ", 2);
    abAppend(ab, body[i], blen);
    int pad = width - 1 - blen;
    for (; pad > 0; pad--) abAppend(ab, " ", 1);
    abAppend(ab, "|", 1);
  }

  /* bottom border */
  int n3 = snprintf(tmp, sizeof(tmp), "\x1b[%d;%dH+", top + 2 + nbody, left + 1);
  abAppend(ab, tmp, n3);
  for (i = 0; i < width; i++) abAppend(ab, "-", 1);
  abAppend(ab, "+", 1);
  abAppend(ab, "\x1b[0m", 4);
}

/* help: centered box of every command and key; any key closes */
void editorHelp(void) {
  char lines[CMD_COUNT][128];
  int n = CMD_COUNT, width = 0, i;
  for (i = 0; i < n; i++) {
    char key[16];
    editorKeyName(E.binds[i], key, sizeof(key));
    int len = snprintf(lines[i], sizeof(lines[i]), "%-10s %-14s %s",
                       key, editorCmds[i].name, editorCmds[i].desc);
    if (len > width) width = len;
  }
  width += 1; /* the cell after the longest line */
  const char *body[CMD_COUNT];
  for (i = 0; i < n; i++) body[i] = lines[i];
  for (;;) {
    struct abuf ab = ABUF_INIT;
    editorDrawBox(&ab, "oshi key bindings", body, n, width);
    ssize_t r = write(STDOUT_FILENO, ab.b, ab.len);
    (void)r;
    abFree(&ab);
    int k = editorReadKey();
    if (k == PASTE_KEY) editorSkipPaste();
    if (k == RESIZE_KEY || k == MOUSE_KEY || k == NOKEY || k == PASTE_KEY)
      continue;
    break;
  }
  E.redraw = 1;
}

/* is doas or sudo available? doas first */
static const char *editorElevTool(void) {
  static const char *names[] = { "doas", "sudo" };
  size_t i;
  for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    if (editorInPath(names[i])) return names[i];
  return NULL;
}

/* make the tool's stderr one short bar-safe line */
static void editorElevReason(char *out, size_t outsz, const char *raw, int rawlen) {
  size_t o = 0;
  int i;
  for (i = 0; i < rawlen && o + 1 < outsz; i++) {
    unsigned char c = (unsigned char)raw[i];
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    else if (c < 32 || c == 127) continue;
    out[o++] = (char)c;
  }
  while (o > 0 && out[o - 1] == ' ') o--;
  out[o] = '\0';
}

/* return 0 only if the file on disk differs from the buffer; *got is its size */
static int editorVerifySaved(const char *buf, int len, long *got) {
  struct stat st;
  char chunk[8192];
  int fd, off = 0;

  *got = -1;
  if (stat(E.filename, &st) == -1 || !S_ISREG(st.st_mode)) return 1;
  *got = (long)st.st_size;
  if (st.st_size != (off_t)len) return 0;
  fd = open(E.filename, O_RDONLY);
  if (fd == -1) return 1;
  for (;;) {
    ssize_t n = read(fd, chunk, sizeof(chunk));
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      close(fd);
      return 1; /* unreadable now: trust the exit status */
    }
    if (off + (int)n > len || memcmp(chunk, buf + off, (size_t)n) != 0) {
      close(fd);
      return 0;
    }
    off += (int)n;
  }
  close(fd);
  return off == len;
}

/* redo the save through doas/sudo by piping into dd; 0 on success */
static int editorRunElevated(const char *tool, const char *buf, int len) {
  char etmpl[] = "/tmp/oshi-err-XXXXXX";
  char arg_of[4096];
  int pfd[2];
  int efd, status = 0, ok = 0;
  pid_t pid;

  if (pipe(pfd) == -1) {
    editorSetStatusMessage("Can't save! pipe: %s", strerror(errno));
    return -1;
  }
  efd = mkstemp(etmpl);
  if (efd == -1) {
    editorSetStatusMessage("Can't save! Temp file: %s", strerror(errno));
    close(pfd[0]);
    close(pfd[1]);
    return -1;
  }
  /* path is one argv for dd's of=; bail if it is too long */
  if (strlen(E.filename) + 3 >= sizeof(arg_of)) {
    editorSetStatusMessage("Can't save! Path too long");
    close(pfd[0]);
    close(pfd[1]);
    close(efd);
    unlink(etmpl);
    return -1;
  }
  snprintf(arg_of, sizeof(arg_of), "of=%s", E.filename);
  /* create the missing parent in the same privileged command */
  char *pdir = editorParentDir(E.filename);
  if (pdir && editorIsDir(pdir)) { free(pdir); pdir = NULL; }

  /* park the caret on the bar for the password prompt */
  {
    char at[32];
    int n = snprintf(at, sizeof(at), "\x1b[%d;1H", E.screenrows);
    editorWriteAll(STDOUT_FILENO, at, n);
  }

  disableRawMode(); /* the password is typed at the terminal */
  editorWrite("\x1b[?1002l\x1b[?1006l\x1b[?2004l"); /* no mouse or paste markers while it is typed */
  /* ignore Ctrl-C here; the tool gets the default back */
  struct sigaction ign, old_int, old_quit;
  memset(&ign, 0, sizeof(ign));
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  sigaction(SIGINT, &ign, &old_int);
  sigaction(SIGQUIT, &ign, &old_quit);
  /* start the child before writing so the pipe cannot block */
  pid = fork();
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    dup2(pfd[0], STDIN_FILENO);        /* the buffer arrives here */
    if (devnull != -1) dup2(devnull, STDOUT_FILENO); /* dd's summary line, discarded */
    dup2(efd, STDERR_FILENO);          /* complaints, for the bar */
    close(pfd[0]);
    close(pfd[1]);
    close(efd);
    if (devnull > 2) close(devnull);
    if (pdir)
      execlp(tool, tool, "sh", "-c",
             "mkdir -p -- \"$2\" && exec dd bs=4k of=\"$1\" conv=fsync",
             "sh", E.filename, pdir, (char *)NULL);
    else
      execlp(tool, tool, "dd", "bs=4k", arg_of, "conv=fsync", (char *)NULL);
    _exit(127); /* not found after all */
  }
  close(pfd[0]); /* only the child reads */
  if (pid > 0) {
    /* SIGPIPE is ignored: a dd exit makes this write fail with EPIPE */
    int werr = editorWriteAll(pfd[1], buf, len) == -1 ? errno : 0;
    close(pfd[1]); /* EOF: tells dd it has the whole file */
    while (waitpid(pid, &status, 0) == -1)
      if (errno != EINTR) break;
    ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 && werr == 0;
  } else {
    close(pfd[1]);
  }
  sigaction(SIGINT, &old_int, NULL);
  sigaction(SIGQUIT, &old_quit, NULL);
  enableRawMode();
  editorTermSetup(); /* restore mouse, paste and the rest */
  E.redraw = 1;      /* wipe the prompt and whatever the tool printed */

  if (pid == -1) {
    editorSetStatusMessage("Can't save! fork: %s", strerror(errno));
  } else if (ok) {
    long got = -1;
    if (!editorVerifySaved(buf, len, &got)) {
      editorSetStatusMessage("Save wrong: %ld of %d bytes - check the file", got, len);
      ok = 0;
    } else {
      editorSetStatusMessage("%d bytes written to disk (via %s)", len, tool);
    }
  } else {
    char raw[160] = "", why[160];
    ssize_t got;
    lseek(efd, 0, SEEK_SET); /* the child wrote through the same description */
    got = read(efd, raw, sizeof(raw) - 1);
    if (got < 0) got = 0;
    editorElevReason(why, sizeof(why), raw, (int)got);
    const char *reason = why; /* sudo and doas already name themselves first */
    if (strncmp(reason, tool, strlen(tool)) == 0 && reason[strlen(tool)] == ':' &&
        reason[strlen(tool) + 1] == ' ')
      reason += strlen(tool) + 2;
    if (reason[0])
      editorSetStatusMessage("%s: %s", tool, reason);
    else if (WIFSIGNALED(status))
      editorSetStatusMessage("%s was interrupted - nothing written", tool);
    else
      editorSetStatusMessage("%s failed (exit %d) - nothing written", tool,
                             WEXITSTATUS(status));
  }
  close(efd);
  unlink(etmpl);
  free(pdir);
  return ok ? 0 : -1;
}

/* permission refused: offer to redo as root in a box */
int editorSaveElevated(const char *buf, int len) {
  const char *tool = editorElevTool();
  char lines[5][96];
  const char *body[5];
  int n = 0, width = 0, i;
  char *pd = editorParentDir(E.filename);
  int mk = pd && !editorIsDir(pd);
  free(pd);

  snprintf(lines[n++], sizeof(lines[0]), "%s", "Save refused: permission denied.");
  if (mk && tool)
    snprintf(lines[n++], sizeof(lines[0]), "%s", "The directory is missing: it will be created.");
  if (tool) {
    snprintf(lines[n++], sizeof(lines[0]), "Redo the save as root with %s?", tool);
    snprintf(lines[n++], sizeof(lines[0]), "%-14s%s", "enter", "save as root");
    snprintf(lines[n++], sizeof(lines[0]), "%-14s%s", "esc/q/ctrl-q", "keep editing");
  } else {
    snprintf(lines[n++], sizeof(lines[0]), "%s", "No doas or sudo found in PATH.");
    snprintf(lines[n++], sizeof(lines[0]), "%-14s%s", "esc/q/ctrl-q", "keep editing");
  }
  for (i = 0; i < n; i++) {
    int l = (int)strlen(lines[i]);
    if (l > width) width = l;
    body[i] = lines[i];
  }
  width += 1; /* the cell after the longest line */

  for (;;) {
    struct abuf ab = ABUF_INIT;
    editorDrawBox(&ab, "privileges required", body, n, width);
    ssize_t r = write(STDOUT_FILENO, ab.b, ab.len);
    (void)r;
    abFree(&ab);
    int k = editorReadKey();
    if (k == PASTE_KEY) editorSkipPaste();
    if (k == RESIZE_KEY || k == MOUSE_KEY || k == NOKEY || k == PASTE_KEY)
      continue;
    if (tool && (k == '\r' || k == '\n')) {
      E.redraw = 1;
      return editorRunElevated(tool, buf, len);
    }
    break;
  }
  E.redraw = 1;
  editorSetStatusMessage(tool ? "Save cancelled" : "No sudo or doas in PATH");
  return -1;
}

void editorProcessKeypress(void) {
  int c = editorReadKey();
  int cmd = editorCommandForKey(c);
  /* up/down keep the aimed column; other keys re-aim it */
  int vertical = (c == ARROW_UP || c == ARROW_DOWN || c == SHIFT_UP ||
                  c == SHIFT_DOWN || c == PAGE_UP || c == PAGE_DOWN ||
                  c == MOUSE_KEY);

  if (c == RESIZE_KEY || c == NOKEY) return; /* nothing for us to do */
  E.config_err[0] = '\0'; /* the config message is seen: done */

  /* selection survives only keys that use it; others drop it */
  if (cmd != CMD_COPY && cmd != CMD_CUT && cmd != CMD_PASTE &&
      c != MOUSE_KEY && c != NOKEY && c != CTRL_LEFT && c != CTRL_RIGHT &&
      (c < SHIFT_UP || c > CSHIFT_RIGHT) &&
      c != '\r' && c != '\t' && c != DEL_KEY && c != CTRL_BACKSPACE &&
      c != UTF8_KEY &&
      c != PASTE_KEY && !(c >= 32 && c < ARROW_LEFT)) E.sel = 0;

  /* config-bound commands run first, whatever key */
  if (cmd != CMD_NONE) {
    switch (cmd) {
    case CMD_QUIT: {
      long now = editorNowMs();
      if (E.dirty && !(E.quit_confirm && now - E.quit_time < OSHI_STATUS_MS)) {
        /* first quit on a dirty buffer: warn (a toast); second quits */
        E.quit_confirm = 1;
        E.quit_time = now;
        return;
      }
      editorLeave(); /* back to the file list if we came from one, else out */
      break;
    }
    case CMD_SAVE: editorSave(); break;
    case CMD_FIND: editorFind(); break;
    case CMD_SELECT: editorSelectAll(); break;
    case CMD_COPY: editorCopySelection(); break;
    case CMD_CUT: editorCut(); break;
    case CMD_PASTE: editorPasteInternal(); break;
    case CMD_KILL: editorDeleteLine(); break;
    case CMD_UNDO: editorUndo(); break;
    case CMD_REDO: editorRedo(); break;
    case CMD_REPLACE: editorReplace(); break;
    case CMD_FIND_NEXT:
      if (find_on) editorFindNextMatch(1);
      break;
    case CMD_FIND_PREV:
      if (find_on) editorFindNextMatch(-1);
      break;
    case CMD_GOTO: editorGoto(); break;
    case CMD_HELP: editorHelp(); break;
    case CMD_DELETE_WORD:
      if (E.sel) editorDeleteSelection();
      else editorDeleteWordBackward();
      break;
    }
    E.quit_confirm = 0;
    editorSyncGoal();
    return;
  }

  /* only Esc clears the live search; movement keeps it */
  if (c == '\x1b' && find_on) { /* Esc: clear the live search + highlights */
    find_on = 0;
    find_q[0] = '\0';
    find_row = -1;
  }

  switch (c) {
  case '\r':
    editorInsertNewline();
    break;

  case HOME_KEY:
    E.cx = 0;
    break;

  case END_KEY:
    if (E.cy < E.numrows) E.cx = E.row[E.cy].size;
    break;

  case CTRL_LEFT:
  case CTRL_RIGHT:
    /* a word at a time, or the whole way after select-all */
    if (editorAllSelected()) {
      editorMoveToEnd(c == CTRL_RIGHT ? 1 : -1);
      E.sel = 0;
    } else {
      editorMoveWord(c == CTRL_RIGHT ? 1 : -1);
    }
    break;

  case SHIFT_UP:
  case SHIFT_DOWN:
  case SHIFT_LEFT:
  case SHIFT_RIGHT:
  case CSHIFT_LEFT:
  case CSHIFT_RIGHT:
    /* shift+arrows move the selection's far end; ctrl+shift by word */
    if (!E.sel) { E.sel_cy = E.cy; E.sel_cx = E.cx; E.sel = 1; }
    if (c == CSHIFT_LEFT || c == CSHIFT_RIGHT)
      editorMoveWord(c == CSHIFT_RIGHT ? 1 : -1);
    else
      editorMoveCursor(c == SHIFT_UP ? ARROW_UP : c == SHIFT_DOWN ? ARROW_DOWN
                     : c == SHIFT_LEFT ? ARROW_LEFT : ARROW_RIGHT);
    if (E.sel_cy == E.cy && E.sel_cx == E.cx) E.sel = 0; /* shrunk to nothing */
    break;

  case MOUSE_KEY:
    editorMouse(E.mouse_button, E.mouse_release, E.mouse_x, E.mouse_y);
    break;

  case BACKSPACE:
  case DEL_KEY:
    if (E.sel) {
      editorDeleteSelection();
      break;
    }
    editorDeleteChar(c == DEL_KEY);
    break;

  case PAGE_UP:
  case PAGE_DOWN:
    {
      int last = E.numrows > 0 ? E.numrows - 1 : 0;
      if (c == PAGE_UP) {
        E.cy = E.rowoff;
      } else {
        E.cy = E.rowoff + E.screenrows - 1;
        if (E.cy > last) E.cy = last;
      }
      int times = E.screenrows;
      while (times--)
        editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      if (E.cy < E.numrows) /* landed on a row that may be shorter */
        E.cx = editorRowRxToCx(&E.row[E.cy], E.goal_rx);
    }
    break;

  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    editorMoveCursor(c);
    break;

  case UTF8_KEY:
    editorTypeBytes(E.utf8, E.utf8len);
    break;

  case PASTE_KEY: {
    int n;
    char *t = editorReadPasteBuf(&n);
    /* terminal ctrl-shift-v: same Pasted message as ctrl-v */
    int lines = editorInsertText(t, n);
    free(t);
    if (lines > 0)
      editorSetStatusMessage("Pasted %d line%s", lines, lines == 1 ? "" : "s");
    break;
  }

  case '\x1b':
    /* Esc: no-op beyond clearing a live search (prompts read it themselves) */
  case CTRL_KEY('l'):
  case NOKEY:
    break;

  default:
    /* keep unbound control bytes and Alt-keys out of the file */
    if ((c >= 32 && c < 127) || c == '\t') {
      char ch = (char)c;
      editorTypeBytes(&ch, 1);
    }
    break;
  }
  E.quit_confirm = 0;
  editorClampCursor();
  if (!vertical) editorSyncGoal();
}

/*** config ***/

static int cfg_errs;
static char cfg_first[128];

/* remember the first config error and whether there were more */
static void cfgError(int lineno, const char *fmt, ...) {
  char msg[96];
  va_list ap;
  if (cfg_errs++ > 0) return;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  snprintf(cfg_first, sizeof(cfg_first), "config line %d: %s", lineno, msg);
}

/* mkdir -p for every part of dir */
void editorMkdirs(char *dir) {
  char *p;
  for (p = dir + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(dir, 0700);
      *p = '/';
    }
  }
  mkdir(dir, 0700);
}

/* no config: create the directory and write defaults */
void editorMakeConfig(const char *path) {
  char dir[512];
  FILE *fp;
  snprintf(dir, sizeof(dir), "%s", path);
  char *slash = strrchr(dir, '/');
  if (!slash) return;
  *slash = '\0';
  editorMkdirs(dir);

  fp = fopen(path, "w");
  if (!fp) return;
  fputs(
    "# oshi config - ctrl-e in the editor lists every command and its key.\n"
    "# A mistake here shows up on the status bar at startup.\n"
    "\n"
    "ruler true            # line numbers in the gutter\n"
    "ruler_width default   # or a fixed number of columns\n"
    "cursor block           # block | beam\n"
    "\n"
    "# color <role> <value>   a 256-palette code (0-255) or hex (#rrggbb/#rgb)\n"
    "# roles: status_bg status_fg gutter_fg gutter_cursor_fg gutter_bg\n"
    "#        gutter_cursor_bg tilde_fg text scrollmark_bg scrollmark_fg find\n"
    "# the values below are oshi's built-in theme:\n"
    "color status_bg #222733\n"
    "color status_fg #EAEFF5\n"
    "color gutter_fg #98A0AE\n"
    "color gutter_cursor_fg #E37B66\n"
    "color gutter_bg #101318\n"
    "color gutter_cursor_bg #101318\n"
    "color tilde_fg #98A0AE\n"
    "color text #EAEFF5\n"
    "color scrollmark_bg #98A0AE\n"
    "color scrollmark_fg #101318\n"
    "color find #E0A568\n"
    "# colorscheme default    resets every color above\n"
    "\n"
    "# bind <key> <command>   move a command to a different key, e.g:\n"
    "# bind ctrl-w save\n", fp);
  fclose(fp);
}

/* key names: ctrl-x, alt-x, none; -1 unbindable, -2 reserved */
int editorParseKey(const char *spec) {
  if (strcmp(spec, "none") == 0) return 0;
  if (strncmp(spec, "ctrl-", 5) == 0 && spec[5] && !spec[6] &&
      isalpha((unsigned char)spec[5])) {
    int k = CTRL_KEY(tolower((unsigned char)spec[5]));
    if (k == '\t' || k == '\r') return -2;
    return k;
  }
  if (strncmp(spec, "alt-", 4) == 0 && spec[4] && !spec[5])
    return ALT_KEY(tolower((unsigned char)spec[4]));
  return -1;
}

/* bind <key> <command>: later lines win; negative return on error */
int editorBind(const char *spec, const char *name) {
  int key = editorParseKey(spec);
  int cmd = -1, i;
  for (i = 0; i < CMD_COUNT; i++)
    if (strcmp(name, editorCmds[i].name) == 0) cmd = i;
  if (cmd < 0) return -3;
  if (key < 0) return key;
  for (i = 0; i < CMD_COUNT; i++)
    if (i != cmd && E.binds[i] == key) E.binds[i] = 0;
  E.binds[cmd] = key;
  return 0;
}

/* which command a key runs; CMD_NONE if none */
int editorCommandForKey(int c) {
  int i;
  if (c == 0) return CMD_NONE;
  for (i = 0; i < CMD_COUNT; i++)
    if (E.binds[i] == c) return i;
  return CMD_NONE;
}

/* How a key reads back: ctrl-q, alt-x, off. */
void editorKeyName(int key, char *buf, size_t len) {
  if (key == 0)
    snprintf(buf, len, "off");
  else if (key == CTRL_BACKSPACE)
    snprintf(buf, len, "ctrl-bksp");
  else if (key >= ALT_KEY(0) && key <= ALT_KEY(255))
    snprintf(buf, len, "alt-%c", key - ALT_KEY(0));
  else if (key >= 1 && key <= 26)
    snprintf(buf, len, "ctrl-%c", key - 1 + 'a');
  else
    snprintf(buf, len, "?");
}

static int cfgBool(const char *val) {
  if (!strcmp(val, "true") || !strcmp(val, "on") ||
      !strcmp(val, "1") || !strcmp(val, "yes")) return 1;
  if (!strcmp(val, "false") || !strcmp(val, "off") ||
      !strcmp(val, "0") || !strcmp(val, "no")) return 0;
  return -1;
}

/* parse a color: 256-palette code or hex; 1 if applied */
static int editorApplyColorSpec(int role, const char *spec) {
  if (spec[0] == '#') {
    size_t i, len = strlen(spec + 1);
    unsigned vals[6];
    if (len != 3 && len != 6) return 0;
    for (i = 0; i < len; i++)
      if (!isxdigit((unsigned char)spec[1 + i])) return 0;
    if (len == 6) {
      if (sscanf(spec + 1, "%02x%02x%02x", &vals[0], &vals[1], &vals[2]) != 3)
        return 0;
      editorColorSetRGB(role, (int)vals[0], (int)vals[1], (int)vals[2]);
    } else {
      if (sscanf(spec + 1, "%01x%01x%01x", &vals[0], &vals[1], &vals[2]) != 3)
        return 0;
      editorColorSetRGB(role, (int)(vals[0] * 17), (int)(vals[1] * 17),
                        (int)(vals[2] * 17));
    }
    return 1;
  }
  {
    char *end;
    long c = strtol(spec, &end, 10);
    if (*spec && !*end && c >= 0 && c < 256) {
      editorColorSet(role, (int)c);
      return 1;
    }
  }
  return 0;
}

void editorLoadConfig(void) {
  char path[512];
  const char *dir = getenv("XDG_CONFIG_HOME");
  if (dir && *dir) {
    snprintf(path, sizeof(path), "%s/oshi/config", dir);
  } else {
    const char *home = getenv("HOME");
    if (!home) return;
    snprintf(path, sizeof(path), "%s/.config/oshi/config", home);
  }

  FILE *fp = fopen(path, "r");
  if (!fp) {
    if (errno == ENOENT) editorMakeConfig(path);
    return;
  }

  char line[256];
  int lineno = 0;
  while (fgets(line, sizeof(line), fp)) {
    lineno++;
    size_t n = strlen(line);
    if (n > 0 && line[n - 1] != '\n' && !feof(fp)) {
      /* overlong line: swallow the rest */
      int ch;
      while ((ch = fgetc(fp)) != EOF && ch != '\n') ;
      cfgError(lineno, "line too long");
      continue;
    }
    /* color lines hold #rrggbb, so parse them before '#' comments */
    {
      char w1[64], w2[64], w3[64];
      int nw = sscanf(line, " %63[^ \t=\r\n] %63[^ \t=\r\n] %63[^ \t=\r\n]",
                      w1, w2, w3);
      if (nw >= 1 && strcmp(w1, "color") == 0) {
        int r;
        if (nw < 3) cfgError(lineno, "'color' wants a role and a value");
        else if ((r = editorParseColorRole(w2)) < 0)
          cfgError(lineno, "no color role '%s'", w2);
        else if (!editorApplyColorSpec(r, w3))
          cfgError(lineno, "'%s' isn't a color (0-255 or #rrggbb)", w3);
        continue;
      }
    }

    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';

    char *key = strtok(line, " \t=\r\n");
    if (!key) continue; /* blank, or all comment */
    char *val = strtok(NULL, " \t=\r\n");
    if (!val) {
      cfgError(lineno, "'%s' needs a value", key);
      continue;
    }

    if (strcmp(key, "bind") == 0) {
      char *cmdname = strtok(NULL, " \t=\r\n");
      if (!cmdname) {
        cfgError(lineno, "bind wants a key and a command");
        continue;
      }
      switch (editorBind(val, cmdname)) {
      case -1: cfgError(lineno, "'%s' isn't a key (ctrl-x, alt-x, none)", val); break;
      case -2: cfgError(lineno, "%s can't be rebound (Tab/Enter)", val); break;
      case -3: cfgError(lineno, "no command called '%s'", cmdname); break;
      }
      continue;
    }

    if (strcmp(key, "colorscheme") == 0) {
      if (strcmp(val, "default") == 0) editorApplyDefaultColors();
      else cfgError(lineno, "colorscheme knows only 'default'");
      continue;
    }

    if (strcmp(key, "ruler") == 0) {
      int b = cfgBool(val);
      if (b < 0) cfgError(lineno, "ruler wants true or false");
      else E.ruler = b;
    } else if (strcmp(key, "ruler_width") == 0) {
      if (strcmp(val, "default") == 0) {
        E.ruler_width = 0; /* auto-fit to line count */
      } else {
        char *end;
        long w = strtol(val, &end, 10);
        if (*val && !*end && w > 0 && w < 100000)
          E.ruler_width = (int)w;
        else
          cfgError(lineno, "ruler_width wants default or a number");
      }
    } else if (strcmp(key, "cursor") == 0) {
      if (strcmp(val, "block") == 0) E.cursor_style = 2;
      else if (strcmp(val, "beam") == 0) E.cursor_style = 6;
      else cfgError(lineno, "cursor wants block or beam");
    } else {
      cfgError(lineno, "unknown setting '%s'", key);
    }
  }
  fclose(fp);
  if (cfg_errs > 0) {
    snprintf(E.config_err, sizeof(E.config_err), "%s%s", cfg_first,
             cfg_errs > 1 ? " (and more)" : "");
    E.config_err_time = editorNowMs(); /* a toast, like the other notices */
  }
}

/*** colors ***/

/* 256-palette escape for a color role */
void editorColorSet(int role, int code) {
  snprintf(E.col_str[role], sizeof(E.col_str[role]),
           colIsFg[role] ? "\x1b[38;5;%dm" : "\x1b[48;5;%dm", code);
}

/* RGB escape for a color role */
void editorColorSetRGB(int role, int r, int g, int b) {
  snprintf(E.col_str[role], sizeof(E.col_str[role]),
           colIsFg[role] ? "\x1b[38;2;%d;%d;%dm" : "\x1b[48;2;%d;%d;%dm",
           r, g, b);
}

/* the stock theme; 'colorscheme default' resets to it */
void editorApplyDefaultColors(void) {
  editorColorSetRGB(COL_STATUS_BG, 0x22, 0x27, 0x33); /* black panel */
  editorColorSetRGB(COL_STATUS_FG, 0xEA, 0xEF, 0xF5); /* foreground */
  editorColorSetRGB(COL_GUTTER_FG, 0x98, 0xA0, 0xAE); /* light black */
  editorColorSetRGB(COL_GUTTER_CURSOR_FG, 0xE3, 0x7B, 0x66); /* cursor accent */
  editorColorSetRGB(COL_GUTTER_BG, 0x10, 0x13, 0x18); /* background */
  editorColorSetRGB(COL_GUTTER_CURSOR_BG, 0x10, 0x13, 0x18); /* background: only the number changes */
  editorColorSetRGB(COL_TILDE_FG, 0x98, 0xA0, 0xAE); /* light black fringe */
  editorColorSetRGB(COL_TEXT_FG, 0xEA, 0xEF, 0xF5); /* foreground */
  editorColorSetRGB(COL_SCROLLMARK_BG, 0x98, 0xA0, 0xAE); /* light black chip */
  editorColorSetRGB(COL_SCROLLMARK_FG, 0x10, 0x13, 0x18); /* background ink */
  editorColorSetRGB(COL_FIND, 0xE0, 0xA5, 0x68); /* yellow hit */
}

int editorParseColorRole(const char *name) {
  if (!strcmp(name, "status_bg")) return COL_STATUS_BG;
  if (!strcmp(name, "status_fg")) return COL_STATUS_FG;
  if (!strcmp(name, "gutter_fg")) return COL_GUTTER_FG;
  if (!strcmp(name, "gutter_cursor_fg")) return COL_GUTTER_CURSOR_FG;
  if (!strcmp(name, "gutter_bg")) return COL_GUTTER_BG;
  if (!strcmp(name, "gutter_cursor_bg")) return COL_GUTTER_CURSOR_BG;
  if (!strcmp(name, "tilde_fg")) return COL_TILDE_FG;
  if (!strcmp(name, "text")) return COL_TEXT_FG;
  if (!strcmp(name, "scrollmark_bg")) return COL_SCROLLMARK_BG;
  if (!strcmp(name, "scrollmark_fg")) return COL_SCROLLMARK_FG;
  if (!strcmp(name, "find")) return COL_FIND;
  return -1;
}

/*** file browser ***/

/* `oshi dir/` opens a file browser; quitting a file returns to it */

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

struct brEntry {
  char *name;
  int isdir;  /* a folder, or a link that leads to one */
  int islink;
  off_t size;
};

static struct brEntry *br_ent;
static int br_n, br_cap;
static char br_dir[PATH_MAX];      /* the folder on show, always absolute */
static char br_last[NAME_MAX + 1]; /* the file opened last: the list lands on it */
static int br_sel, br_top;
static int br_hidden;              /* show dotfiles */
static int br_active;              /* the editor was started from the list */

static int brListHeight(void) {
  int h = E.screenrows - 1; /* the header and the bar take two of the rows */
  return h < 1 ? 1 : h;
}

static void brFreeEntries(void) {
  int i;
  for (i = 0; i < br_n; i++) free(br_ent[i].name);
  br_n = 0;
}

static int brCmp(const void *a, const void *b) {
  const struct brEntry *x = a, *y = b;
  int r;
  if (x->isdir != y->isdir) return y->isdir - x->isdir; /* folders first */
  r = strcasecmp(x->name, y->name);
  return r ? r : strcmp(x->name, y->name);
}

static void brAddEntry(const char *name, int isdir, int islink, off_t size) {
  if (br_n == br_cap) {
    br_cap = br_cap ? br_cap * 2 : 64;
    br_ent = xrealloc(br_ent, sizeof(*br_ent) * (size_t)br_cap);
  }
  br_ent[br_n].name = strdup(name);
  if (br_ent[br_n].name == NULL) die("strdup");
  br_ent[br_n].isdir = isdir;
  br_ent[br_n].islink = islink;
  br_ent[br_n].size = size;
  br_n++;
}

/* br_dir/name into out. -1 when it doesn't fit. */
static int brPath(char *out, size_t sz, const char *name) {
  int n = snprintf(out, sz, "%s%s%s", br_dir,
                   strcmp(br_dir, "/") == 0 ? "" : "/", name);
  return (n < 0 || (size_t)n >= sz) ? -1 : 0;
}

/* read br_dir into the list; -1 with errno, list untouched */
static int brLoad(void) {
  DIR *d = opendir(br_dir);
  struct dirent *de;
  int first;
  if (d == NULL) return -1;
  brFreeEntries();
  if (strcmp(br_dir, "/") != 0) brAddEntry("..", 1, 0, 0);
  first = br_n;
  while ((de = readdir(d)) != NULL) {
    char full[PATH_MAX + NAME_MAX + 2];
    struct stat lst, st;
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    if (!br_hidden && de->d_name[0] == '.') continue;
    if (brPath(full, sizeof(full), de->d_name) == -1) continue;
    if (lstat(full, &lst) == -1) continue;
    if (S_ISLNK(lst.st_mode) && stat(full, &st) == 0)
      brAddEntry(de->d_name, S_ISDIR(st.st_mode), 1, st.st_size);
    else
      brAddEntry(de->d_name, S_ISDIR(lst.st_mode), S_ISLNK(lst.st_mode),
                 lst.st_size);
  }
  closedir(d);
  if (br_ent != NULL && br_n - first > 1)
    qsort(br_ent + first, (size_t)(br_n - first), sizeof(*br_ent), brCmp);
  return 0;
}

/* cursor to `name`, else the first real entry */
static void brSelectName(const char *name) {
  int i;
  br_sel = (br_ent != NULL && br_n > 1 && strcmp(br_ent[0].name, "..") == 0) ? 1 : 0;
  if (name == NULL) return;
  for (i = 0; i < br_n; i++)
    if (strcmp(br_ent[i].name, name) == 0) {
      br_sel = i;
      return;
    }
}

/* go to dir; on failure keep the old folder and set msg */
static int brGo(const char *dir, const char *select, char *msg, size_t msgsz) {
  char old[PATH_MAX], real[PATH_MAX];
  if (realpath(dir, real) == NULL) {
    snprintf(msg, msgsz, "%s: %s", dir, strerror(errno));
    return -1;
  }
  snprintf(old, sizeof(old), "%s", br_dir);
  snprintf(br_dir, sizeof(br_dir), "%s", real);
  if (brLoad() == -1) {
    int e = errno;
    snprintf(br_dir, sizeof(br_dir), "%s", old);
    snprintf(msg, msgsz, "Can't open folder: %s", strerror(e));
    return -1;
  }
  br_top = 0;
  brSelectName(select);
  return 0;
}

static void brUp(char *msg, size_t msgsz) {
  char child[NAME_MAX + 1], parent[PATH_MAX];
  char *slash = strrchr(br_dir, '/');
  if (slash == NULL || strcmp(br_dir, "/") == 0) return;
  snprintf(child, sizeof(child), "%s", slash + 1);
  if (slash == br_dir) {
    snprintf(parent, sizeof(parent), "/");
  } else {
    size_t n = (size_t)(slash - br_dir);
    memcpy(parent, br_dir, n);
    parent[n] = '\0';
  }
  brGo(parent, child, msg, msgsz); /* lands on the folder we came out of */
}

/* re-read the folder, keeping the cursor on `select` or its row */
static void brReload(const char *select, char *msg, size_t msgsz) {
  int keep = br_sel;
  if (brLoad() == -1) {
    snprintf(msg, msgsz, "Can't read folder: %s", strerror(errno));
    return;
  }
  if (select) brSelectName(select);
  else br_sel = keep;
}

/* draw a name safely: control, C1 and bidi bytes become '?'; returns cells used */
static int brPutClean(struct abuf *ab, const char *s, int maxcols) {
  int used = 0, i = 0, n = (int)strlen(s);
  if (maxcols < 0) maxcols = 0;
  while (i < n) {
    int len, w, cp = utf8Decode((const unsigned char *)s + i, n - i, &len);
    int bad = cp < 32 || cp == 127 || (cp >= 0x80 && cp < 0xA0) ||
              cp == 0x061C || cp == 0x200E || cp == 0x200F ||
              (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069);
    w = bad ? 1 : cpWidth(cp);
    if (used + w > maxcols) break;
    if (bad) abAppend(ab, "?", 1);
    else if (cp == 0xFFFD && len == 1) abAppend(ab, "\xEF\xBF\xBD", 3);
    else abAppend(ab, s + i, len);
    used += w;
    i += len;
  }
  return used;
}

static void brPad(struct abuf *ab, int n) {
  while (n-- > 0) abAppend(ab, " ", 1);
}

static void brColor(struct abuf *ab, int role) {
  abAppend(ab, E.col_str[role], (int)strlen(E.col_str[role]));
}

static void brHuman(off_t n, char *out, size_t sz) {
  static const char unit[] = "KMGTP";
  double v = (double)n;
  int u = -1;
  if (n < 1024) {
    snprintf(out, sz, "%lldB", (long long)n);
    return;
  }
  while (v >= 1024 && u < 4) {
    v /= 1024;
    u++;
  }
  snprintf(out, sz, "%.1f%c", v, unit[u]);
}

/* draw the folder, the list and the bar */
static void brDraw(const char *msg, const char *plabel, const char *pbuf) {
  static const char hint[] =
    "enter open  n file  N folder  r rename  d delete  . hidden  ? help  q quit";
  struct abuf ab = ABUF_INIT;
  char tmp[48];
  int rows = E.screenrows + 1, cols = E.screencols;
  int lh = brListHeight(), r, n, used;

  if (br_sel >= br_n) br_sel = br_n - 1;
  if (br_sel < 0) br_sel = 0;
  if (br_sel < br_top) br_top = br_sel;
  if (br_sel >= br_top + lh) br_top = br_sel - lh + 1;
  if (br_top < 0) br_top = 0;

  abAppend(&ab, "\x1b[?25l", 6);
  if (E.redraw) {
    abAppend(&ab, "\x1b[2J", 4);
    E.redraw = 0;
  }

  /* the folder, its tail if it is too long for the row */
  abAppend(&ab, "\x1b[1;1H\x1b[0m", 10);
  brColor(&ab, COL_STATUS_BG);
  brColor(&ab, COL_STATUS_FG);
  abAppend(&ab, " ", 1);
  {
    const char *s = br_dir;
    int dl = (int)strlen(s), avail = cols - 2;
    if (avail < 1) avail = 1;
    used = 1;
    if (editorStrWidth(s, dl) > avail) {
      s += editorFitTail(s, dl, avail - 1);
      abAppend(&ab, "<", 1);
      used++;
    }
    used += brPutClean(&ab, s, cols - used);
    brPad(&ab, cols - used);
  }
  abAppend(&ab, "\x1b[0m", 4);

  for (r = 0; r < lh; r++) {
    int idx = br_top + r;
    n = snprintf(tmp, sizeof(tmp), "\x1b[%d;1H\x1b[0m\x1b[K", 2 + r);
    abAppend(&ab, tmp, n);
    if (idx < br_n) {
      const struct brEntry *e = &br_ent[idx];
      int sizew = cols >= 36 ? 10 : 0;
      int namew = cols - 1 - sizew;
      char label[NAME_MAX + 3];
      const char *suffix = strcmp(e->name, "..") == 0 ? ""
                         : e->isdir ? "/" : e->islink ? "@" : "";
      brColor(&ab, e->isdir ? COL_GUTTER_CURSOR_FG : COL_TEXT_FG);
      if (idx == br_sel) abAppend(&ab, "\x1b[7m", 4);
      abAppend(&ab, " ", 1);
      snprintf(label, sizeof(label), "%s%s", e->name, suffix);
      used = brPutClean(&ab, label, namew);
      brPad(&ab, namew - used);
      if (sizew) {
        char hs[32], col[48];
        hs[0] = '\0';
        if (!e->isdir) brHuman(e->size, hs, sizeof(hs));
        n = snprintf(col, sizeof(col), "%9s ", hs);
        abAppend(&ab, col, n);
      }
    } else if (br_n == 0 && r == 0) {
      brColor(&ab, COL_TILDE_FG);
      abAppend(&ab, " (empty)", 8);
    }
    abAppend(&ab, "\x1b[0m", 4);
  }

  n = snprintf(tmp, sizeof(tmp), "\x1b[%d;1H\x1b[0m", rows);
  abAppend(&ab, tmp, n);
  brColor(&ab, COL_STATUS_BG);
  brColor(&ab, COL_STATUS_FG);
  abAppend(&ab, " ", 1);
  used = 1;
  if (plabel) {
    int room, bl = (int)strlen(pbuf);
    used += brPutClean(&ab, plabel, cols - 2);
    room = cols - used - 2;
    if (room < 1) room = 1;
    used += brPutClean(&ab, pbuf + editorFitTail(pbuf, bl, room), room);
    abAppend(&ab, "\x1b[7m \x1b[27m", 10); /* the caret */
    used++;
  } else if (msg[0]) {
    used += brPutClean(&ab, msg, cols - 2);
  } else {
    used += brPutClean(&ab, hint, cols - 2);
  }
  brPad(&ab, cols - used);
  abAppend(&ab, "\x1b[0m", 4);

  editorWriteAll(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/* A name that makes sense inside one folder. */
static int brValidName(const char *s) {
  return s[0] != '\0' && strchr(s, '/') == NULL && strcmp(s, ".") != 0 &&
         strcmp(s, "..") != 0;
}

/* read one line on the bar; NULL on Esc */
static char *brPrompt(const char *label, const char *initial) {
  char buf[NAME_MAX + 1];
  size_t len;
  snprintf(buf, sizeof(buf), "%s", initial ? initial : "");
  len = strlen(buf);
  for (;;) {
    int c;
    brDraw("", label, buf);
    c = editorReadKey();
    if (c == '\x1b') return NULL;
    if (c == '\r') {
      char *out;
      if (len == 0) continue;
      out = strdup(buf);
      if (out == NULL) die("strdup");
      return out;
    }
    if (c == BACKSPACE || c == DEL_KEY) {
      if (len > 0) { /* a whole character, not one byte of it */
        do {
          len--;
        } while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80);
        buf[len] = '\0';
      }
    } else if (c == CTRL_BACKSPACE) {
      while (len > 0 && buf[len - 1] == ' ') len--;
      while (len > 0 && buf[len - 1] != ' ') len--;
      buf[len] = '\0';
    } else if (c == UTF8_KEY) {
      if (len + (size_t)E.utf8len <= NAME_MAX) {
        memcpy(buf + len, E.utf8, (size_t)E.utf8len);
        len += (size_t)E.utf8len;
        buf[len] = '\0';
      }
    } else if (c == PASTE_KEY) { /* the first line, printable bytes only */
      int n, i;
      char *t = editorReadPasteBuf(&n);
      for (i = 0; i < n && t[i] != '\n' && t[i] != '\r'; i++) {
        unsigned char u = (unsigned char)t[i];
        if (u < 32 || u == 127) continue;
        if (len + 1 > NAME_MAX) break;
        buf[len++] = t[i];
      }
      buf[len] = '\0';
      free(t);
    } else if (c >= 32 && c < 127) {
      if (len + 1 <= NAME_MAX) {
        buf[len++] = (char)c;
        buf[len] = '\0';
      }
    }
  }
}

/* confirm on the bar; only y says yes */
static int brConfirm(const char *line) {
  for (;;) {
    int k;
    brDraw(line, NULL, NULL);
    k = editorReadKey();
    if (k == 'y' || k == 'Y') return 1;
    if (k == 'n' || k == 'N' || k == 'q' || k == '\x1b' || k == CTRL_KEY('q'))
      return 0;
    if (k == PASTE_KEY) editorSkipPaste();
  }
}

static int brDirIsEmpty(const char *path) {
  DIR *d = opendir(path);
  struct dirent *de;
  int empty = 1;
  if (d == NULL) return 1; /* rmdir will say what is wrong */
  while ((de = readdir(d)) != NULL)
    if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
      empty = 0;
      break;
    }
  closedir(d);
  return empty;
}

/* rm -r for one path; links are removed, not followed */
static int brRemoveTree(const char *path, int depth) {
  struct stat st;
  DIR *d;
  struct dirent *de;
  int rc = 0, e;
  if (lstat(path, &st) == -1) return -1;
  if (!S_ISDIR(st.st_mode)) return unlink(path);
  if (depth > 64) {
    errno = ELOOP;
    return -1;
  }
  d = opendir(path);
  if (d == NULL) return -1;
  while ((de = readdir(d)) != NULL) {
    char child[PATH_MAX];
    int n;
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    n = snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
    if (n < 0 || (size_t)n >= sizeof(child)) {
      errno = ENAMETOOLONG;
      rc = -1;
      break;
    }
    if (brRemoveTree(child, depth + 1) == -1) {
      rc = -1;
      break;
    }
  }
  e = errno;
  closedir(d);
  if (rc == -1) {
    errno = e;
    return -1;
  }
  return rmdir(path);
}

static void brHelp(void) {
  static const char *body[] = {
    "up/down  j/k        move",
    "enter  right  l     open a file or folder",
    "backspace  left  h  up a folder",
    "n                   new file",
    "N                   new folder",
    "r                   rename",
    "d  del              delete (asks first)",
    ".                   show or hide dotfiles",
    "q  ctrl-q           quit",
  };
  int n = (int)(sizeof(body) / sizeof(body[0])), width = 0, i;
  for (i = 0; i < n; i++) {
    int l = (int)strlen(body[i]);
    if (l > width) width = l;
  }
  width += 1;
  for (;;) {
    struct abuf ab = ABUF_INIT;
    int k;
    editorDrawBox(&ab, "files", body, n, width);
    editorWriteAll(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
    k = editorReadKey();
    if (k == PASTE_KEY) editorSkipPaste();
    if (k == RESIZE_KEY || k == MOUSE_KEY || k == NOKEY || k == PASTE_KEY)
      continue;
    break;
  }
  E.redraw = 1;
}

/* show the folder `start`; returns the picked path (malloc'd) or NULL */
char *editorBrowse(const char *start, const char *select) {
  char msg[300] = "";
  char real[PATH_MAX];
  static long last_click_ms;
  static int last_click_idx = -1;

  if (realpath(start, real) == NULL) die(start);
  snprintf(br_dir, sizeof(br_dir), "%s", real);
  br_top = 0;
  if (brLoad() == -1) die(br_dir);
  brSelectName(select);
  E.redraw = 1;
  E.notice_pending = 0;
  E.in_prompt = 0;

  for (;;) {
    int c, page = brListHeight() - 1;
    char path[PATH_MAX + NAME_MAX + 2], keep[NAME_MAX + 1];
    const struct brEntry *e;
    int do_open = 0;

    brDraw(msg, NULL, NULL);
    msg[0] = '\0';
    c = editorReadKey();
    e = br_n > 0 ? &br_ent[br_sel] : NULL;
    if (page < 1) page = 1;

    if (c == RESIZE_KEY || c == NOKEY) continue;
    if (c == PASTE_KEY) {
      editorSkipPaste();
      continue;
    }
    if (c == 'q' || c == E.binds[CMD_QUIT]) {
      E.redraw = 1;
      return NULL;
    }

    switch (c) {
    case ARROW_UP: case 'k': if (br_sel > 0) br_sel--; break;
    case ARROW_DOWN: case 'j': if (br_sel + 1 < br_n) br_sel++; break;
    case PAGE_UP: br_sel -= page; if (br_sel < 0) br_sel = 0; break;
    case PAGE_DOWN:
      br_sel += page;
      if (br_sel > br_n - 1) br_sel = br_n - 1;
      break;
    case HOME_KEY: case 'g': br_sel = 0; break;
    case END_KEY: case 'G': br_sel = br_n > 0 ? br_n - 1 : 0; break;

    case ARROW_LEFT: case BACKSPACE: case 'h':
      brUp(msg, sizeof(msg));
      break;

    case '\r': case ARROW_RIGHT: case 'l':
      do_open = 1;
      break;

    case '.':
      snprintf(keep, sizeof(keep), "%s", e ? e->name : "");
      br_hidden = !br_hidden;
      brReload(keep, msg, sizeof(msg));
      break;

    case 'n': case 'N': {
      int isdir = c == 'N';
      char *name = brPrompt(isdir ? "New folder: " : "New file: ", NULL);
      if (name == NULL) break;
      if (!brValidName(name)) {
        snprintf(msg, sizeof(msg), "Not a name for a file: it can't hold a '/'");
      } else if (brPath(path, sizeof(path), name) == -1) {
        snprintf(msg, sizeof(msg), "That path is too long");
      } else if (isdir ? mkdir(path, 0777) == -1 : 0) {
        snprintf(msg, sizeof(msg), "Can't make %s: %s", name, strerror(errno));
      } else {
        int ok = 1;
        if (!isdir) {
          int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
          if (fd == -1) {
            snprintf(msg, sizeof(msg), "Can't make %s: %s", name, strerror(errno));
            ok = 0;
          } else {
            close(fd);
          }
        }
        if (ok) {
          brReload(name, msg, sizeof(msg));
          if (msg[0] == '\0')
            snprintf(msg, sizeof(msg), "Made %s%s", name,
                     name[0] == '.' && !br_hidden ? " (hidden: press . to see it)" : "");
        }
      }
      free(name);
      break;
    }

    case 'r': {
      char *name;
      if (e == NULL || strcmp(e->name, "..") == 0) break;
      snprintf(keep, sizeof(keep), "%s", e->name);
      name = brPrompt("Rename to: ", keep);
      if (name == NULL || strcmp(name, keep) == 0) {
        free(name);
        break;
      }
      if (!brValidName(name)) {
        snprintf(msg, sizeof(msg), "Not a name for a file: it can't hold a '/'");
      } else {
        char from[PATH_MAX + NAME_MAX + 2];
        struct stat st;
        if (brPath(from, sizeof(from), keep) == -1 ||
            brPath(path, sizeof(path), name) == -1) {
          snprintf(msg, sizeof(msg), "That path is too long");
        } else if (lstat(path, &st) == 0) {
          snprintf(msg, sizeof(msg), "%s already exists", name);
        } else if (rename(from, path) == -1) {
          snprintf(msg, sizeof(msg), "Can't rename: %s", strerror(errno));
        } else {
          brReload(name, msg, sizeof(msg));
          if (msg[0] == '\0') snprintf(msg, sizeof(msg), "Renamed to %s", name);
        }
      }
      free(name);
      break;
    }

    case 'd': case DEL_KEY: {
      char line[NAME_MAX + 96];
      int full;
      if (e == NULL || strcmp(e->name, "..") == 0) break;
      if (brPath(path, sizeof(path), e->name) == -1) {
        snprintf(msg, sizeof(msg), "That path is too long");
        break;
      }
      full = e->isdir && !e->islink && !brDirIsEmpty(path);
      snprintf(keep, sizeof(keep), "%s", e->name);
      if (full)
        snprintf(line, sizeof(line),
                 "Delete folder '%s' and EVERYTHING in it? (y/n)", keep);
      else
        snprintf(line, sizeof(line), "Delete '%s'? (y/n)", keep);
      if (!brConfirm(line)) {
        snprintf(msg, sizeof(msg), "Kept %s", keep);
        break;
      }
      if (brRemoveTree(path, 0) == -1) {
        snprintf(msg, sizeof(msg), "Can't delete %s: %s", keep, strerror(errno));
        brReload(NULL, line, sizeof(line)); /* it may be half gone */
      } else {
        brReload(NULL, msg, sizeof(msg));
        if (msg[0] == '\0') snprintf(msg, sizeof(msg), "Deleted %s", keep);
      }
      break;
    }

    case '?':
      brHelp();
      break;

    case MOUSE_KEY: {
      int button = E.mouse_button;
      int idx = br_top + (E.mouse_y - 2);
      if (button & 64) { /* the wheel: three rows a notch */
        int step = (button & 1) ? 3 : -3;
        br_sel += step;
        if (br_sel < 0) br_sel = 0;
        if (br_sel > br_n - 1) br_sel = br_n > 0 ? br_n - 1 : 0;
      } else if (!E.mouse_release && (button & ~(4 | 8 | 16)) == 0 &&
                 E.mouse_y >= 2 && E.mouse_y < 2 + brListHeight() &&
                 idx >= 0 && idx < br_n) {
        long now = editorNowMs();
        br_sel = idx;
        if (idx == last_click_idx && now - last_click_ms <= OSHI_CLICK_MS)
          do_open = 1; /* a second click on the same row opens it */
        last_click_idx = idx;
        last_click_ms = now;
      }
      break;
    }
    }

    if (do_open) {
      e = br_n > 0 ? &br_ent[br_sel] : NULL; /* the list may have moved under us */
      if (e == NULL) continue;
      if (e->isdir) {
        if (strcmp(e->name, "..") == 0) {
          brUp(msg, sizeof(msg));
        } else if (brPath(path, sizeof(path), e->name) == -1) {
          snprintf(msg, sizeof(msg), "That path is too long");
        } else {
          brGo(path, NULL, msg, sizeof(msg));
        }
      } else if (brPath(path, sizeof(path), e->name) == -1) {
        snprintf(msg, sizeof(msg), "That path is too long");
      } else {
        struct stat st;
        if (stat(path, &st) == -1) {
          snprintf(msg, sizeof(msg), "Can't open %s: %s", e->name, strerror(errno));
        } else if (!S_ISREG(st.st_mode)) {
          snprintf(msg, sizeof(msg), "%s isn't a regular file", e->name);
        } else if (access(path, R_OK) == -1) {
          snprintf(msg, sizeof(msg), "Can't read %s: %s", e->name, strerror(errno));
        } else {
          char *out = strdup(path);
          if (out == NULL) die("strdup");
          snprintf(br_last, sizeof(br_last), "%s", e->name);
          E.redraw = 1;
          return out;
        }
      }
    }
  }
}

/* empty buffer; E.clip stays so copies carry over */
static void editorResetBuffer(void) {
  int i;
  for (i = 0; i < E.numrows; i++) editorFreeRow(&E.row[i]);
  free(E.row);
  E.row = NULL;
  E.numrows = 0;
  E.rowcap = 0;
  free(E.filename);
  E.filename = NULL;
  while (undo_count > 0) editorFreeRec(&undo_stack[--undo_count]);
  while (redo_count > 0) editorFreeRec(&redo_stack[--redo_count]);
  undo_dropped = 0;
  undo_run = 0;
  undo_pending = 0;
  find_on = 0;
  find_q[0] = '\0';
  find_row = -1;
  E.cx = E.cy = E.rx = E.goal_rx = 0;
  E.rowoff = E.coloff = 0;
  E.sel = 0;
  E.crlf = 0;
  E.quit_confirm = 0;
  E.mouse_click_n = 0;
  E.statusmsg[0] = '\0';
  editorSnapshot();
  E.dirty = 0;
}

/* ctrl-q on a finished buffer: quit, or back to the list */
void editorLeave(void) {
  char *path;
  if (!br_active) editorQuit();
  editorResetBuffer();
  path = editorBrowse(br_dir, br_last);
  if (path == NULL) editorQuit();
  editorOpen(path);
  free(path);
}

/*** init ***/

void editorFreeAll(void) {
  int i;
  for (i = 0; i < E.numrows; i++)
    editorFreeRow(&E.row[i]);
  free(E.row);
  E.row = NULL;
  E.numrows = 0;
  free(E.filename);
  E.filename = NULL;
  free(E.clip);
  E.clip = NULL;
  while (undo_count > 0) editorFreeRec(&undo_stack[--undo_count]);
  free(undo_stack);
  undo_stack = NULL;
  while (redo_count > 0) editorFreeRec(&redo_stack[--redo_count]);
  free(redo_stack);
  redo_stack = NULL;
  free(saved_text);
  saved_text = NULL;
  saved_len = 0;
}

void initEditor(void) {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.goal_rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.rowcap = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  E.notice_pending = 0;
  E.notice_deadline = 0;
  E.config_err[0] = '\0';
  E.config_err_time = 0;
  E.quit_time = 0;
  E.ruler = 1;
  E.ruler_width = 0;
  E.gutter = 0;
  E.cursor_style = 2; /* steady block */
  E.clip = NULL;
  E.clip_shared = 0;
  editorSnapshot(); /* an empty buffer counts as what's on disk */
  E.dirty = 0;
  E.binds[CMD_SAVE] = CTRL_KEY('s');
  E.binds[CMD_QUIT] = CTRL_KEY('q');
  E.binds[CMD_FIND] = CTRL_KEY('f');
  E.binds[CMD_SELECT] = CTRL_KEY('a');
  E.binds[CMD_COPY] = CTRL_KEY('c');
  E.binds[CMD_CUT] = CTRL_KEY('x');
  E.binds[CMD_PASTE] = CTRL_KEY('v');
  E.binds[CMD_KILL] = CTRL_KEY('d');
  E.binds[CMD_UNDO] = CTRL_KEY('z');
  E.binds[CMD_REDO] = CTRL_KEY('r');
  E.binds[CMD_REPLACE] = ALT_KEY('r');
  E.binds[CMD_FIND_NEXT] = CTRL_KEY('n');
  E.binds[CMD_FIND_PREV] = CTRL_KEY('p');
  E.binds[CMD_GOTO] = CTRL_KEY('g');
  E.binds[CMD_HELP] = CTRL_KEY('e');
  E.binds[CMD_DELETE_WORD] = CTRL_BACKSPACE;
  editorApplyDefaultColors();
  editorLoadConfig();
  atexit(editorFreeAll);
  editorSetCursorStyle();
  editorUseAltScreen();
  editorTermSetup();

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -= 1;
  if (E.screenrows < 1) E.screenrows = 1;
}

int main(int argc, char *argv[]) {
  if (argc >= 2 && (strcmp(argv[1], "--version") == 0 ||
                    strcmp(argv[1], "-v") == 0)) {
    printf("oshi %s\n", OSHI_VERSION);
    return 0;
  }
  if (argc > 2) {
    fprintf(stderr, "usage: oshi [-v|--version] [file|folder]\n");
    return 1;
  }

  enableRawMode();
  editorInstallSignals(); /* from here on, a fatal signal restores the terminal */
  initEditor();
  if (argc >= 2 && editorIsDir(argv[1])) {
    char *picked;
    br_active = 1;
    picked = editorBrowse(argv[1], NULL);
    if (picked == NULL) editorQuit();
    editorOpen(picked);
    free(picked);
  } else if (argc >= 2) {
    editorOpen(argv[1]);
  }

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}
