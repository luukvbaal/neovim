// message.c: functions for displaying messages on the command line

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "klib/kvec.h"
#include "nvim/api/private/defs.h"
#include "nvim/api/private/helpers.h"
#include "nvim/ascii_defs.h"
#include "nvim/buffer_defs.h"
#include "nvim/channel.h"
#include "nvim/charset.h"
#include "nvim/drawscreen.h"
#include "nvim/errors.h"
#include "nvim/eval.h"
#include "nvim/eval/typval.h"
#include "nvim/eval/typval_defs.h"
#include "nvim/event/defs.h"
#include "nvim/event/loop.h"
#include "nvim/event/multiqueue.h"
#include "nvim/ex_cmds_defs.h"
#include "nvim/ex_eval.h"
#include "nvim/fileio.h"
#include "nvim/garray.h"
#include "nvim/garray_defs.h"
#include "nvim/getchar.h"
#include "nvim/gettext_defs.h"
#include "nvim/globals.h"
#include "nvim/grid.h"
#include "nvim/highlight.h"
#include "nvim/highlight_defs.h"
#include "nvim/indent.h"
#include "nvim/input.h"
#include "nvim/keycodes.h"
#include "nvim/log.h"
#include "nvim/main.h"
#include "nvim/mbyte.h"
#include "nvim/mbyte_defs.h"
#include "nvim/memory.h"
#include "nvim/message.h"
#include "nvim/mouse.h"
#include "nvim/ops.h"
#include "nvim/option.h"
#include "nvim/option_vars.h"
#include "nvim/os/fs.h"
#include "nvim/os/input.h"
#include "nvim/os/os.h"
#include "nvim/os/time.h"
#include "nvim/pos_defs.h"
#include "nvim/regexp.h"
#include "nvim/runtime.h"
#include "nvim/runtime_defs.h"
#include "nvim/state_defs.h"
#include "nvim/strings.h"
#include "nvim/types_defs.h"
#include "nvim/ui.h"
#include "nvim/ui_compositor.h"
#include "nvim/ui_defs.h"
#include "nvim/vim_defs.h"

// Magic chars used in confirm dialog strings
enum {
  DLG_BUTTON_SEP = '\n',
  DLG_HOTKEY_CHAR = '&',
};

static int confirm_msg_used = false;            // displaying confirm_msg
#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "message.c.generated.h"
#endif
static char *confirm_msg = NULL;            // ":confirm" message
static char *confirm_msg_tail;              // tail of confirm_msg

MessageHistoryEntry *first_msg_hist = NULL;
MessageHistoryEntry *last_msg_hist = NULL;
static int msg_hist_len = 0;

static FILE *verbose_fd = NULL;
static bool verbose_did_open = false;

bool keep_msg_more = false;    // keep_msg was set by msgmore()

// When writing messages to the screen, there are many different situations.
// A number of variables is used to remember the current state:
// msg_nowait       No extra delay for the last drawn message.
//                  Used in normal_cmd() before the mode message is drawn.
// emsg_on_display  There was an error message recently.  Indicates that there
//                  should be a delay before redrawing.
// keep_msg         Message to be displayed after redrawing the screen, in
//                  Normal mode main loop.
//                  This is an allocated string or NULL when not used.

// Extended msg state, currently used for external UIs with ext_messages
static const char *msg_ext_kind = NULL;
static Array *msg_ext_chunks = NULL;
static garray_T msg_ext_last_chunk = GA_INIT(sizeof(char), 40);
static sattr_T msg_ext_last_attr = -1;
static size_t msg_ext_cur_len = 0;

static bool msg_ext_history_visible = false;

/// Shouldn't clear message after leaving cmdline
static bool msg_ext_keep_after_cmdline = false;

/// Like msg() but keep it silent when 'verbosefile' is set.
int verb_msg(const char *s)
{
  verbose_enter();
  int n = msg_attr_keep(s, 0, false, false);
  verbose_leave();

  return n;
}

/// Displays the string 's' on the status line
/// When terminal not initialized (yet) printf("%s", ..) is used.
///
/// @return  true if wait_return() not called
bool msg(const char *s, const int attr)
  FUNC_ATTR_NONNULL_ARG(1)
{
  return msg_attr_keep(s, attr, false, false);
}

/// Similar to msg_outtrans, but support newlines and tabs.
void msg_multiline(const char *s, int attr, bool check_int)
  FUNC_ATTR_NONNULL_ALL
{
  const char *next_spec = s;

  while (next_spec != NULL) {
    if (check_int && got_int) {
      return;
    }
    next_spec = strpbrk(s, "\t\n\r");

    if (next_spec != NULL) {
      // Printing all char that are before the char found by strpbrk
      msg_outtrans_len(s, (int)(next_spec - s), attr);
      msg_putchar_attr((uint8_t)(*next_spec), attr);
      s = next_spec + 1;
    }
  }

  // Print the rest of the message. We know there is no special
  // character because strpbrk returned NULL
  if (*s != NUL) {
    msg_outtrans(s, attr);
  }
}

void msg_multiattr(HlMessage hl_msg, const char *kind, bool history)
{
  msg_start();
  msg_ext_set_kind(kind);
  for (uint32_t i = 0; i < kv_size(hl_msg); i++) {
    HlMessageChunk chunk = kv_A(hl_msg, i);
    msg_multiline(chunk.text.data, chunk.attr, true);
  }
  if (history && kv_size(hl_msg)) {
    add_msg_hist_multiattr(NULL, 0, 0, true, hl_msg);
  }
  msg_end();
}

/// @param keep set keep_msg if it doesn't scroll
bool msg_attr_keep(const char *s, int attr, bool keep, bool multiline)
  FUNC_ATTR_NONNULL_ALL
{
  static int entered = 0;

  if (keep && multiline) {
    // Not implemented. 'multiline' is only used by nvim-added messages,
    // which should avoid 'keep' behavior (just show the message at
    // the correct time already).
    abort();
  }

  // Skip messages not match ":filter pattern".
  // Don't filter when there is an error.
  if (!emsg_on_display && message_filtered(s)) {
    return true;
  }

  if (attr == 0) {
    set_vim_var_string(VV_STATUSMSG, s, -1);
  }

  // It is possible that displaying a messages causes a problem (e.g.,
  // when redrawing the window), which causes another message, etc..    To
  // break this loop, limit the recursiveness to 3 levels.
  if (entered >= 3) {
    return true;
  }
  entered++;

  // Add message to history (unless it's a repeated kept message or a
  // truncated message)
  if (s != keep_msg
      || (*s != '<'
          && last_msg_hist != NULL
          && last_msg_hist->msg != NULL
          && strcmp(s, last_msg_hist->msg) != 0)) {
    add_msg_hist(s, -1, attr, multiline);
  }

  msg_start();
  if (multiline) {
    msg_multiline(s, attr, false);
  } else {
    msg_outtrans(s, attr);
  }
  bool retval = msg_end();

  if (keep && retval && vim_strsize(s) < (Rows - cmdline_row - 1) * Columns + sc_col) {
    set_keep_msg(s, 0);
  }

  need_fileinfo = false;

  entered--;
  return retval;
}

/// Truncate a string "s" to "buf" with cell width "room".
/// "s" and "buf" may be equal.
void trunc_string(const char *s, char *buf, int room_in, int buflen)
{
  int room = room_in - 3;  // "..." takes 3 chars
  int len = 0;
  int e;
  int i;
  int n;

  if (*s == NUL) {
    if (buflen > 0) {
      *buf = NUL;
    }
    return;
  }

  if (room_in < 3) {
    room = 0;
  }
  int half = room / 2;

  // First part: Start of the string.
  for (e = 0; len < half && e < buflen; e++) {
    if (s[e] == NUL) {
      // text fits without truncating!
      buf[e] = NUL;
      return;
    }
    n = ptr2cells(s + e);
    if (len + n > half) {
      break;
    }
    len += n;
    buf[e] = s[e];
    for (n = utfc_ptr2len(s + e); --n > 0;) {
      if (++e == buflen) {
        break;
      }
      buf[e] = s[e];
    }
  }

  // Last part: End of the string.
  half = i = (int)strlen(s);
  while (true) {
    do {
      half = half - utf_head_off(s, s + half - 1) - 1;
    } while (half > 0 && utf_iscomposing(utf_ptr2char(s + half)));
    n = ptr2cells(s + half);
    if (len + n > room || half == 0) {
      break;
    }
    len += n;
    i = half;
  }

  if (i <= e + 3) {
    // text fits without truncating
    if (s != buf) {
      len = (int)strlen(s);
      if (len >= buflen) {
        len = buflen - 1;
      }
      len = len - e + 1;
      if (len < 1) {
        buf[e - 1] = NUL;
      } else {
        memmove(buf + e, s + e, (size_t)len);
      }
    }
  } else if (e + 3 < buflen) {
    // set the middle and copy the last part
    memmove(buf + e, "...", 3);
    len = (int)strlen(s + i) + 1;
    if (len >= buflen - e - 3) {
      len = buflen - e - 3 - 1;
    }
    memmove(buf + e + 3, s + i, (size_t)len);
    buf[e + 3 + len - 1] = NUL;
  } else {
    // can't fit in the "...", just truncate it
    buf[buflen - 1] = NUL;
  }
}

/// Shows a printf-style message with attributes.
///
/// Note: Caller must check the resulting string is shorter than IOSIZE!!!
///
/// @see semsg
/// @see swmsg
///
/// @param s printf-style format message
int smsg(int attr, const char *s, ...)
  FUNC_ATTR_PRINTF(2, 3)
{
  va_list arglist;

  va_start(arglist, s);
  vim_vsnprintf(IObuff, IOSIZE, s, arglist);
  va_end(arglist);
  return msg(IObuff, attr);
}

int smsg_attr_keep(int attr, const char *s, ...)
  FUNC_ATTR_PRINTF(2, 3)
{
  va_list arglist;

  va_start(arglist, s);
  vim_vsnprintf(IObuff, IOSIZE, s, arglist);
  va_end(arglist);
  return msg_attr_keep(IObuff, attr, true, false);
}

// Remember the last sourcing name/lnum used in an error message, so that it
// isn't printed each time when it didn't change.
static int last_sourcing_lnum = 0;
static char *last_sourcing_name = NULL;

/// Reset the last used sourcing name/lnum.  Makes sure it is displayed again
/// for the next error message;
void reset_last_sourcing(void)
{
  XFREE_CLEAR(last_sourcing_name);
  last_sourcing_lnum = 0;
}

/// @return  true if "SOURCING_NAME" differs from "last_sourcing_name".
static bool other_sourcing_name(void)
{
  if (SOURCING_NAME != NULL) {
    if (last_sourcing_name != NULL) {
      return strcmp(SOURCING_NAME, last_sourcing_name) != 0;
    }
    return true;
  }
  return false;
}

/// Get the message about the source, as used for an error message
///
/// @return [allocated] String with room for one more character. NULL when no
///                     message is to be given.
static char *get_emsg_source(void)
  FUNC_ATTR_MALLOC FUNC_ATTR_WARN_UNUSED_RESULT
{
  if (SOURCING_NAME != NULL && other_sourcing_name()) {
    char *sname = estack_sfile(ESTACK_NONE);
    char *tofree = sname;

    if (sname == NULL) {
      sname = SOURCING_NAME;
    }

    const char *const p = _("Error detected while processing %s:");
    const size_t buf_len = strlen(sname) + strlen(p) + 1;
    char *const buf = xmalloc(buf_len);
    snprintf(buf, buf_len, p, sname);
    xfree(tofree);
    return buf;
  }
  return NULL;
}

/// Get the message about the source lnum, as used for an error message.
///
/// @return [allocated] String with room for one more character. NULL when no
///                     message is to be given.
static char *get_emsg_lnum(void)
  FUNC_ATTR_MALLOC FUNC_ATTR_WARN_UNUSED_RESULT
{
  // lnum is 0 when executing a command from the command line
  // argument, we don't want a line number then
  if (SOURCING_NAME != NULL
      && (other_sourcing_name() || SOURCING_LNUM != last_sourcing_lnum)
      && SOURCING_LNUM != 0) {
    const char *const p = _("line %4" PRIdLINENR ":");
    const size_t buf_len = 20 + strlen(p);
    char *const buf = xmalloc(buf_len);
    snprintf(buf, buf_len, p, SOURCING_LNUM);
    return buf;
  }
  return NULL;
}

/// Display name and line number for the source of an error.
/// Remember the file name and line number, so that for the next error the info
/// is only displayed if it changed.
void msg_source(int attr)
{
  static bool recursive = false;

  // Bail out if something called here causes an error.
  if (recursive) {
    return;
  }
  recursive = true;

  char *p = get_emsg_source();
  if (p != NULL) {
    msg(p, attr);
    xfree(p);
  }
  p = get_emsg_lnum();
  if (p != NULL) {
    msg(p, HL_ATTR(HLF_N));
    xfree(p);
    last_sourcing_lnum = SOURCING_LNUM;      // only once for each line
  }

  // remember the last sourcing name printed, also when it's empty
  if (SOURCING_NAME == NULL || other_sourcing_name()) {
    XFREE_CLEAR(last_sourcing_name);
    if (SOURCING_NAME != NULL) {
      last_sourcing_name = xstrdup(SOURCING_NAME);
    }
  }

  recursive = false;
}

/// @return  true if not giving error messages right now:
///            If "emsg_off" is set: no error messages at the moment.
///            If "msg" is in 'debug': do error message but without side effects.
///            If "emsg_skip" is set: never do error messages.
int emsg_not_now(void)
{
  if ((emsg_off > 0 && vim_strchr(p_debug, 'm') == NULL
       && vim_strchr(p_debug, 't') == NULL)
      || emsg_skip > 0) {
    return true;
  }
  return false;
}

bool emsg_multiline(const char *s, bool multiline)
{
  bool ignore = false;

  // Skip this if not giving error messages at the moment.
  if (emsg_not_now()) {
    return true;
  }

  called_emsg++;

  // If "emsg_severe" is true: When an error exception is to be thrown,
  // prefer this message over previous messages for the same command.
  bool severe = emsg_severe;
  emsg_severe = false;

  if (!emsg_off || vim_strchr(p_debug, 't') != NULL) {
    // Cause a throw of an error exception if appropriate.  Don't display
    // the error message in this case.  (If no matching catch clause will
    // be found, the message will be displayed later on.)  "ignore" is set
    // when the message should be ignored completely (used for the
    // interrupt message).
    if (cause_errthrow(s, multiline, severe, &ignore)) {
      if (!ignore) {
        did_emsg++;
      }
      return true;
    }

    if (in_assert_fails && emsg_assert_fails_msg == NULL) {
      emsg_assert_fails_msg = xstrdup(s);
      emsg_assert_fails_lnum = SOURCING_LNUM;
      xfree(emsg_assert_fails_context);
      emsg_assert_fails_context = xstrdup(SOURCING_NAME == NULL ? "" : SOURCING_NAME);
    }

    // set "v:errmsg", also when using ":silent! cmd"
    set_vim_var_string(VV_ERRMSG, s, -1);

    // When using ":silent! cmd" ignore error messages.
    // But do write it to the redirection file.
    if (emsg_silent != 0) {
      if (!emsg_noredir) {
        msg_start();
        char *p = get_emsg_source();
        if (p != NULL) {
          const size_t p_len = strlen(p);
          p[p_len] = '\n';
          redir_write(p, (ptrdiff_t)p_len + 1);
          xfree(p);
        }
        p = get_emsg_lnum();
        if (p != NULL) {
          const size_t p_len = strlen(p);
          p[p_len] = '\n';
          redir_write(p, (ptrdiff_t)p_len + 1);
          xfree(p);
        }
        redir_write(s, (ptrdiff_t)strlen(s));
      }

      // Log (silent) errors as debug messages.
      if (SOURCING_NAME != NULL && SOURCING_LNUM != 0) {
        DLOG("(:silent) %s (%s (line %" PRIdLINENR "))",
             s, SOURCING_NAME, SOURCING_LNUM);
      } else {
        DLOG("(:silent) %s", s);
      }

      return true;
    }

    // Log editor errors as INFO.
    if (SOURCING_NAME != NULL && SOURCING_LNUM != 0) {
      ILOG("%s (%s (line %" PRIdLINENR "))", s, SOURCING_NAME, SOURCING_LNUM);
    } else {
      ILOG("%s", s);
    }

    ex_exitval = 1;

    // Reset msg_silent, an error causes messages to be switched back on.
    msg_silent = 0;
    cmd_silent = false;

    if (global_busy) {        // break :global command
      global_busy++;
    }

    if (p_eb) {
      beep_flush();           // also includes flush_buffers()
    } else {
      flush_buffers(FLUSH_MINIMAL);  // flush internal buffers
    }
    did_emsg++;               // flag for DoOneCmd()
  }

  emsg_on_display = true;     // remember there is an error message
  int attr = HL_ATTR(HLF_E);      // set highlight mode for error messages
  if (msg_ext_kind == NULL) {
    msg_ext_set_kind("emsg");
  }

  // Display name and line number for the source of the error.
  msg_source(attr);

  // Display the error message itself.
  msg_nowait = false;  // Wait for this msg.
  return msg_attr_keep(s, attr, false, multiline);
}

/// emsg() - display an error message
///
/// Rings the bell, if appropriate, and calls message() to do the real work
/// When terminal not initialized (yet) fprintf(stderr, "%s", ..) is used.
///
/// @return true if wait_return() not called
bool emsg(const char *s)
{
  return emsg_multiline(s, false);
}

void emsg_invreg(int name)
{
  semsg(_("E354: Invalid register name: '%s'"), transchar_buf(NULL, name));
}

/// Print an error message with unknown number of arguments
///
/// @return whether the message was displayed
bool semsg(const char *const fmt, ...)
  FUNC_ATTR_PRINTF(1, 2)
{
  bool ret;

  va_list ap;
  va_start(ap, fmt);
  ret = semsgv(fmt, ap);
  va_end(ap);

  return ret;
}

#define MULTILINE_BUFSIZE 8192

bool semsg_multiline(const char *const fmt, ...)
{
  bool ret;
  va_list ap;

  static char errbuf[MULTILINE_BUFSIZE];
  if (emsg_not_now()) {
    return true;
  }

  va_start(ap, fmt);
  vim_vsnprintf(errbuf, sizeof(errbuf), fmt, ap);
  va_end(ap);

  ret = emsg_multiline(errbuf, true);

  return ret;
}

/// Print an error message with unknown number of arguments
static bool semsgv(const char *fmt, va_list ap)
{
  static char errbuf[IOSIZE];
  if (emsg_not_now()) {
    return true;
  }

  vim_vsnprintf(errbuf, sizeof(errbuf), fmt, ap);

  return emsg(errbuf);
}

/// Same as emsg(...), but abort on error when ABORT_ON_INTERNAL_ERROR is
/// defined. It is used for internal errors only, so that they can be
/// detected when fuzzing vim.
void iemsg(const char *s)
{
  if (emsg_not_now()) {
    return;
  }

  emsg(s);
#ifdef ABORT_ON_INTERNAL_ERROR
  set_vim_var_string(VV_ERRMSG, s, -1);
  msg_putchar('\n');  // avoid overwriting the error message
  ui_flush();
  abort();
#endif
}

/// Same as semsg(...) but abort on error when ABORT_ON_INTERNAL_ERROR is
/// defined. It is used for internal errors only, so that they can be
/// detected when fuzzing vim.
void siemsg(const char *s, ...)
{
  if (emsg_not_now()) {
    return;
  }

  va_list ap;
  va_start(ap, s);
  semsgv(s, ap);
  va_end(ap);
#ifdef ABORT_ON_INTERNAL_ERROR
  msg_putchar('\n');  // avoid overwriting the error message
  ui_flush();
  abort();
#endif
}

/// Give an "Internal error" message.
void internal_error(const char *where)
{
  siemsg(_(e_intern2), where);
}

static void msg_semsg_event(void **argv)
{
  char *s = argv[0];
  emsg(s);
  xfree(s);
}

void msg_schedule_semsg(const char *const fmt, ...)
  FUNC_ATTR_PRINTF(1, 2)
{
  va_list ap;
  va_start(ap, fmt);
  vim_vsnprintf(IObuff, IOSIZE, fmt, ap);
  va_end(ap);

  char *s = xstrdup(IObuff);
  loop_schedule_deferred(&main_loop, event_create(msg_semsg_event, s));
}

static void msg_semsg_multiline_event(void **argv)
{
  char *s = argv[0];
  emsg_multiline(s, true);
  xfree(s);
}

void msg_schedule_semsg_multiline(const char *const fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vim_vsnprintf(IObuff, IOSIZE, fmt, ap);
  va_end(ap);

  char *s = xstrdup(IObuff);
  loop_schedule_deferred(&main_loop, event_create(msg_semsg_multiline_event, s));
}

void hl_msg_free(HlMessage hl_msg)
{
  for (size_t i = 0; i < kv_size(hl_msg); i++) {
    xfree(kv_A(hl_msg, i).text.data);
  }
  kv_destroy(hl_msg);
}

/// @param[in]  len  Length of s or -1.
static void add_msg_hist(const char *s, int len, int attr, bool multiline)
{
  add_msg_hist_multiattr(s, len, attr, multiline, (HlMessage)KV_INITIAL_VALUE);
}

static void add_msg_hist_multiattr(const char *s, int len, int attr, bool multiline,
                                   HlMessage multiattr)
{
  if (msg_hist_off || msg_silent != 0) {
    hl_msg_free(multiattr);
    return;
  }

  // Don't let the message history get too big
  while (msg_hist_len > MAX_MSG_HIST_LEN) {
    delete_first_msg();
  }

  // allocate an entry and add the message at the end of the history
  struct msg_hist *p = xmalloc(sizeof(struct msg_hist));
  if (s) {
    if (len < 0) {
      len = (int)strlen(s);
    }
    // remove leading and trailing newlines
    while (len > 0 && *s == '\n') {
      s++;
      len--;
    }
    while (len > 0 && s[len - 1] == '\n') {
      len--;
    }
    p->msg = xmemdupz(s, (size_t)len);
  } else {
    p->msg = NULL;
  }
  p->next = NULL;
  p->attr = attr;
  p->multiline = multiline;
  p->multiattr = multiattr;
  p->kind = msg_ext_kind;
  if (last_msg_hist != NULL) {
    last_msg_hist->next = p;
  }
  last_msg_hist = p;
  if (first_msg_hist == NULL) {
    first_msg_hist = last_msg_hist;
  }
  msg_hist_len++;
}

/// Delete the first (oldest) message from the history.
///
/// @return  FAIL if there are no messages.
int delete_first_msg(void)
{
  if (msg_hist_len <= 0) {
    return FAIL;
  }
  struct msg_hist *p = first_msg_hist;
  first_msg_hist = p->next;
  if (first_msg_hist == NULL) {  // history is becoming empty
    assert(msg_hist_len == 1);
    last_msg_hist = NULL;
  }
  xfree(p->msg);
  hl_msg_free(p->multiattr);
  xfree(p);
  msg_hist_len--;
  return OK;
}

/// :messages command implementation
void ex_messages(exarg_T *eap)
  FUNC_ATTR_NONNULL_ALL
{
  if (strcmp(eap->arg, "clear") == 0) {
    int keep = eap->addr_count == 0 ? 0 : eap->line2;

    while (msg_hist_len > keep) {
      delete_first_msg();
    }
    return;
  }

  if (*eap->arg != NUL) {
    emsg(_(e_invarg));
    return;
  }

  struct msg_hist *p = first_msg_hist;

  if (eap->addr_count != 0) {
    int c = 0;
    // Count total messages
    for (; p != NULL && !got_int; p = p->next) {
      c++;
    }

    c -= eap->line2;

    // Skip without number of messages specified
    for (p = first_msg_hist; p != NULL && !got_int && c > 0; p = p->next, c--) {}
  }

  // Display what was not skipped.
  if (msg_silent) {
    return;
  }
  Array entries = ARRAY_DICT_INIT;
  for (; p != NULL; p = p->next) {
    if (kv_size(p->multiattr) || (p->msg && p->msg[0])) {
      Array entry = ARRAY_DICT_INIT;
      ADD(entry, CSTR_TO_OBJ(p->kind));
      Array content = ARRAY_DICT_INIT;
      if (kv_size(p->multiattr)) {
        for (uint32_t i = 0; i < kv_size(p->multiattr); i++) {
          HlMessageChunk chunk = kv_A(p->multiattr, i);
          Array content_entry = ARRAY_DICT_INIT;
          ADD(content_entry, INTEGER_OBJ(chunk.attr));
          ADD(content_entry, STRING_OBJ(copy_string(chunk.text, NULL)));
          ADD(content, ARRAY_OBJ(content_entry));
        }
      } else if (p->msg && p->msg[0]) {
        Array content_entry = ARRAY_DICT_INIT;
        ADD(content_entry, INTEGER_OBJ(p->attr));
        ADD(content_entry, CSTR_TO_OBJ(p->msg));
        ADD(content, ARRAY_OBJ(content_entry));
      }
      ADD(entry, ARRAY_OBJ(content));
      ADD(entries, ARRAY_OBJ(entry));
    }
  }
  ui_call_msg_history_show(entries);
  api_free_array(entries);
  msg_ext_history_visible = true;
}

/// Set "keep_msg" to "s".  Free the old value and check for NULL pointer.
void set_keep_msg(const char *s, int attr)
{
  xfree(keep_msg);
  if (s != NULL && msg_silent == 0) {
    keep_msg = xstrdup(s);
  } else {
    keep_msg = NULL;
  }
  keep_msg_more = false;
  keep_msg_attr = attr;
}

/// Return true if printing messages should currently be done.
bool messaging(void)
{
  // TODO(bfredl): with general support for "async" messages with p_ch,
  // this should be re-enabled.
  return !(p_lz && char_avail() && !KeyTyped);
}

void msgmore(int n)
{
  int pn;

  if (global_busy           // no messages now, wait until global is finished
      || !messaging()) {      // 'lazyredraw' set, don't do messages now
    return;
  }

  // We don't want to overwrite another important message, but do overwrite
  // a previous "more lines" or "fewer lines" message, so that "5dd" and
  // then "put" reports the last action.
  if (keep_msg != NULL && !keep_msg_more) {
    return;
  }

  if (n > 0) {
    pn = n;
  } else {
    pn = -n;
  }

  if (pn > p_report) {
    if (n > 0) {
      vim_snprintf(msg_buf, MSG_BUF_LEN,
                   NGETTEXT("%d more line", "%d more lines", pn),
                   pn);
    } else {
      vim_snprintf(msg_buf, MSG_BUF_LEN,
                   NGETTEXT("%d line less", "%d fewer lines", pn),
                   pn);
    }
    if (got_int) {
      xstrlcat(msg_buf, _(" (Interrupted)"), MSG_BUF_LEN);
    }
    if (msg(msg_buf, 0)) {
      set_keep_msg(msg_buf, 0);
      keep_msg_more = true;
    }
  }
}

void msg_ext_set_kind(const char *msg_kind)
{
  // Don't change the label of an existing batch:
  msg_ext_ui_flush();

  // TODO(bfredl): would be nice to avoid dynamic scoping, but that would
  // need refactoring the msg_ interface to not be "please pretend nvim is
  // a terminal for a moment"
  msg_ext_kind = msg_kind;
}

/// Prepare for outputting characters in the command line.
void msg_start(void)
{
  bool did_return = false;

  if (!msg_silent) {
    XFREE_CLEAR(keep_msg);              // don't display old message now
    need_fileinfo = false;
  }

  msg_ext_ui_flush();

  // When redirecting, may need to start a new line.
  if (!did_return) {
    redir_write("\n", 1);
  }
}

void msg_putchar(int c)
{
  msg_putchar_attr(c, 0);
}

void msg_putchar_attr(int c, int attr)
{
  char buf[MB_MAXCHAR + 1];

  if (IS_SPECIAL(c)) {
    buf[0] = (char)K_SPECIAL;
    buf[1] = (char)K_SECOND(c);
    buf[2] = (char)K_THIRD(c);
    buf[3] = NUL;
  } else {
    buf[utf_char2bytes(c, buf)] = NUL;
  }
  msg_puts_attr(buf, attr);
}

void msg_outnum(int n)
{
  char buf[20];

  snprintf(buf, sizeof(buf), "%d", n);
  msg_puts(buf);
}

void msg_home_replace(const char *fname)
{
  msg_home_replace_attr(fname, 0);
}

void msg_home_replace_hl(const char *fname)
{
  msg_home_replace_attr(fname, HL_ATTR(HLF_D));
}

static void msg_home_replace_attr(const char *fname, int attr)
{
  char *name = home_replace_save(NULL, fname);
  msg_outtrans(name, attr);
  xfree(name);
}

/// Output 'len' characters in 'str' (including NULs) with translation
/// if 'len' is -1, output up to a NUL character.
/// Use attributes 'attr'.
///
/// @return  the number of characters it takes on the screen.
int msg_outtrans(const char *str, int attr)
{
  return msg_outtrans_len(str, (int)strlen(str), attr);
}

/// Output one character at "p".
/// Handles multi-byte characters.
///
/// @return  pointer to the next character.
const char *msg_outtrans_one(const char *p, int attr)
{
  int l;

  if ((l = utfc_ptr2len(p)) > 1) {
    msg_outtrans_len(p, l, attr);
    return p + l;
  }
  msg_puts_attr(transchar_byte_buf(NULL, (uint8_t)(*p)), attr);
  return p + 1;
}

int msg_outtrans_len(const char *msgstr, int len, int attr)
{
  int retval = 0;
  const char *str = msgstr;
  const char *plain_start = msgstr;
  char *s;
  int c;
  int save_got_int = got_int;

  // Only quit when got_int was set in here.
  got_int = false;

  // if MSG_HIST flag set, add message to history
  if (attr & MSG_HIST) {
    add_msg_hist(str, len, attr, false);
    attr &= ~MSG_HIST;
  }

  // Go over the string.  Special characters are translated and printed.
  // Normal characters are printed several at a time.
  while (--len >= 0 && !got_int) {
    // Don't include composing chars after the end.
    int mb_l = utfc_ptr2len_len(str, len + 1);
    if (mb_l > 1) {
      c = utf_ptr2char(str);
      if (vim_isprintc(c)) {
        // Printable multi-byte char: count the cells.
        retval += utf_ptr2cells(str);
      } else {
        // Unprintable multi-byte char: print the printable chars so
        // far and the translation of the unprintable char.
        if (str > plain_start) {
          msg_puts_len(plain_start, str - plain_start, attr);
        }
        plain_start = str + mb_l;
        msg_puts_attr(transchar_buf(NULL, c), attr == 0 ? HL_ATTR(HLF_8) : attr);
        retval += char2cells(c);
      }
      len -= mb_l - 1;
      str += mb_l;
    } else {
      s = transchar_byte_buf(NULL, (uint8_t)(*str));
      if (s[1] != NUL) {
        // Unprintable char: print the printable chars so far and the
        // translation of the unprintable char.
        if (str > plain_start) {
          msg_puts_len(plain_start, str - plain_start, attr);
        }
        plain_start = str + 1;
        msg_puts_attr(s, attr == 0 ? HL_ATTR(HLF_8) : attr);
        retval += (int)strlen(s);
      } else {
        retval++;
      }
      str++;
    }
  }

  if (str > plain_start && !got_int) {
    // Print the printable chars at the end.
    msg_puts_len(plain_start, str - plain_start, attr);
  }

  got_int |= save_got_int;

  return retval;
}

void msg_make(const char *arg)
{
  int i;
  static const char *str = "eeffoc";
  static const char *rs = "Plon#dqg#vxjduB";

  arg = skipwhite(arg);
  for (i = 5; *arg && i >= 0; i--) {
    if (*arg++ != str[i]) {
      break;
    }
  }
  if (i < 0) {
    msg_putchar('\n');
    for (i = 0; rs[i]; i++) {
      msg_putchar(rs[i] - 3);
    }
  }
}

/// Output the string 'str' up to a NUL character.
/// Return the number of characters it takes on the screen.
///
/// If K_SPECIAL is encountered, then it is taken in conjunction with the
/// following character and shown as <F1>, <S-Up> etc.  Any other character
/// which is not printable shown in <> form.
/// If 'from' is true (lhs of a mapping), a space is shown as <Space>.
/// If a character is displayed in one of these special ways, is also
/// highlighted (its highlight name is '8' in the p_hl variable).
/// Otherwise characters are not highlighted.
/// This function is used to show mappings, where we want to see how to type
/// the character/string -- webb
///
/// @param from  true for LHS of a mapping
/// @param maxlen  screen columns, 0 for unlimited
int msg_outtrans_special(const char *strstart, bool from, int maxlen)
{
  if (strstart == NULL) {
    return 0;  // Do nothing.
  }
  const char *str = strstart;
  int retval = 0;
  int attr = HL_ATTR(HLF_8);

  while (*str != NUL) {
    const char *text;
    // Leading and trailing spaces need to be displayed in <> form.
    if ((str == strstart || str[1] == NUL) && *str == ' ') {
      text = "<Space>";
      str++;
    } else {
      text = str2special(&str, from, false);
    }
    if (text[0] != NUL && text[1] == NUL) {
      // single-byte character or illegal byte
      text = transchar_byte_buf(NULL, (uint8_t)text[0]);
    }
    const int len = vim_strsize(text);
    if (maxlen > 0 && retval + len >= maxlen) {
      break;
    }
    // Highlight special keys
    msg_puts_attr(text, (len > 1
                         && utfc_ptr2len(text) <= 1
                         ? attr : 0));
    retval += len;
  }
  return retval;
}

/// Convert string, replacing key codes with printables
///
/// Used for lhs or rhs of mappings.
///
/// @param[in]  str  String to convert.
/// @param[in]  replace_spaces  Convert spaces into `<Space>`, normally used for
///                             lhs of mapping and keytrans(), but not rhs.
/// @param[in]  replace_lt  Convert `<` into `<lt>`.
///
/// @return [allocated] Converted string.
char *str2special_save(const char *const str, const bool replace_spaces, const bool replace_lt)
  FUNC_ATTR_NONNULL_ALL FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_MALLOC
  FUNC_ATTR_NONNULL_RET
{
  garray_T ga;
  ga_init(&ga, 1, 40);

  const char *p = str;
  while (*p != NUL) {
    ga_concat(&ga, str2special(&p, replace_spaces, replace_lt));
  }
  ga_append(&ga, NUL);
  return (char *)ga.ga_data;
}

/// Convert string, replacing key codes with printables
///
/// Used for lhs or rhs of mappings.
///
/// @param[in]  str  String to convert.
/// @param[in]  replace_spaces  Convert spaces into `<Space>`, normally used for
///                             lhs of mapping and keytrans(), but not rhs.
/// @param[in]  replace_lt  Convert `<` into `<lt>`.
///
/// @return [allocated] Converted string.
char *str2special_arena(const char *str, bool replace_spaces, bool replace_lt, Arena *arena)
  FUNC_ATTR_NONNULL_ALL FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_MALLOC
  FUNC_ATTR_NONNULL_RET
{
  const char *p = str;
  size_t len = 0;
  while (*p) {
    len += strlen(str2special(&p, replace_spaces, replace_lt));
  }

  char *buf = arena_alloc(arena, len + 1, false);
  size_t pos = 0;
  p = str;
  while (*p) {
    const char *s = str2special(&p, replace_spaces, replace_lt);
    size_t s_len = strlen(s);
    memcpy(buf + pos, s, s_len);
    pos += s_len;
  }
  buf[pos] = NUL;
  return buf;
}

/// Convert character, replacing key with printable representation.
///
/// @param[in,out]  sp  String to convert. Is advanced to the next key code.
/// @param[in]  replace_spaces  Convert spaces into `<Space>`, normally used for
///                             lhs of mapping and keytrans(), but not rhs.
/// @param[in]  replace_lt  Convert `<` into `<lt>`.
///
/// @return Converted key code, in a static buffer. Buffer is always one and the
///         same, so save converted string somewhere before running str2special
///         for the second time.
///         On illegal byte return a string with only that byte.
const char *str2special(const char **const sp, const bool replace_spaces, const bool replace_lt)
  FUNC_ATTR_NONNULL_ALL FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_NONNULL_RET
{
  static char buf[7];

  {
    // Try to un-escape a multi-byte character.  Return the un-escaped
    // string if it is a multi-byte character.
    const char *const p = mb_unescape(sp);
    if (p != NULL) {
      return p;
    }
  }

  const char *str = *sp;
  int c = (uint8_t)(*str);
  int modifiers = 0;
  bool special = false;
  if (c == K_SPECIAL && str[1] != NUL && str[2] != NUL) {
    if ((uint8_t)str[1] == KS_MODIFIER) {
      modifiers = (uint8_t)str[2];
      str += 3;
      c = (uint8_t)(*str);
    }
    if (c == K_SPECIAL && str[1] != NUL && str[2] != NUL) {
      c = TO_SPECIAL((uint8_t)str[1], (uint8_t)str[2]);
      str += 2;
    }
    if (IS_SPECIAL(c) || modifiers) {  // Special key.
      special = true;
    }
  }

  if (!IS_SPECIAL(c) && MB_BYTE2LEN(c) > 1) {
    *sp = str;
    // Try to un-escape a multi-byte character after modifiers.
    const char *p = mb_unescape(sp);
    if (p != NULL) {
      // Since 'special' is true the multi-byte character 'c' will be
      // processed by get_special_key_name().
      c = utf_ptr2char(p);
    } else {
      // illegal byte
      *sp = str + 1;
    }
  } else {
    // single-byte character, NUL or illegal byte
    *sp = str + (*str == NUL ? 0 : 1);
  }

  // Make special keys and C0 control characters in <> form, also <M-Space>.
  if (special
      || c < ' '
      || (replace_spaces && c == ' ')
      || (replace_lt && c == '<')) {
    return get_special_key_name(c, modifiers);
  }
  buf[0] = (char)c;
  buf[1] = NUL;
  return buf;
}

/// Convert string, replacing key codes with printables
///
/// @param[in]  str  String to convert.
/// @param[out]  buf  Buffer to save results to.
/// @param[in]  len  Buffer length.
void str2specialbuf(const char *sp, char *buf, size_t len)
  FUNC_ATTR_NONNULL_ALL
{
  while (*sp) {
    const char *s = str2special(&sp, false, false);
    const size_t s_len = strlen(s);
    if (len <= s_len) {
      break;
    }
    memcpy(buf, s, s_len);
    buf += s_len;
    len -= s_len;
  }
  *buf = NUL;
}

/// print line for :print or :list command
void msg_prt_line(const char *s, bool list)
{
  schar_T sc;
  int col = 0;
  int n_extra = 0;
  schar_T sc_extra = 0;
  schar_T sc_final = 0;
  const char *p_extra = NULL;  // init to make SASC shut up. ASCII only!
  int n;
  int attr = 0;
  const char *lead = NULL;
  bool in_multispace = false;
  int multispace_pos = 0;
  const char *trail = NULL;
  int l;

  if (curwin->w_p_list) {
    list = true;
  }

  if (list) {
    // find start of trailing whitespace
    if (curwin->w_p_lcs_chars.trail) {
      trail = s + strlen(s);
      while (trail > s && ascii_iswhite(trail[-1])) {
        trail--;
      }
    }
    // find end of leading whitespace
    if (curwin->w_p_lcs_chars.lead || curwin->w_p_lcs_chars.leadmultispace != NULL) {
      lead = s;
      while (ascii_iswhite(lead[0])) {
        lead++;
      }
      // in a line full of spaces all of them are treated as trailing
      if (*lead == NUL) {
        lead = NULL;
      }
    }
  }

  // output a space for an empty line, otherwise the line will be overwritten
  if (*s == NUL && !(list && curwin->w_p_lcs_chars.eol != NUL)) {
    msg_putchar(' ');
  }

  while (!got_int) {
    if (n_extra > 0) {
      n_extra--;
      if (n_extra == 0 && sc_final) {
        sc = sc_final;
      } else if (sc_extra) {
        sc = sc_extra;
      } else {
        assert(p_extra != NULL);
        sc = schar_from_ascii((unsigned char)(*p_extra++));
      }
    } else if ((l = utfc_ptr2len(s)) > 1) {
      col += utf_ptr2cells(s);
      char buf[MB_MAXBYTES + 1];
      if (l >= MB_MAXBYTES) {
        xstrlcpy(buf, "?", sizeof(buf));
      } else if (curwin->w_p_lcs_chars.nbsp != NUL && list
                 && (utf_ptr2char(s) == 160 || utf_ptr2char(s) == 0x202f)) {
        schar_get(buf, curwin->w_p_lcs_chars.nbsp);
      } else {
        memmove(buf, s, (size_t)l);
        buf[l] = NUL;
      }
      msg_puts(buf);
      s += l;
      continue;
    } else {
      attr = 0;
      int c = (uint8_t)(*s++);
      sc_extra = NUL;
      sc_final = NUL;
      if (list) {
        in_multispace = c == ' ' && (*s == ' '
                                     || (col > 0 && s[-2] == ' '));
        if (!in_multispace) {
          multispace_pos = 0;
        }
      }
      if (c == TAB && (!list || curwin->w_p_lcs_chars.tab1)) {
        // tab amount depends on current column
        n_extra = tabstop_padding(col, curbuf->b_p_ts,
                                  curbuf->b_p_vts_array) - 1;
        if (!list) {
          sc = schar_from_ascii(' ');
          sc_extra = schar_from_ascii(' ');
        } else {
          sc = (n_extra == 0 && curwin->w_p_lcs_chars.tab3)
               ? curwin->w_p_lcs_chars.tab3
               : curwin->w_p_lcs_chars.tab1;
          sc_extra = curwin->w_p_lcs_chars.tab2;
          sc_final = curwin->w_p_lcs_chars.tab3;
          attr = HL_ATTR(HLF_0);
        }
      } else if (c == NUL && list && curwin->w_p_lcs_chars.eol != NUL) {
        p_extra = "";
        n_extra = 1;
        sc = curwin->w_p_lcs_chars.eol;
        attr = HL_ATTR(HLF_AT);
        s--;
      } else if (c != NUL && (n = byte2cells(c)) > 1) {
        n_extra = n - 1;
        p_extra = transchar_byte_buf(NULL, c);
        sc = schar_from_ascii(*p_extra++);
        // Use special coloring to be able to distinguish <hex> from
        // the same in plain text.
        attr = HL_ATTR(HLF_0);
      } else if (c == ' ') {
        if (lead != NULL && s <= lead && in_multispace
            && curwin->w_p_lcs_chars.leadmultispace != NULL) {
          sc = curwin->w_p_lcs_chars.leadmultispace[multispace_pos++];
          if (curwin->w_p_lcs_chars.leadmultispace[multispace_pos] == NUL) {
            multispace_pos = 0;
          }
          attr = HL_ATTR(HLF_0);
        } else if (lead != NULL && s <= lead && curwin->w_p_lcs_chars.lead != NUL) {
          sc = curwin->w_p_lcs_chars.lead;
          attr = HL_ATTR(HLF_0);
        } else if (trail != NULL && s > trail) {
          sc = curwin->w_p_lcs_chars.trail;
          attr = HL_ATTR(HLF_0);
        } else if (in_multispace
                   && curwin->w_p_lcs_chars.multispace != NULL) {
          sc = curwin->w_p_lcs_chars.multispace[multispace_pos++];
          if (curwin->w_p_lcs_chars.multispace[multispace_pos] == NUL) {
            multispace_pos = 0;
          }
          attr = HL_ATTR(HLF_0);
        } else if (list && curwin->w_p_lcs_chars.space != NUL) {
          sc = curwin->w_p_lcs_chars.space;
          attr = HL_ATTR(HLF_0);
        } else {
          sc = schar_from_ascii(' ');  // SPACE!
        }
      } else {
        sc = schar_from_ascii(c);
      }
    }

    if (sc == NUL) {
      break;
    }

    // TODO(bfredl): this is such baloney. need msg_put_schar
    char buf[MAX_SCHAR_SIZE];
    schar_get(buf, sc);
    msg_puts_attr(buf, attr);
    col++;
  }
}

/// Output a string without attributes.
void msg_puts(const char *s)
{
  msg_puts_attr(s, 0);
}

void msg_puts_title(const char *s)
{
  msg_puts_attr(s, HL_ATTR(HLF_T));
}

/// Basic function for writing a message with highlight attributes.
void msg_puts_attr(const char *const s, const int attr)
{
  msg_puts_len(s, -1, attr);
}

/// Write a message with highlight attributes
///
/// @param[in]  str  NUL-terminated message string.
/// @param[in]  len  Length of the string or -1.
/// @param[in]  attr  Highlight attribute.
void msg_puts_len(const char *const str, const ptrdiff_t len, int attr)
  FUNC_ATTR_NONNULL_ALL
{
  assert(len < 0 || memchr(str, 0, (size_t)len) == NULL);
  // If redirection is on, also write to the redirection file.
  redir_write(str, len);

  // Don't print anything when using ":silent cmd".
  if (msg_silent != 0) {
    return;
  }

  // if MSG_HIST flag set, add message to history
  if (attr & MSG_HIST) {
    add_msg_hist(str, (int)len, attr, false);
    attr &= ~MSG_HIST;
  }

  // If there is no valid screen, use fprintf so we can see error messages.
  // If termcap is not active, we may be writing in an alternate console
  // window, cursor positioning may not work correctly (window size may be
  // different, e.g. for Win32 console) or we just don't know where the
  // cursor is.
  if (msg_use_printf()) {
    msg_puts_printf(str, len);
  }
  if (!msg_use_printf() || (headless_mode && default_grid.chars)) {
    // Concatenate pieces with the same highlight.
    if (attr != msg_ext_last_attr) {
      msg_ext_emit_chunk();
      msg_ext_last_attr = attr;
    }
    size_t concat_len = strnlen(str, (size_t)len);
    ga_concat_len(&msg_ext_last_chunk, str, concat_len);
    msg_ext_cur_len += (size_t)len;
  }

  need_fileinfo = false;
}

/// Print a formatted message
///
/// Message printed is limited by #IOSIZE. Must not be used from inside
/// msg_puts_attr().
///
/// @param[in]  attr  Highlight attributes.
/// @param[in]  fmt  Format string.
void msg_printf_attr(const int attr, const char *const fmt, ...)
  FUNC_ATTR_NONNULL_ARG(2) FUNC_ATTR_PRINTF(2, 3)
{
  static char msgbuf[IOSIZE];

  va_list ap;
  va_start(ap, fmt);
  const size_t len = (size_t)vim_vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
  va_end(ap);
  msg_puts_len(msgbuf, (ptrdiff_t)len, attr);
}

static void msg_ext_emit_chunk(void)
{
  if (msg_ext_chunks == NULL) {
    msg_ext_init_chunks();
  }
  // Color was changed or a message flushed, end current chunk.
  if (msg_ext_last_attr == -1) {
    return;  // no chunk
  }
  Array chunk = ARRAY_DICT_INIT;
  ADD(chunk, INTEGER_OBJ(msg_ext_last_attr));
  msg_ext_last_attr = -1;
  String text = ga_take_string(&msg_ext_last_chunk);
  ADD(chunk, STRING_OBJ(text));
  ADD(*msg_ext_chunks, ARRAY_OBJ(chunk));
}

/// @return  true when ":filter pattern" was used and "msg" does not match
///          "pattern".
bool message_filtered(const char *msg)
{
  if (cmdmod.cmod_filter_regmatch.regprog == NULL) {
    return false;
  }

  bool match = vim_regexec(&cmdmod.cmod_filter_regmatch, msg, 0);
  return cmdmod.cmod_filter_force ? match : !match;
}

/// @return  true when messages should be printed to stdout/stderr:
///          - "batch mode" ("silent mode", -es/-Es)
///          - no UI and not embedded
int msg_use_printf(void)
{
  return !embedded_mode && !ui_active();
}

/// Print a message when there is no valid screen.
static void msg_puts_printf(const char *str, const ptrdiff_t maxlen)
{
  const char *s = str;
  char buf[7];
  char *p;

  if (on_print.type != kCallbackNone) {
    typval_T argv[1];
    argv[0].v_type = VAR_STRING;
    argv[0].v_lock = VAR_UNLOCKED;
    argv[0].vval.v_string = (char *)str;
    typval_T rettv = TV_INITIAL_VALUE;
    callback_call(&on_print, 1, argv, &rettv);
    tv_clear(&rettv);
    return;
  }

  while ((maxlen < 0 || s - str < maxlen) && *s != NUL) {
    int len = utf_ptr2len(s);
    if (!(silent_mode && p_verbose == 0)) {
      // NL --> CR NL translation (for Unix, not for "--version")
      p = &buf[0];
      if (*s == '\n' && !info_message) {
        *p++ = '\r';
      }
      memcpy(p, s, (size_t)len);
      *(p + len) = NUL;
      if (info_message) {
        printf("%s", buf);
      } else {
        fprintf(stderr, "%s", buf);
      }
    }

    s += len;
  }
}

/// end putting a message on the screen
/// call wait_return() if the message does not fit in the available space
///
/// @return  true if wait_return() not called.
bool msg_end(void)
{
  // NOTE: ui_flush() used to be called here. This had to be removed, as it
  // inhibited substantial performance improvements. It is assumed that relevant
  // callers invoke ui_flush() before going into CPU busywork, or restricted
  // event processing after displaying a message to the user.
  msg_ext_ui_flush();
  return true;
}

/// Clear "msg_ext_chunks" before flushing so that ui_flush() does not re-emit
/// the same message recursively.
static Array *msg_ext_init_chunks(void)
{
  Array *tofree = msg_ext_chunks;
  msg_ext_chunks = xcalloc(1, sizeof(*msg_ext_chunks));
  msg_ext_cur_len = 0;
  return tofree;
}

void msg_ext_ui_flush(void)
{
  msg_ext_emit_chunk();
  if (msg_ext_chunks->size > 0) {
    Array *tofree = msg_ext_init_chunks();
    ui_call_msg_show(cstr_as_string(msg_ext_kind), *tofree, false);
    api_free_array(*tofree);
    xfree(tofree);
    msg_ext_kind = NULL;
  }
}

void msg_ext_flush_showmode(void)
{
  // Showmode messages doesn't interrupt normal message flow, so we use
  // separate event. Still reuse the same chunking logic, for simplicity.
  if (ui_has(kUIMessages)) {
    msg_ext_emit_chunk();
    Array *tofree = msg_ext_init_chunks();
    ui_call_msg_showmode(*tofree);
    api_free_array(*tofree);
    xfree(tofree);
  }
}

void msg_ext_clear(bool force)
{
  if ((!msg_ext_keep_after_cmdline || force)) {
    ui_call_msg_clear();
  }
  if (msg_ext_history_visible) {
    ui_call_msg_history_clear();
    msg_ext_history_visible = false;
  }

  // Only keep once.
  msg_ext_keep_after_cmdline = false;
}

void msg_ext_clear_later(void)
{
  msg_ext_need_clear = true;
  set_must_redraw(UPD_VALID);
}

void msg_ext_check_clear(void)
{
  // Redraw after cmdline or prompt is expected to clear messages.
  if (msg_ext_need_clear) {
    msg_ext_clear(true);
    msg_ext_need_clear = false;
  }
}

/// May write a string to the redirection file.
///
/// @param maxlen  if -1, write the whole string, otherwise up to "maxlen" bytes.
static void redir_write(const char *const str, const ptrdiff_t maxlen)
{
  const char *s = str;
  static size_t cur_col = 0;

  if (maxlen == 0) {
    return;
  }

  // Don't do anything for displaying prompts and the like.
  if (redir_off) {
    return;
  }

  // If 'verbosefile' is set prepare for writing in that file.
  if (*p_vfile != NUL && verbose_fd == NULL) {
    verbose_open();
  }

  if (redirecting()) {
    // If the string doesn't start with CR or NL, go to msg_ext_cur_len
    if (*s != '\n' && *s != '\r') {
      while (cur_col < msg_ext_cur_len) {
        if (capture_ga) {
          ga_concat_len(capture_ga, " ", 1);
        }
        if (redir_reg) {
          write_reg_contents(redir_reg, " ", 1, true);
        } else if (redir_vname) {
          var_redir_str(" ", -1);
        } else if (redir_fd != NULL) {
          fputs(" ", redir_fd);
        }
        if (verbose_fd != NULL) {
          fputs(" ", verbose_fd);
        }
        cur_col++;
      }
    }

    size_t len = maxlen == -1 ? strlen(s) : (size_t)maxlen;
    if (capture_ga) {
      ga_concat_len(capture_ga, str, len);
    }
    if (redir_reg) {
      write_reg_contents(redir_reg, s, (ssize_t)len, true);
    }
    if (redir_vname) {
      var_redir_str(s, (int)maxlen);
    }

    // Write and adjust the current column.
    while (*s != NUL
           && (maxlen < 0 || (int)(s - str) < maxlen)) {
      if (!redir_reg && !redir_vname && !capture_ga) {
        if (redir_fd != NULL) {
          putc(*s, redir_fd);
        }
      }
      if (verbose_fd != NULL) {
        putc(*s, verbose_fd);
      }
      if (*s == '\r' || *s == '\n') {
        cur_col = 0;
      } else if (*s == '\t') {
        cur_col += (8 - cur_col % 8);
      } else {
        cur_col++;
      }
      s++;
    }
  }
}

int redirecting(void)
{
  return redir_fd != NULL || *p_vfile != NUL
         || redir_reg || redir_vname || capture_ga != NULL;
}

/// Before giving verbose message.
/// Must always be called paired with verbose_leave()!
void verbose_enter(void)
{
  if (*p_vfile != NUL) {
    msg_silent++;
  }
}

/// After giving verbose message.
/// Must always be called paired with verbose_enter()!
void verbose_leave(void)
{
  if (*p_vfile != NUL) {
    if (--msg_silent < 0) {
      msg_silent = 0;
    }
  }
}

/// Called when 'verbosefile' is set: stop writing to the file.
void verbose_stop(void)
{
  if (verbose_fd != NULL) {
    fclose(verbose_fd);
    verbose_fd = NULL;
  }
  verbose_did_open = false;
}

/// Open the file 'verbosefile'.
///
/// @return  FAIL or OK.
int verbose_open(void)
{
  if (verbose_fd == NULL && !verbose_did_open) {
    // Only give the error message once.
    verbose_did_open = true;

    verbose_fd = os_fopen(p_vfile, "a");
    if (verbose_fd == NULL) {
      semsg(_(e_notopen), p_vfile);
      return FAIL;
    }
  }
  return OK;
}

/// Give a warning message (for searching).
/// Use 'w' highlighting and may repeat the message after redrawing
void give_warning(const char *message, bool hl)
  FUNC_ATTR_NONNULL_ARG(1)
{
  // Don't do this for ":silent".
  if (msg_silent != 0) {
    return;
  }

  set_vim_var_string(VV_WARNINGMSG, message, -1);
  XFREE_CLEAR(keep_msg);
  if (hl) {
    keep_msg_attr = HL_ATTR(HLF_W);
  } else {
    keep_msg_attr = 0;
  }

  if (msg_ext_kind == NULL) {
    msg_ext_set_kind("wmsg");
  }

  if (msg(message, keep_msg_attr)) {
    set_keep_msg(message, keep_msg_attr);
  }
}

/// Shows a warning, with optional highlighting.
///
/// @param hl enable highlighting
/// @param fmt printf-style format message
///
/// @see smsg
/// @see semsg
void swmsg(bool hl, const char *const fmt, ...)
  FUNC_ATTR_PRINTF(2, 3)
{
  va_list args;

  va_start(args, fmt);
  vim_vsnprintf(IObuff, IOSIZE, fmt, args);
  va_end(args);

  give_warning(IObuff, hl);
}

/// Advance msg cursor to column "col".
void msg_advance(int col)
{
  if (msg_silent != 0) {        // nothing to advance to
    return;
  }
  // TODO(bfredl): use byte count as a basic proxy.
  // later on we might add proper support for formatted messages.
  while (msg_ext_cur_len < (size_t)col) {
    msg_putchar(' ');
  }
}

/// Used for "confirm()" function, and the :confirm command prefix.
/// Versions which haven't got flexible dialogs yet, and console
/// versions, get this generic handler which uses the command line.
///
/// type  = one of:
///         VIM_QUESTION, VIM_INFO, VIM_WARNING, VIM_ERROR or VIM_GENERIC
/// title = title string (can be NULL for default)
/// (neither used in console dialogs at the moment)
///
/// Format of the "buttons" string:
/// "Button1Name\nButton2Name\nButton3Name"
/// The first button should normally be the default/accept
/// The second button should be the 'Cancel' button
/// Other buttons- use your imagination!
/// A '&' in a button name becomes a shortcut, so each '&' should be before a
/// different letter.
///
/// @param textfiel  IObuff for inputdialog(), NULL otherwise
/// @param ex_cmd  when true pressing : accepts default and starts Ex command
/// @returns 0 if cancelled, otherwise the nth button (1-indexed).
int do_dialog(int type, const char *title, const char *message, const char *buttons, int dfltbutton,
              const char *textfield, int ex_cmd)
{
  int retval = 0;
  int i;

  if (silent_mode) {  // No dialogs in silent mode ("ex -s")
    return dfltbutton;  // return default option
  }

  int save_msg_silent = msg_silent;
  int oldState = State;

  msg_silent = 0;  // If dialog prompts for input, user needs to see it! #8788
  State = MODE_CONFIRM;
  setmouse();

  char *hotkeys = msg_show_console_dialog(message, buttons, dfltbutton);

  while (true) {
    // Without a UI Nvim waits for input forever.
    if (!ui_active() && !input_available()) {
      retval = dfltbutton;
      break;
    }

    // Get a typed character directly from the user.
    int c = get_keystroke(NULL);
    switch (c) {
    case CAR:                 // User accepts default option
    case NL:
      retval = dfltbutton;
      break;
    case Ctrl_C:              // User aborts/cancels
    case ESC:
      retval = 0;
      break;
    default:                  // Could be a hotkey?
      if (c < 0) {            // special keys are ignored here
        continue;
      }
      if (c == ':' && ex_cmd) {
        retval = dfltbutton;
        ins_char_typebuf(':', 0, false);
        break;
      }

      // Make the character lowercase, as chars in "hotkeys" are.
      c = mb_tolower(c);
      retval = 1;
      for (i = 0; hotkeys[i]; i++) {
        if (utf_ptr2char(hotkeys + i) == c) {
          break;
        }
        i += utfc_ptr2len(hotkeys + i) - 1;
        retval++;
      }
      if (hotkeys[i]) {
        break;
      }
      // No hotkey match, so keep waiting
      continue;
    }
    break;
  }

  xfree(hotkeys);

  msg_silent = save_msg_silent;
  State = oldState;
  setmouse();

  return retval;
}

/// Copy one character from "*from" to "*to", taking care of multi-byte
/// characters.  Return the length of the character in bytes.
///
/// @param lowercase  make character lower case
static int copy_char(const char *from, char *to, bool lowercase)
  FUNC_ATTR_NONNULL_ALL
{
  if (lowercase) {
    int c = mb_tolower(utf_ptr2char(from));
    return utf_char2bytes(c, to);
  }
  int len = utfc_ptr2len(from);
  memmove(to, from, (size_t)len);
  return len;
}

#define HAS_HOTKEY_LEN 30
#define HOTK_LEN MB_MAXBYTES

/// Allocates memory for dialog string & for storing hotkeys
///
/// Finds the size of memory required for the confirm_msg & for storing hotkeys
/// and then allocates the memory for them.
/// has_hotkey array is also filled-up.
///
/// @param message Message which will be part of the confirm_msg
/// @param buttons String containing button names
/// @param[out] has_hotkey An element in this array is set to true if
///                        corresponding button has a hotkey
///
/// @return Pointer to memory allocated for storing hotkeys
static char *console_dialog_alloc(const char *message, const char *buttons, bool has_hotkey[])
{
  int lenhotkey = HOTK_LEN;  // count first button
  has_hotkey[0] = false;

  // Compute the size of memory to allocate.
  int len = 0;
  int idx = 0;
  const char *r = buttons;
  while (*r) {
    if (*r == DLG_BUTTON_SEP) {
      len += 3;                         // '\n' -> ', '; 'x' -> '(x)'
      lenhotkey += HOTK_LEN;            // each button needs a hotkey
      if (idx < HAS_HOTKEY_LEN - 1) {
        has_hotkey[++idx] = false;
      }
    } else if (*r == DLG_HOTKEY_CHAR) {
      r++;
      len++;                    // '&a' -> '[a]'
      if (idx < HAS_HOTKEY_LEN - 1) {
        has_hotkey[idx] = true;
      }
    }

    // Advance to the next character
    MB_PTR_ADV(r);
  }

  len += (int)(strlen(message)
               + 2                          // for the NL's
               + strlen(buttons)
               + 3);                        // for the ": " and NUL
  lenhotkey++;                               // for the NUL

  // If no hotkey is specified, first char is used.
  if (!has_hotkey[0]) {
    len += 2;                                // "x" -> "[x]"
  }

  // Now allocate space for the strings
  xfree(confirm_msg);
  confirm_msg = xmalloc((size_t)len);
  *confirm_msg = NUL;

  return xmalloc((size_t)lenhotkey);
}

/// Format the dialog string, and display it at the bottom of
/// the screen. Return a string of hotkey chars (if defined) for
/// each 'button'. If a button has no hotkey defined, the first character of
/// the button is used.
/// The hotkeys can be multi-byte characters, but without combining chars.
///
/// @return  an allocated string with hotkeys.
static char *msg_show_console_dialog(const char *message, const char *buttons, int dfltbutton)
  FUNC_ATTR_NONNULL_RET
{
  bool has_hotkey[HAS_HOTKEY_LEN] = { false };
  char *hotk = console_dialog_alloc(message, buttons, has_hotkey);

  copy_hotkeys_and_msg(message, buttons, dfltbutton, has_hotkey, hotk);

  display_confirm_msg();
  return hotk;
}

/// Copies hotkeys & dialog message into the memory allocated for it
///
/// @param message Message which will be part of the confirm_msg
/// @param buttons String containing button names
/// @param default_button_idx Number of default button
/// @param has_hotkey An element in this array is true if corresponding button
///                   has a hotkey
/// @param[out] hotkeys_ptr Pointer to the memory location where hotkeys will be copied
static void copy_hotkeys_and_msg(const char *message, const char *buttons, int default_button_idx,
                                 const bool has_hotkey[], char *hotkeys_ptr)
{
  *confirm_msg = '\n';
  STRCPY(confirm_msg + 1, message);

  char *msgp = confirm_msg + 1 + strlen(message);

  // Define first default hotkey. Keep the hotkey string NUL
  // terminated to avoid reading past the end.
  hotkeys_ptr[copy_char(buttons, hotkeys_ptr, true)] = NUL;

  // Remember where the choices start, displaying starts here when
  // "hotkeys_ptr" typed at the more prompt.
  confirm_msg_tail = msgp;
  *msgp++ = '\n';

  bool first_hotkey = false;  // Is the first char of button a hotkey
  if (!has_hotkey[0]) {
    first_hotkey = true;     // If no hotkey is specified, first char is used
  }

  int idx = 0;
  const char *r = buttons;
  while (*r) {
    if (*r == DLG_BUTTON_SEP) {
      *msgp++ = ',';
      *msgp++ = ' ';                    // '\n' -> ', '

      // Advance to next hotkey and set default hotkey
      hotkeys_ptr += strlen(hotkeys_ptr);
      hotkeys_ptr[copy_char(r + 1, hotkeys_ptr, true)] = NUL;

      if (default_button_idx) {
        default_button_idx--;
      }

      // If no hotkey is specified, first char is used.
      if (idx < HAS_HOTKEY_LEN - 1 && !has_hotkey[++idx]) {
        first_hotkey = true;
      }
    } else if (*r == DLG_HOTKEY_CHAR || first_hotkey) {
      if (*r == DLG_HOTKEY_CHAR) {
        r++;
      }

      first_hotkey = false;
      if (*r == DLG_HOTKEY_CHAR) {                 // '&&a' -> '&a'
        *msgp++ = *r;
      } else {
        // '&a' -> '[a]'
        *msgp++ = (default_button_idx == 1) ? '[' : '(';
        msgp += copy_char(r, msgp, false);
        *msgp++ = (default_button_idx == 1) ? ']' : ')';

        // redefine hotkey
        hotkeys_ptr[copy_char(r, hotkeys_ptr, true)] = NUL;
      }
    } else {
      // everything else copy literally
      msgp += copy_char(r, msgp, false);
    }

    // advance to the next character
    MB_PTR_ADV(r);
  }

  *msgp++ = ':';
  *msgp++ = ' ';
  *msgp = NUL;
}

/// Display the ":confirm" message.  Also called when screen resized.
void display_confirm_msg(void)
{
  // Avoid that 'q' at the more prompt truncates the message here.
  confirm_msg_used++;
  if (confirm_msg != NULL) {
    msg_ext_set_kind("confirm");
    msg_puts_attr(confirm_msg, HL_ATTR(HLF_M));
  }
  confirm_msg_used--;
}

int vim_dialog_yesno(int type, char *title, char *message, int dflt)
{
  if (do_dialog(type,
                title == NULL ? _("Question") : title,
                message,
                _("&Yes\n&No"), dflt, NULL, false) == 1) {
    return VIM_YES;
  }
  return VIM_NO;
}

int vim_dialog_yesnocancel(int type, char *title, char *message, int dflt)
{
  switch (do_dialog(type,
                    title == NULL ? _("Question") : title,
                    message,
                    _("&Yes\n&No\n&Cancel"), dflt, NULL, false)) {
  case 1:
    return VIM_YES;
  case 2:
    return VIM_NO;
  }
  return VIM_CANCEL;
}

int vim_dialog_yesnoallcancel(int type, char *title, char *message, int dflt)
{
  switch (do_dialog(type,
                    title == NULL ? "Question" : title,
                    message,
                    _("&Yes\n&No\nSave &All\n&Discard All\n&Cancel"),
                    dflt, NULL, false)) {
  case 1:
    return VIM_YES;
  case 2:
    return VIM_NO;
  case 3:
    return VIM_ALL;
  case 4:
    return VIM_DISCARDALL;
  }
  return VIM_CANCEL;
}
