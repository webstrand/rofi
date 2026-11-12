/*
 * rofi
 *
 * MIT/X11 License
 * Copyright © 2013-2023 Qball Cow <qball@gmpclient.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#define G_LOG_DOMAIN "Modes.Script"

/** The log domain of this dialog. */
#include "glib.h"

#include "display.h"
#include "helper.h"
#include "modes/script.h"
#include "rofi.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "widgets/textbox.h"

#include "mode-private.h"

#include "rofi-icon-fetcher.h"

#include "modes/dmenuscriptshared.h"

typedef struct {
  /** ID of the current script. */
  unsigned int id;
  /** List of visible items. */
  DmenuScriptEntry *cmd_list;
  /** length list of visible items. */
  unsigned int cmd_list_length;

  /** Urgent list */
  struct rofi_range_pair *urgent_list;
  unsigned int num_urgent_list;
  /** Active list */
  struct rofi_range_pair *active_list;
  unsigned int num_active_list;
  /** Configuration settings. */
  char *message;
  char *prompt;
  char *data;
  gboolean do_markup;
  gboolean keep_selection;
  int64_t new_selection;
  char delim;
  /** no custom */
  gboolean no_custom;
  /** keep filter */
  gboolean keep_filter;

  gboolean use_hot_keys;

  int switch_mode;

  /** fd source: allocated size of cmd_list. */
  size_t cmd_list_capacity;
  /** fd source: main loop watch on the input fd, 0 when done. */
  guint in_source;
  /** fd source: input received but not yet terminated by delim. */
  GString *pending;
  /** fd source: file status flags of the input fd before we changed them. */
  int in_flags;
} ScriptModePrivateData;

/**
 * Configuration of a `name:<N[>M]` mode, stored in Mode::ed.
 * Rows are read from fd N as they arrive; the pick is printed to fd M
 * (stdout if absent) and rofi exits.
 */
typedef struct {
  int in_fd;
  int out_fd;
} ScriptFdSource;

/**
 * Shared function between DMENU and Script mode.
 */
void dmenuscript_parse_entry_extras(G_GNUC_UNUSED Mode *sw,
                                    DmenuScriptEntry *entry, char *buffer,
                                    G_GNUC_UNUSED size_t length) {
  gchar **extras = g_strsplit(buffer, "\x1f", -1);
  gchar **extra = extras;
  for (; *extra != NULL && *(extra + 1) != NULL; extra += 2) {
    gchar *key = *extra;
    gchar *value = *(extra + 1);
    // Mark NULL
    *(extra) = NULL;
    *(extra + 1) = NULL;
    if (strcasecmp(key, "icon") == 0) {
      entry->icon_name = g_strsplit(value, ",", -1);
      g_free(value);
    } else if (strcasecmp(key, "display") == 0) {
      entry->display = value;
    } else if (strcasecmp(key, "meta") == 0) {
      entry->meta = value;
    } else if (strcasecmp(key, "info") == 0) {
      entry->info = value;
    } else if (strcasecmp(key, "nonselectable") == 0) {
      entry->nonselectable = g_ascii_strcasecmp(value, "true") == 0;
      g_free(value);
    } else if (strcasecmp(key, "permanent") == 0) {
      entry->permanent = g_ascii_strcasecmp(value, "true") == 0;
      g_free(value);
    } else if (strcasecmp(key, "urgent") == 0) {
      entry->urgent = g_ascii_strcasecmp(value, "true") == 0;
      g_free(value);
    } else if (strcasecmp(key, "active") == 0) {
      entry->active = g_ascii_strcasecmp(value, "true") == 0;
      g_free(value);
    } else {
      g_free(value);
    }
    g_free(key);
  }
  // Got an extra entry.. lets free it.
  if (*extras != NULL) {
    g_free(*extras);
  }
  g_free(extras);
}

/**
 * End of shared functions.
 */

static void script_switch_mode(Mode *sw, char *value) {
  int index = mode_lookup(value);
  ScriptModePrivateData *pd = (ScriptModePrivateData *)sw->private_data;

  if (index >= 0) {
    pd->switch_mode = index;
  } else {
    g_warning("Mode \"%s\" not found, please make sure it's enabled.\n", value);
  }
}

static void parse_header_entry(Mode *sw, char *line, ssize_t length) {
  ScriptModePrivateData *pd = (ScriptModePrivateData *)sw->private_data;
  ssize_t length_key = 0; // strlen ( line );
  while (length_key < length && line[length_key] != '\x1f') {
    length_key++;
  }

  if ((length_key + 1) < length) {
    line[length_key] = '\0';
    char *value = line + length_key + 1;
    if (strcasecmp(line, "message") == 0) {
      g_free(pd->message);
      pd->message = strlen(value) ? g_strdup(value) : NULL;
    } else if (strcasecmp(line, "prompt") == 0) {
      g_free(pd->prompt);
      pd->prompt = g_strdup(value);
      sw->display_name = pd->prompt;
    } else if (strcasecmp(line, "markup-rows") == 0) {
      pd->do_markup = (strcasecmp(value, "true") == 0);
    } else if (strcasecmp(line, "urgent") == 0) {
      parse_ranges(value, &(pd->urgent_list), &(pd->num_urgent_list));
    } else if (strcasecmp(line, "active") == 0) {
      parse_ranges(value, &(pd->active_list), &(pd->num_active_list));
    } else if (strcasecmp(line, "delim") == 0) {
      pd->delim = helper_parse_char(value);
    } else if (strcasecmp(line, "no-custom") == 0) {
      pd->no_custom = (strcasecmp(value, "true") == 0);
    } else if (strcasecmp(line, "use-hot-keys") == 0) {
      pd->use_hot_keys = (strcasecmp(value, "true") == 0);
    } else if (strcasecmp(line, "keep-selection") == 0) {
      pd->keep_selection = (strcasecmp(value, "true") == 0);
    } else if (strcasecmp(line, "keep-filter") == 0) {
      pd->keep_filter = (strcasecmp(value, "true") == 0);
    } else if (strcasecmp(line, "new-selection") == 0) {
      pd->new_selection = (int64_t)g_ascii_strtoll(value, NULL, 0);
    } else if (strcasecmp(line, "data") == 0) {
      g_free(pd->data);
      pd->data = g_strdup(value);
    } else if (strcasecmp(line, "theme") == 0) {
      if (rofi_theme_parse_string((const char *)value)) {
        g_warning("Failed to parse: '%s'", value);
        rofi_clear_error_messages();
      }
    } else if (strcasecmp(line, "switch-mode") == 0) {
      script_switch_mode(sw, value);
    }
  }
}

/**
 * Parse one delimited line of script output and append it to the list.
 *
 * @param buffer the line; must have a '\0' at buffer[read_length - 1] or be
 *               '\0' terminated at buffer[read_length] (the delimiter, if
 *               present, is replaced by '\0').
 * @param read_length length of the line including its delimiter.
 *
 * The list is kept with a zeroed slot past the last entry.
 *
 * @returns FALSE if the list could not be grown.
 */
static gboolean script_mode_add_line(Mode *sw, DmenuScriptEntry **list,
                                     unsigned int *length, size_t *capacity,
                                     char *buffer, ssize_t read_length) {
  ScriptModePrivateData *pd = (ScriptModePrivateData *)sw->private_data;
  // Filter out line-end.
  if (buffer[read_length - 1] == pd->delim) {
    buffer[read_length - 1] = '\0';
  }
  if (buffer[0] == '\0') {
    parse_header_entry(sw, &buffer[1], read_length - 1);
    return TRUE;
  }
  if (*capacity < ((*length) + 2)) {
    *capacity += 256;
    // Check if it returns NULL, if it does, break out and show what we
    // have so far.
    DmenuScriptEntry *retvn =
        g_realloc(*list, (*capacity) * sizeof(DmenuScriptEntry));
    if (retvn == NULL) {
      return FALSE;
    }
    *list = retvn;
  }
  DmenuScriptEntry *retv = *list;
  size_t buf_length = strlen(buffer);
  retv[(*length)].entry = rofi_force_utf8(buffer, buf_length);
  retv[(*length)].icon_name = NULL;
  retv[(*length)].display = NULL;
  retv[(*length)].meta = NULL;
  retv[(*length)].info = NULL;
  retv[(*length)].active = FALSE;
  retv[(*length)].urgent = FALSE;
  retv[(*length)].icon_fetch_uid = 0;
  retv[(*length)].icon_fetch_size = 0;
  retv[(*length)].icon_fetch_scale = 0;
  /* The memset below only clears the next slot, leaving the
   * first entry uninitialized. */
  retv[(*length)].icon_fallback_index = 0;
  retv[(*length)].nonselectable = FALSE;
  retv[(*length)].permanent = FALSE;

  // Skip the '\0'
  buf_length++;
  if (buf_length > 0 && (read_length > (ssize_t)buf_length)) {
    dmenuscript_parse_entry_extras(sw, &(retv[(*length)]), buffer + buf_length,
                                   read_length - buf_length);
  }
  memset(&(retv[(*length) + 1]), 0, sizeof(DmenuScriptEntry));
  (*length)++;
  return TRUE;
}

static DmenuScriptEntry *execute_executor(Mode *sw, char *arg,
                                          unsigned int *length, int value,
                                          DmenuScriptEntry *entry,
                                          char *input) {
  ScriptModePrivateData *pd = (ScriptModePrivateData *)sw->private_data;
  int fd = -1;
  GError *error = NULL;
  DmenuScriptEntry *retv = NULL;
  char **argv = NULL;
  int argc = 0;
  *length = 0;
  // Reset these between runs.
  pd->new_selection = -1;
  pd->keep_selection = 0;
  pd->keep_filter = 0;
  // Environment
  char **env = g_get_environ();

  char *str_value = g_strdup_printf("%d", value);
  env = g_environ_setenv(env, "ROFI_RETV", str_value, TRUE);
  g_free(str_value);

  str_value = g_strdup_printf("%d", (int)getpid());
  env = g_environ_setenv(env, "ROFI_OUTSIDE", str_value, TRUE);
  g_free(str_value);

  if (entry && entry->info) {
    env = g_environ_setenv(env, "ROFI_INFO", entry->info, TRUE);
  }
  if (pd->data) {
    env = g_environ_setenv(env, "ROFI_DATA", pd->data, TRUE);
  }

  if (input != NULL) {
    env = g_environ_setenv(env, "ROFI_INPUT", input, TRUE);
  }

  if (g_shell_parse_argv(sw->ed, &argc, &argv, &error)) {
    argv = g_realloc(argv, (argc + 2) * sizeof(char *));
    argv[argc] = g_strdup(arg);
    argv[argc + 1] = NULL;
    g_spawn_async_with_pipes(NULL, argv, env, G_SPAWN_SEARCH_PATH, NULL, NULL,
                             NULL, NULL, &fd, NULL, &error);
  }
  g_strfreev(env);
  if (error != NULL) {
    char *msg = g_strdup_printf("Failed to execute: '%s'\nError: '%s'",
                                (char *)sw->ed, error->message);
    rofi_view_error_dialog(msg, FALSE);
    g_free(msg);
    // print error.
    g_error_free(error);
    fd = -1;
  }
  if (fd >= 0) {
    FILE *inp = fdopen(fd, "r");
    if (inp) {
      char *buffer = NULL;
      size_t buffer_length = 0;
      ssize_t read_length = 0;
      size_t actual_size = 0;
      while ((read_length = getdelim(&buffer, &buffer_length, pd->delim, inp)) >
             0) {
        if (!script_mode_add_line(sw, &retv, length, &actual_size, buffer,
                                  read_length)) {
          break;
        }
      }
      if (buffer) {
        free(buffer);
      }
      if (fclose(inp) != 0) {
        g_warning("Failed to close stdout off executor script: '%s'",
                  g_strerror(errno));
      }
    }
  }
  g_strfreev(argv);
  return retv;
}

static void script_switcher_free(Mode *sw) {
  if (sw == NULL) {
    return;
  }
  g_free(sw->name);
  g_free(sw->ed);
  g_free(sw);
}

static int script_mode_init(Mode *sw) {
  if (sw->private_data == NULL) {
    ScriptModePrivateData *pd = g_malloc0(sizeof(*pd));
    pd->delim = '\n';
    sw->private_data = (void *)pd;
    pd->cmd_list =
        execute_executor(sw, NULL, &(pd->cmd_list_length), 0, NULL, NULL);
    pd->switch_mode = -1;
  }
  return TRUE;
}
static unsigned int script_mode_get_num_entries(const Mode *sw) {
  const ScriptModePrivateData *rmpd =
      (const ScriptModePrivateData *)sw->private_data;
  return rmpd->cmd_list_length;
}

static void script_mode_reset_highlight(Mode *sw) {
  ScriptModePrivateData *rmpd = (ScriptModePrivateData *)sw->private_data;

  rmpd->num_urgent_list = 0;
  g_free(rmpd->urgent_list);
  rmpd->urgent_list = NULL;
  rmpd->num_active_list = 0;
  g_free(rmpd->active_list);
  rmpd->active_list = NULL;
}

static void script_mode_free_entry_list(DmenuScriptEntry *list,
                                        unsigned int length) {
  for (unsigned int i = 0; i < length; i++) {
    g_free(list[i].entry);
    g_strfreev(list[i].icon_name);
    g_free(list[i].display);
    g_free(list[i].meta);
    g_free(list[i].info);
  }
  g_free(list);
}
static ModeMode script_mode_result(Mode *sw, int mretv, char **input,
                                   unsigned int selected_line) {
  ScriptModePrivateData *rmpd = (ScriptModePrivateData *)sw->private_data;
  ModeMode retv = MODE_EXIT;
  DmenuScriptEntry *new_list = NULL;
  unsigned int new_length = 0;
  // store them as they might be different on next executor and reset.
  gboolean keep_filter = rmpd->keep_filter;
  gboolean keep_selection = rmpd->keep_selection;

  if ((mretv & MENU_CUSTOM_COMMAND)) {
    if (rmpd->use_hot_keys) {
      script_mode_reset_highlight(sw);
      if (selected_line != UINT32_MAX) {
        new_list = execute_executor(sw, rmpd->cmd_list[selected_line].entry,
                                    &new_length, 10 + (mretv & MENU_LOWER_MASK),
                                    &(rmpd->cmd_list[selected_line]), *input);
      } else {
        if (rmpd->no_custom == FALSE) {
          new_list =
              execute_executor(sw, *input, &new_length,
                               10 + (mretv & MENU_LOWER_MASK), NULL, *input);
        } else {
          return RELOAD_DIALOG;
        }
      }
    } else {
      retv = (mretv & MENU_LOWER_MASK);
      return retv;
    }
  } else if ((mretv & MENU_ENTRY_DELETE) && selected_line != UINT32_MAX) {
    script_mode_reset_highlight(sw);
    new_list =
        execute_executor(sw, rmpd->cmd_list[selected_line].entry, &new_length,
                         3, &(rmpd->cmd_list[selected_line]), *input);
  } else if ((mretv & MENU_OK) && rmpd->cmd_list[selected_line].entry != NULL) {
    if (rmpd->cmd_list[selected_line].nonselectable) {
      return RELOAD_DIALOG;
    }
    script_mode_reset_highlight(sw);
    new_list =
        execute_executor(sw, rmpd->cmd_list[selected_line].entry, &new_length,
                         1, &(rmpd->cmd_list[selected_line]), *input);
  } else if ((mretv & MENU_CUSTOM_INPUT) && *input != NULL) {
    if (rmpd->no_custom == FALSE) {
      script_mode_reset_highlight(sw);
      new_list = execute_executor(sw, *input, &new_length, 2, NULL, *input);
    } else {
      return RELOAD_DIALOG;
    }
  }

  if (rmpd->switch_mode >= 0) {
    script_mode_free_entry_list(new_list, new_length);
    retv = rmpd->switch_mode;
    free(*input);
    *input = NULL;
    rmpd->switch_mode = -1;
    return retv;
  }

  // If a new list was generated, use that an loop around.
  if (new_list != NULL) {
    script_mode_free_entry_list(rmpd->cmd_list, rmpd->cmd_list_length);
    rmpd->cmd_list = new_list;
    rmpd->cmd_list_length = new_length;
    if (keep_selection) {
      if (rmpd->new_selection >= 0 &&
          rmpd->new_selection < rmpd->cmd_list_length) {
        rofi_view_set_selected_line(rofi_view_get_active(),
                                    rmpd->new_selection);
      } else {
        rofi_view_set_selected_line(rofi_view_get_active(), selected_line);
      }
    } else {
      rofi_view_set_selected_line(rofi_view_get_active(), 0);
    }
    if (keep_filter == FALSE) {
      g_free(*input);
      *input = NULL;
    }
    retv = RELOAD_DIALOG;
  }
  return retv;
}

static void script_mode_destroy(Mode *sw) {
  ScriptModePrivateData *rmpd = (ScriptModePrivateData *)sw->private_data;
  if (rmpd != NULL) {
    script_mode_free_entry_list(rmpd->cmd_list, rmpd->cmd_list_length);
    g_free(rmpd->message);
    g_free(rmpd->prompt);
    g_free(rmpd->data);
    g_free(rmpd->urgent_list);
    g_free(rmpd->active_list);
    g_free(rmpd);
    sw->private_data = NULL;
  }
}

/**
 * fd source modes (`name:<N[>M]`).
 *
 * Unlike a script, the input is never waited on: rows are appended as they
 * arrive on fd N, so the window shows immediately even when the producer is
 * slow. Picking a row prints it (see -format, which also accepts `t` for the
 * mode name and `I` for the row's info) to fd M or stdout and exits rofi.
 */

/** Stop reading: drop the watch, restore the fd's flags and close it. */
static void script_fd_close_input(Mode *sw) {
  ScriptModePrivateData *pd = (ScriptModePrivateData *)sw->private_data;
  ScriptFdSource *src = (ScriptFdSource *)sw->ed;
  if (pd->in_source > 0) {
    g_source_remove(pd->in_source);
    pd->in_source = 0;
  }
  if (src->in_fd >= 0) {
    // O_NONBLOCK is shared with whoever else holds this file description
    // (e.g. the invoking shell's terminal), so put it back.
    fcntl(src->in_fd, F_SETFL, pd->in_flags);
    close(src->in_fd);
    src->in_fd = -1;
  }
}

static gboolean script_fd_read_cb(gint fd, G_GNUC_UNUSED GIOCondition cond,
                                  gpointer user_data) {
  Mode *sw = (Mode *)user_data;
  ScriptModePrivateData *pd = (ScriptModePrivateData *)sw->private_data;
  gboolean eof = FALSE;
  char buf[65536];

  // Bounded so a fast producer can't starve the UI.
  for (int i = 0; i < 16; i++) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      g_string_append_len(pd->pending, buf, n);
    } else if (n == 0) {
      eof = TRUE;
      break;
    } else if (errno == EINTR) {
      continue;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    } else {
      g_warning("Mode '%s': failed to read input: %s", sw->name,
                g_strerror(errno));
      eof = TRUE;
      break;
    }
  }

  unsigned int old_length = pd->cmd_list_length;
  gsize start = 0;
  while (start < pd->pending->len) {
    // Re-read the delimiter each line: a header may change it.
    char *line = pd->pending->str + start;
    char *end = memchr(line, pd->delim, pd->pending->len - start);
    if (end == NULL) {
      break;
    }
    gsize len = end - line;
    script_mode_add_line(sw, &(pd->cmd_list), &(pd->cmd_list_length),
                         &(pd->cmd_list_capacity), line, len + 1);
    start += len + 1;
  }
  g_string_erase(pd->pending, 0, start);
  if (eof && pd->pending->len > 0) {
    // Unterminated last line; GString keeps a '\0' at str[len].
    script_mode_add_line(sw, &(pd->cmd_list), &(pd->cmd_list_length),
                         &(pd->cmd_list_capacity), pd->pending->str,
                         pd->pending->len + 1);
    g_string_truncate(pd->pending, 0);
  }

  // Headers (message, prompt, ...) can change the view too, so reload on
  // any consumed input, not just new rows.
  if (start > 0 || pd->cmd_list_length != old_length || eof) {
    rofi_view_reload();
  }
  if (eof) {
    // The watch is removed by returning G_SOURCE_REMOVE.
    pd->in_source = 0;
    script_fd_close_input(sw);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

/**
 * @returns NULL if fd is open (and marks it close-on-exec), otherwise an
 * error message for the mode's message bar.
 */
static char *script_fd_check(const Mode *sw, int fd, const char *what) {
  int flags = fcntl(fd, F_GETFD);
  if (flags == -1) {
    g_warning("Mode '%s': %s fd %d is not open.", sw->name, what, fd);
    return g_markup_printf_escaped(
        "<b>%s</b>: %s fd %d is not open. Pass it from the shell, "
        "e.g. <i>%d%s(command)</i>",
        sw->name, what, fd, fd,
        g_strcmp0(what, "input") == 0 ? "< <" : "> >");
  }
  // Don't leak our ends into programs other modes launch.
  fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  return NULL;
}

static int script_fd_mode_init(Mode *sw) {
  if (sw->private_data != NULL) {
    return TRUE;
  }
  ScriptFdSource *src = (ScriptFdSource *)sw->ed;
  ScriptModePrivateData *pd = g_malloc0(sizeof(*pd));
  pd->delim = '\n';
  pd->switch_mode = -1;
  pd->new_selection = -1;
  pd->pending = g_string_new(NULL);
  sw->private_data = (void *)pd;

  // A bad fd only breaks this tab: show why there instead of refusing to
  // start, so the other modes stay usable.
  char *error = script_fd_check(sw, src->in_fd, "input");
  if (error == NULL && src->out_fd >= 0) {
    error = script_fd_check(sw, src->out_fd, "output");
  }
  if (error != NULL) {
    pd->message = error;
    // Nothing to pick, and custom input must not leak to stdout instead.
    pd->no_custom = TRUE;
    src->in_fd = -1;
    src->out_fd = -1;
    return TRUE;
  }

  pd->in_flags = fcntl(src->in_fd, F_GETFL);
  fcntl(src->in_fd, F_SETFL, pd->in_flags | O_NONBLOCK);
  pd->in_source = g_unix_fd_add(src->in_fd, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                script_fd_read_cb, sw);
  return TRUE;
}

static void script_fd_output(Mode *sw, const char *string, int index,
                             const char *filter, const char *info) {
  ScriptFdSource *src = (ScriptFdSource *)sw->ed;
  const char *format = "s";
  find_arg_str("-format", (char **)&format);

  FILE *out = stdout;
  if (src->out_fd >= 0) {
    out = fdopen(src->out_fd, "w");
    if (out == NULL) {
      g_warning("Mode '%s': cannot open output fd %d: %s", sw->name,
                src->out_fd, g_strerror(errno));
      return;
    }
  }
  // The reader may already be gone; report that as a write error, not a
  // crash. rofi exits right after this, so nothing inherits the setting.
  signal(SIGPIPE, SIG_IGN);
  rofi_output_formatted_line_to(out, format, string, index, filter, sw->name,
                                info);
  if (src->out_fd >= 0) {
    // Close so a reader waiting for EOF finishes.
    if (fclose(out) != 0) {
      g_warning("Mode '%s': failed to write output: %s", sw->name,
                g_strerror(errno));
    }
    src->out_fd = -1;
  }
}

static ModeMode script_fd_mode_result(Mode *sw, int mretv, char **input,
                                      unsigned int selected_line) {
  ScriptModePrivateData *pd = (ScriptModePrivateData *)sw->private_data;
  gboolean have_row = selected_line < pd->cmd_list_length;
  const char *filter = *input;

  if (mretv & MENU_CUSTOM_COMMAND) {
    if (!pd->use_hot_keys) {
      // Same as script mode: kb-custom-N switches to mode N.
      return (mretv & MENU_LOWER_MASK);
    }
    if (have_row) {
      script_fd_output(sw, pd->cmd_list[selected_line].entry, selected_line,
                       filter, pd->cmd_list[selected_line].info);
    } else if (!pd->no_custom && *input != NULL) {
      script_fd_output(sw, *input, -1, filter, NULL);
    } else {
      return RELOAD_DIALOG;
    }
    rofi_set_return_code(10 + (mretv & MENU_LOWER_MASK));
    return MODE_EXIT;
  }
  if ((mretv & MENU_OK) && have_row) {
    if (pd->cmd_list[selected_line].nonselectable) {
      return RELOAD_DIALOG;
    }
    script_fd_output(sw, pd->cmd_list[selected_line].entry, selected_line,
                     filter, pd->cmd_list[selected_line].info);
    rofi_set_return_code(EXIT_SUCCESS);
    return MODE_EXIT;
  }
  if ((mretv & MENU_CUSTOM_INPUT) && *input != NULL) {
    if (pd->no_custom) {
      return RELOAD_DIALOG;
    }
    script_fd_output(sw, *input, -1, filter, NULL);
    rofi_set_return_code(EXIT_SUCCESS);
    return MODE_EXIT;
  }
  if (mretv & MENU_CANCEL) {
    // Like dmenu: nothing picked.
    rofi_set_return_code(1);
    return MODE_EXIT;
  }
  // Delete etc. have no meaning for a static list.
  return RELOAD_DIALOG;
}

static void script_fd_mode_destroy(Mode *sw) {
  ScriptModePrivateData *pd = (ScriptModePrivateData *)sw->private_data;
  if (pd == NULL) {
    return;
  }
  script_fd_close_input(sw);
  g_string_free(pd->pending, TRUE);
  script_mode_destroy(sw);
}

static inline unsigned int get_index(unsigned int length, int index) {
  if (index >= 0) {
    return index;
  }
  if (((unsigned int)-index) <= length) {
    return length + index;
  }
  // Out of range.
  return UINT_MAX;
}
static char *_get_display_value(const Mode *sw, unsigned int selected_line,
                                G_GNUC_UNUSED int *state,
                                G_GNUC_UNUSED GList **list, int get_entry) {
  ScriptModePrivateData *pd = sw->private_data;
  for (unsigned int i = 0; i < pd->num_active_list; i++) {
    unsigned int start =
        get_index(pd->cmd_list_length, pd->active_list[i].start);
    unsigned int stop = get_index(pd->cmd_list_length, pd->active_list[i].stop);
    if (selected_line >= start && selected_line <= stop) {
      *state |= ACTIVE;
    }
  }
  for (unsigned int i = 0; i < pd->num_urgent_list; i++) {
    unsigned int start =
        get_index(pd->cmd_list_length, pd->urgent_list[i].start);
    unsigned int stop = get_index(pd->cmd_list_length, pd->urgent_list[i].stop);
    if (selected_line >= start && selected_line <= stop) {
      *state |= URGENT;
    }
  }
  if (pd->cmd_list[selected_line].urgent) {
    *state |= URGENT;
  }
  if (pd->cmd_list[selected_line].active) {
    *state |= ACTIVE;
  }
  if (pd->do_markup) {
    *state |= MARKUP;
  }
  if (pd->cmd_list[selected_line].display) {
    return get_entry ? g_strdup(pd->cmd_list[selected_line].display) : NULL;
  } else {
    return get_entry ? g_strdup(pd->cmd_list[selected_line].entry) : NULL;
  }
}

static int script_token_match(const Mode *sw, rofi_int_matcher **tokens,
                              unsigned int index) {
  ScriptModePrivateData *rmpd = sw->private_data;
  /** Strip out the markup when matching. */
  char *esc = NULL;

  if (rmpd->cmd_list[index].permanent == TRUE) {
    // Always match
    return 1;
  }

  if (rmpd->do_markup) {
    pango_parse_markup(rmpd->cmd_list[index].entry, -1, 0, NULL, &esc, NULL,
                       NULL);
  } else {
    esc = rmpd->cmd_list[index].entry;
  }
  if (esc) {
    int match = 1;
    if (tokens) {
      for (int j = 0; match && tokens[j] != NULL; j++) {
        rofi_int_matcher *ftokens[2] = {tokens[j], NULL};
        int test = 0;
        test = helper_token_match(ftokens, esc);
        if (test == tokens[j]->invert && rmpd->cmd_list[index].meta) {
          test = helper_token_match(ftokens, rmpd->cmd_list[index].meta);
        }

        if (test == 0) {
          match = 0;
        }
      }
    }
    if (rmpd->do_markup) {
      g_free(esc);
    }
    return match;
  }
  return FALSE;
}
static char *script_get_message(const Mode *sw) {
  ScriptModePrivateData *pd = sw->private_data;
  return g_strdup(pd->message);
}
static cairo_surface_t *script_get_icon(const Mode *sw,
                                        unsigned int selected_line,
                                        unsigned int height) {
  ScriptModePrivateData *pd =
      (ScriptModePrivateData *)mode_get_private_data(sw);

  const guint scale = display_scale();

  g_return_val_if_fail(pd->cmd_list != NULL, NULL);
  DmenuScriptEntry *dr = &(pd->cmd_list[selected_line]);
  if (dr->icon_name == NULL) {
    return NULL;
  }

  /* A row icon and icon-current-entry ask for the same entry at different
   * sizes; one cached uid cannot serve both. */
  cairo_surface_t *surface = NULL;
  if (dr->icon_fetch_uid > 0 && dr->icon_fetch_size == height &&
      dr->icon_fetch_scale == scale) {
    gboolean query_done =
        rofi_icon_fetcher_get_ex(dr->icon_fetch_uid, &surface);

    if (surface != NULL) {
      return surface;
    } else if (query_done) {
      dr->icon_fallback_index++;
      dr->icon_fetch_uid = 0;
    } else {
      return NULL;
    }
  }

  char *current_icon = NULL;
  if (dr->icon_name && dr->icon_fallback_index >= 0) {
    int icon_count = g_strv_length(dr->icon_name);
    if (dr->icon_fallback_index < icon_count) {
      current_icon = dr->icon_name[dr->icon_fallback_index];
    }
  }
  if (current_icon) {
    dr->icon_fetch_uid = rofi_icon_fetcher_query(current_icon, height);
    dr->icon_fetch_size = height;
    dr->icon_fetch_scale = scale;
    /* The sizes alternate, so returning NULL would blank the icon on every
     * frame, not just the first. */
    rofi_icon_fetcher_get_ex(dr->icon_fetch_uid, &surface);
    return surface;
  }

  dr->icon_fetch_uid = 0;
  return NULL;
}

#include "mode-private.h"

/** Structure that holds a user script
 * found in $config/rofi/scripts/
 */
typedef struct ScriptUser {
  /** name of the script */
  char *name;
  /** path to the script. */
  char *path;
} ScriptUser;

/** list of user_scripts. */
ScriptUser *user_scripts = NULL;
/** number of user scripts collected */
size_t num_scripts = 0;

void script_mode_cleanup(void) {
  for (size_t i = 0; i < num_scripts; i++) {
    g_free(user_scripts[i].name);
    g_free(user_scripts[i].path);
  }
  g_free(user_scripts);
}
void script_mode_gather_user_scripts(void) {
  const char *cpath = g_get_user_config_dir();
  char *script_dir = g_build_filename(cpath, "rofi", "scripts", NULL);
  if (g_file_test(script_dir, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR) ==
      FALSE) {
    g_free(script_dir);
    return;
  }
  GDir *sd = g_dir_open(script_dir, 0, NULL);
  if (sd) {
    const char *file = NULL;
    while ((file = g_dir_read_name(sd)) != NULL) {
      char *sp = g_build_filename(cpath, "rofi", "scripts", file, NULL);
      user_scripts =
          g_realloc(user_scripts, sizeof(ScriptUser) * (num_scripts + 1));
      user_scripts[num_scripts].path = sp;
      user_scripts[num_scripts].name = g_strdup(file);
      char *dot = strrchr(user_scripts[num_scripts].name, '.');
      if (dot) {
        *dot = '\0';
      }
      num_scripts++;
    }
    g_dir_close(sd);
  }

  g_free(script_dir);
}

static int script_mode_has_user_script(char const *const user) {

  for (size_t i = 0; i < num_scripts; i++) {
    if (g_strcmp0(user_scripts[i].name, user) == 0) {
      return i;
    }
  }
  return -1;
}

Mode *script_mode_parse_setup(const char *str) {
  int ui = 0;
  if ((ui = script_mode_has_user_script(str)) >= 0) {
    Mode *sw = g_malloc0(sizeof(*sw));
    sw->name = g_strdup(user_scripts[ui].name);
    sw->ed = g_strdup(user_scripts[ui].path);
    sw->free = script_switcher_free;
    sw->_init = script_mode_init;
    sw->_get_num_entries = script_mode_get_num_entries;
    sw->_result = script_mode_result;
    sw->_destroy = script_mode_destroy;
    sw->_token_match = script_token_match;
    sw->_get_message = script_get_message;
    sw->_get_icon = script_get_icon;
    sw->_get_completion = NULL, sw->_preprocess_input = NULL,
    sw->_get_display_value = _get_display_value;
    sw->type = MODE_TYPE_SWITCHER;
    return sw;
  }
  Mode *sw = g_malloc0(sizeof(*sw));
  unsigned int index = 0;
  const char *const sep = ":";
  char **tokens = g_strsplit(str, sep, 2);
  if (tokens && g_strv_length(tokens) == 2 && tokens[1][0] == '<') {
    // fd source: name:<N or name:<N>M
    ScriptFdSource src = {.in_fd = -1, .out_fd = -1};
    char *end = NULL;
    gint64 in = g_ascii_strtoll(tokens[1] + 1, &end, 10);
    gboolean ok = end != tokens[1] + 1 && in >= 0 && in <= INT_MAX;
    src.in_fd = (int)in;
    if (ok && *end == '>') {
      char *out_start = end + 1;
      gint64 out = g_ascii_strtoll(out_start, &end, 10);
      ok = end != out_start && out >= 0 && out <= INT_MAX;
      src.out_fd = (int)out;
    }
    if (!ok || *end != '\0') {
      fprintf(stderr,
              "The fd mode '%s' is invalid, expected <name>:<IN or "
              "<name>:<IN>OUT with IN and OUT file descriptor numbers.\n",
              str);
      g_strfreev(tokens);
      g_free(sw);
      return NULL;
    }
    sw->name = g_strdup(tokens[0]);
    sw->ed = g_memdup2(&src, sizeof(src));
    g_strfreev(tokens);
    sw->free = script_switcher_free;
    sw->_init = script_fd_mode_init;
    sw->_get_num_entries = script_mode_get_num_entries;
    sw->_result = script_fd_mode_result;
    sw->_destroy = script_fd_mode_destroy;
    sw->_token_match = script_token_match;
    sw->_get_message = script_get_message;
    sw->_get_icon = script_get_icon;
    sw->_get_completion = NULL;
    sw->_preprocess_input = NULL;
    sw->_get_display_value = _get_display_value;
    sw->type = MODE_TYPE_SWITCHER;
    return sw;
  }
  if (tokens) {
    index = g_strv_length(tokens);
    sw->name = g_strdup(tokens[0]);
    sw->ed = (void *)rofi_expand_path(tokens[1]);
    g_strfreev(tokens);
  }
  if (index == 2) {
    sw->free = script_switcher_free;
    sw->_init = script_mode_init;
    sw->_get_num_entries = script_mode_get_num_entries;
    sw->_result = script_mode_result;
    sw->_destroy = script_mode_destroy;
    sw->_token_match = script_token_match;
    sw->_get_message = script_get_message;
    sw->_get_icon = script_get_icon;
    sw->_get_completion = NULL, sw->_preprocess_input = NULL,
    sw->_get_display_value = _get_display_value;
    sw->type = MODE_TYPE_SWITCHER;

    return sw;
  }
  fprintf(
      stderr,
      "The script command '%s' has %u options, but needs 2: <name>:<script>.",
      str, index);
  script_switcher_free(sw);
  return NULL;
}

gboolean script_mode_is_valid(const char *token) {
  if (script_mode_has_user_script(token) >= 0) {
    return TRUE;
  }
  return strchr(token, ':') != NULL;
}

void script_user_list(gboolean is_term) {

  for (unsigned int i = 0; i < num_scripts; i++) {
    printf("        • %s%s%s (%s)\n", is_term ? (color_bold) : "",
           user_scripts[i].name, is_term ? color_reset : "",
           user_scripts[i].path);
  }
}
