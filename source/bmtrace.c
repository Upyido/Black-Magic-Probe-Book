/*
 * Trace viewer utility for visualizing output on the TRACESWO pin via the
 * Black Magic Probe. This utility is built with Nuklear for a cross-platform
 * GUI.
 *
 * Copyright 2019-2022 CompuPhase
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined WIN32 || defined _WIN32
  #define STRICT
  #define WIN32_LEAN_AND_MEAN
  #define _WIN32_WINNT   0x0500 /* for AttachConsole() */
  #include <windows.h>
  #include <direct.h>
  #include <io.h>
  #include <malloc.h>
  #if defined __MINGW32__ || defined __MINGW64__
    #include "strlcpy.h"
  #elif defined _MSC_VER
    #include "strlcpy.h"
    #define access(p,m)       _access((p),(m))
    #define mkdir(p)          _mkdir(p)
  #endif
#elif defined __linux__
  #include <alloca.h>
  #include <pthread.h>
  #include <unistd.h>
  #include <bsd/string.h>
  #include <sys/stat.h>
  #include <sys/time.h>
#endif
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guidriver.h"
#include "bmcommon.h"
#include "bmp-script.h"
#include "bmp-support.h"
#include "bmp-scan.h"
#include "demangle.h"
#include "dwarf.h"
#include "elf.h"
#include "gdb-rsp.h"
#include "minIni.h"
#include "noc_file_dialog.h"
#include "nuklear_mousepointer.h"
#include "nuklear_splitter.h"
#include "nuklear_style.h"
#include "nuklear_tooltip.h"
#include "rs232.h"
#include "specialfolder.h"
#include "tcpip.h"

#include "parsetsdl.h"
#include "decodectf.h"
#include "swotrace.h"

#if defined __linux__ || defined __unix__
  #include "res/icon_trace_64.h"
#endif

#if defined _MSC_VER
  #define stricmp(a,b)    _stricmp((a),(b))
  #define strdup(s)       _strdup(s)
#endif

#if !defined _MAX_PATH
  #define _MAX_PATH 260
#endif

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
  #define stricmp(s1,s2)    strcasecmp((s1),(s2))
#endif

#if !defined sizearray
  #define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

#if defined WIN32 || defined _WIN32
  #define IS_OPTION(s)  ((s)[0] == '-' || (s)[0] == '/')
#else
  #define IS_OPTION(s)  ((s)[0] == '-')
#endif

static DWARF_LINELOOKUP dwarf_linetable = { NULL };
static DWARF_SYMBOLLIST dwarf_symboltable = { NULL};
static DWARF_PATHLIST dwarf_filetable = { NULL};

int ctf_error_notify(int code, int linenr, const char *message)
{
  char msg[200];

  (void)code;
  assert(message != NULL);
  if (linenr > 0)
    sprintf(msg, "TSDL file error, line %d: ", linenr);
  else
    strcpy(msg, "TSDL file error: ");
  strlcat(msg, message, sizearray(msg));
  tracelog_statusmsg(TRACESTATMSG_CTF, msg, 0);
  return 0;
}

static int bmp_callback(int code, const char *message)
{
  tracelog_statusmsg(TRACESTATMSG_BMP, message, code);
  return code >= 0;
}


#define WINDOW_WIDTH    700     /* default window size (window is resizable) */
#define WINDOW_HEIGHT   400
#define FONT_HEIGHT     14      /* default font size */
#define ROW_HEIGHT      (1.6 * opt_fontsize)
#define COMBOROW_CY     (0.9 * opt_fontsize)
#define BROWSEBTN_WIDTH (1.5 * opt_fontsize)
static float opt_fontsize = FONT_HEIGHT;

#define FILTER_MAXSTRING  128


#define ERROR_NO_TSDL 0x0001
#define ERROR_NO_ELF  0x0002

static void usage(const char *invalid_option)
{
  #if defined _WIN32  /* fix console output on Windows */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
      freopen("CONOUT$", "wb", stdout);
      freopen("CONOUT$", "wb", stderr);
    }
    printf("\n");
  #endif

  if (invalid_option != NULL)
    fprintf(stderr, "Unknown option %s; use -h for help.\n\n", invalid_option);
  else
    printf("BMTrace - SWO Trace Viewer for the Black Magic Probe.\n\n");
  printf("Usage: bmtrace [options]\n\n"
         "Options:\n"
         "-f=value  Font size to use (value must be 8 or larger).\n"
         "-h        This help.\n"
         "-t=path   Path to the TSDL metadata file to use.\n");
}

typedef struct tagAPPSTATE {
  int probe;                    /**< selected debug probe (index) */
  int netprobe;                 /**< index for the IP address (pseudo-probe) */
  const char **probelist;       /**< list of detected probes */
  char mcu_driver[32];          /**< target driver (detected by BMP) */
  char mcu_architecture[32];    /**< target Cortex architexture */
  int reinitialize;             /**< whether to re-initialize the traceswo interface */
  int trace_status;             /**< status of traceswo */
  int trace_running;            /**< whether tracing is running or paused */
  int error_flags;              /**< errors in initialization or decoding */
  char IPaddr[64];              /**< IP address for network probe */
  unsigned char trace_endpoint; /**< standard USB endpoint for tracing */
  int probe_type;               /**< BMP or ctxLink (needed to select manchester/async mode) */
  int mode;                     /**< manchester or async */
  int init_target;              /**< whether to configure the target MCU for tracing */
  int init_bmp;                 /**< whether to configure the debug probe for tracing */
  int connect_srst;             /**< whether to force reset while attaching */
  char cpuclock_str[16];        /**< edit buffer for CPU clock frequency */
  unsigned long cpuclock;       /**< active CPU clock frequency */
  char bitrate_str[16];         /**< edit buffer for bitrate */
  unsigned long bitrate;        /**< active bitrate */
  int datasize;                 /**< packet size */
  int reload_format;            /**< whether to reload the TSDL file */
  char TSDLfile[_MAX_PATH];     /**< CTF decoding, message file */
  char ELFfile[_MAX_PATH];      /**< ELF file for symbol/address look-up */
  TRACEFILTER *filterlist;      /**< filter expressions */
  int filtercount;              /**< count of valid entries in filterlist */
  int filterlistsize;           /**< count of allocated entries in filterlist */
  char newfiltertext[FILTER_MAXSTRING]; /**< text field for filters */
  unsigned long channelmask;    /**< bit mask of enabled channels */
  int cur_chan_edit;            /**< channel info currently being edited (-1 if none) */
  char chan_str[64];            /**< edit string for channel currently being edited */
  int cur_match_line;           /**< current line matched in "find" function */
  int find_popup;               /**< whether "find" popup is active */
  char findtext[128];           /**< search text (keywords) */
} APPSTATE;

enum {
  TAB_CONFIGURATION,
  TAB_CHANNELS,
  TAB_FILTERS,
  /* --- */
  TAB_COUNT
};

enum {
  MODE_MANCHESTER = 1,
  MODE_ASYNC
};

static void find_popup(struct nk_context *ctx, APPSTATE *state, float canvas_width, float canvas_height)
{
  if (state->find_popup > 0) {
    struct nk_rect rc;
    rc.x = canvas_width - 18 * opt_fontsize;
    rc.y = canvas_height - 6.5 * ROW_HEIGHT;
    rc.w = 200;
    rc.h = 3.6 * ROW_HEIGHT;
    if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Search", NK_WINDOW_NO_SCROLLBAR, rc)) {
      nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 2, nk_ratio(2, 0.2, 0.8));
      nk_label(ctx, "Text", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_edit_focus(ctx, 0);
      nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD|NK_EDIT_CLIPBOARD,
                                     state->findtext, sizearray(state->findtext),
                                     nk_filter_ascii);
      nk_layout_row(ctx, NK_DYNAMIC, opt_fontsize, 2, nk_ratio(2, 0.2, 0.8));
      nk_spacing(ctx, 1);
      if (state->find_popup == 2)
        nk_label_colored(ctx, "Text not found", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, nk_rgb(255, 80, 100));
      nk_layout_row_dynamic(ctx, ROW_HEIGHT, 3);
      nk_spacing(ctx, 1);
      if (nk_button_label(ctx, "Find") || nk_input_is_key_pressed(&ctx->input, NK_KEY_ENTER)) {
        if (strlen(state->findtext) > 0) {
          int line = tracestring_find(state->findtext, state->cur_match_line);
          if (line != state->cur_match_line) {
            state->cur_match_line = line;
            state->find_popup = 0;
            state->trace_running = nk_false;
          } else {
            state->cur_match_line = -1;
            state->find_popup = 2; /* to mark "string not found" */
          }
          nk_popup_close(ctx);
        } /* if (len > 0) */
      }
      if (nk_button_label(ctx, "Cancel") || nk_input_is_key_pressed(&ctx->input, NK_KEY_ESCAPE)) {
        state->find_popup = 0;
        nk_popup_close(ctx);
      }
      nk_popup_end(ctx);
    } else {
      state->find_popup = 0;
    }
  }
}

static void panel_options(struct nk_context *ctx, APPSTATE *state,
                          enum nk_collapse_states tab_states[TAB_COUNT],
                          float panel_width)
{
  static const char *datasize_strings[] = { "auto", "8 bit", "16 bit", "32 bit" };
  static const char *mode_strings[] = { "Manchester", "NRZ/async." };

  #define LABEL_WIDTH (4.5 * opt_fontsize)
  #define VALUE_WIDTH (panel_width - LABEL_WIDTH - 26)

  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Configuration", &tab_states[TAB_CONFIGURATION])) {
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "Probe", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH);
    struct nk_rect bounds = nk_widget_bounds(ctx);
    state->probe = nk_combo(ctx, state->probelist, state->netprobe+1, state->probe,
                            (int)COMBOROW_CY, nk_vec2(bounds.w, 4.5*ROW_HEIGHT));
    if (state->probe == state->netprobe) {
      int reconnect = 0;
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
      nk_layout_row_push(ctx, LABEL_WIDTH);
      nk_label(ctx, "IP Addr", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_layout_row_push(ctx, VALUE_WIDTH - BROWSEBTN_WIDTH - 5);
      int result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                                    state->IPaddr, sizearray(state->IPaddr), nk_filter_ascii,
                                    "IP address of the ctxLink");
      if ((result & NK_EDIT_COMMITED) != 0 && bmp_is_ip_address(state->IPaddr))
        reconnect = 1;
      nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
      if (button_symbol_tooltip(ctx, NK_SYMBOL_TRIPLE_DOT, NK_KEY_NONE, "Scan network for ctxLink probes.")) {
        #if defined WIN32 || defined _WIN32
          HCURSOR hcur = SetCursor(LoadCursor(NULL, IDC_WAIT));
        #endif
        unsigned long addr;
        int count = scan_network(&addr, 1);
        #if defined WIN32 || defined _WIN32
          SetCursor(hcur);
        #endif
        if (count == 1) {
          sprintf(state->IPaddr, "%lu.%lu.%lu.%lu",
                 addr & 0xff, (addr >> 8) & 0xff, (addr >> 16) & 0xff, (addr >> 24) & 0xff);
          reconnect = 1;
        } else {
          strlcpy(state->IPaddr, "none found", sizearray(state->IPaddr));
        }
      }
      if (reconnect) {
        bmp_disconnect();
        state->reinitialize = nk_true;
      }
    }
    if (state->probe_type == PROBE_UNKNOWN) {
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
      nk_layout_row_push(ctx, LABEL_WIDTH);
      nk_label(ctx, "Mode", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_layout_row_push(ctx, VALUE_WIDTH);
      int result = state->mode - MODE_MANCHESTER;
      result = nk_combo(ctx, mode_strings, NK_LEN(mode_strings), result, opt_fontsize, nk_vec2(VALUE_WIDTH,4.5*opt_fontsize));
      if (state->mode != result + MODE_MANCHESTER) {
        /* mode is 1-based, the result of nk_combo() is 0-based, which is
           why MODE_MANCHESTER is added (MODE_MANCHESTER == 1) */
        state->mode = result + MODE_MANCHESTER;
        state->reinitialize = nk_true;
      }
      nk_layout_row_end(ctx);
    }
    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    if (checkbox_tooltip(ctx, "Configure Target", &state->init_target, NK_TEXT_LEFT, "Configure the target microcontroller for SWO"))
      state->reinitialize = nk_true;
    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    if (checkbox_tooltip(ctx, "Configure Debug Probe", &state->init_bmp, NK_TEXT_LEFT, "Activate SWO trace capture in the Black Magic Probe"))
      state->reinitialize = nk_true;
    if (state->init_target || state->init_bmp) {
      nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
      if (checkbox_tooltip(ctx, "Reset target during connect", &state->connect_srst, NK_TEXT_LEFT, "Keep the target in reset state while scanning and attaching"))
        state->reinitialize = nk_true;
    }
    if (state->init_target) {
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
      nk_layout_row_push(ctx, LABEL_WIDTH);
      nk_label(ctx, "CPU clock", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_layout_row_push(ctx, VALUE_WIDTH);
      int result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                                    state->cpuclock_str, sizearray(state->cpuclock_str), nk_filter_decimal,
                                    "CPU clock of the target microcontroller");
      if ((result & NK_EDIT_COMMITED) != 0 || ((result & NK_EDIT_DEACTIVATED) && strtoul(state->cpuclock_str, NULL, 10) != state->cpuclock))
        state->reinitialize = nk_true;
      nk_layout_row_end(ctx);
    }
    if (state->init_target || (state->init_bmp && state->mode == MODE_ASYNC)) {
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
      nk_layout_row_push(ctx, LABEL_WIDTH);
      nk_label(ctx, "Bit rate", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_layout_row_push(ctx, VALUE_WIDTH);
      int result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                                    state->bitrate_str, sizearray(state->bitrate_str), nk_filter_decimal,
                                    "SWO bit rate (data rate)");
      if ((result & NK_EDIT_COMMITED) != 0 || ((result & NK_EDIT_DEACTIVATED) && strtoul(state->bitrate_str, NULL, 10) != state->bitrate))
        state->reinitialize = nk_true;
      nk_layout_row_end(ctx);
    }
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "Data size", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH);
    bounds = nk_widget_bounds(ctx);
    int result = state->datasize;
    state->datasize = nk_combo(ctx, datasize_strings, NK_LEN(datasize_strings), state->datasize, opt_fontsize, nk_vec2(VALUE_WIDTH,5.5*opt_fontsize));
    if (state->datasize != result) {
      trace_setdatasize((state->datasize == 3) ? 4 : (short)state->datasize);
      tracestring_clear();
      if (state->trace_status == TRACESTAT_OK)
        tracelog_statusmsg(TRACESTATMSG_BMP, "Listening ...", BMPSTAT_SUCCESS);
    }
    tooltip(ctx, bounds, "Payload size of an SWO packet (in bits); auto for autodetect");
    nk_layout_row_end(ctx);
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "TSDL file", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH - BROWSEBTN_WIDTH - 5);
    if (state->error_flags & ERROR_NO_TSDL)
      nk_style_push_color(ctx,&ctx->style.edit.text_normal, nk_rgb(255, 80, 100));
    result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                              state->TSDLfile, sizearray(state->TSDLfile), nk_filter_ascii,
                              "Metadata file for Common Trace Format (CTF)");
    if (result & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED))
      state->reload_format = nk_true;
    if (state->error_flags & ERROR_NO_TSDL)
      nk_style_pop_color(ctx);
    nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
    if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT)) {
      const char *s = noc_file_dialog_open(NOC_FILE_DIALOG_OPEN,
                                           "TSDL files\0*.tsdl;*.ctf\0All files\0*.*\0",
                                           NULL, state->TSDLfile, "Select metadata file for CTF",
                                           guidriver_apphandle());
      if (s != NULL && strlen(s) < sizearray(state->TSDLfile)) {
        strcpy(state->TSDLfile, s);
        state->reload_format = nk_true;
        free((void*)s);
      }
    }
    nk_layout_row_end(ctx);
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "ELF file", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH - BROWSEBTN_WIDTH - 5);
    if (state->error_flags & ERROR_NO_ELF)
      nk_style_push_color(ctx,&ctx->style.edit.text_normal, nk_rgb(255, 80, 100));
    result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                              state->ELFfile, sizearray(state->ELFfile), nk_filter_ascii,
                              "ELF file for symbol lookup");
    if (result & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED))
      state->reload_format = nk_true;
    if (state->error_flags & ERROR_NO_ELF)
      nk_style_pop_color(ctx);
    nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
    if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT)) {
      const char *s = noc_file_dialog_open(NOC_FILE_DIALOG_OPEN,
                                           "ELF Executables\0*.elf;*.bin;*.\0All files\0*.*\0",
                                           NULL, state->ELFfile, "Select ELF Executable",
                                           guidriver_apphandle());
      if (s != NULL && strlen(s) < sizearray(state->ELFfile)) {
        strcpy(state->ELFfile, s);
        state->reload_format = nk_true;
        free((void*)s);
      }
    }
    nk_layout_row_end(ctx);
    nk_tree_state_pop(ctx);
  }
}

static void filter_options(struct nk_context *ctx, APPSTATE *state,
                          enum nk_collapse_states tab_states[TAB_COUNT])
{
  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Filters", &tab_states[TAB_FILTERS])) {
    char filter[FILTER_MAXSTRING];
    assert(state->filterlistsize == 0 || state->filterlist != NULL);
    assert(state->filterlistsize == 0 || state->filtercount < state->filterlistsize);
    assert(state->filterlistsize == 0 || (state->filterlist[state->filtercount].expr == NULL && !state->filterlist[state->filtercount].enabled));
    struct nk_rect bounds = nk_widget_bounds(ctx);
    int txtwidth = bounds.w - 2 * BROWSEBTN_WIDTH - (2 * 5);
    for (int idx = 0; idx < state->filtercount; idx++) {
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
      nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
      checkbox_tooltip(ctx, "", &state->filterlist[idx].enabled, NK_TEXT_LEFT, "Enable/disable this filter");
      nk_layout_row_push(ctx, txtwidth);
      assert(state->filterlist[idx].expr != NULL);
      strcpy(filter, state->filterlist[idx].expr);
      int result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                                    filter, sizearray(filter), nk_filter_ascii,
                                    "Text to filter on (case-sensitive)");
      if (strcmp(filter, state->filterlist[idx].expr) != 0) {
        strcpy(state->filterlist[idx].expr, filter);
        state->filterlist[idx].enabled = (strlen(state->filterlist[idx].expr) > 0);
      }
      nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
      if (button_symbol_tooltip(ctx, NK_SYMBOL_X, NK_KEY_NONE, "Remove this filter")
          || ((result & NK_EDIT_COMMITED) && strlen(filter) == 0))
      {
        /* remove row */
        assert(state->filterlist[idx].expr != NULL);
        free(state->filterlist[idx].expr);
        state->filtercount -= 1;
        if (idx < state->filtercount)
          memmove(&state->filterlist[idx], &state->filterlist[idx+1], (state->filtercount - idx) * sizeof(TRACEFILTER));
        state->filterlist[state->filtercount].expr = NULL;
        state->filterlist[state->filtercount].enabled = 0;
      }
    }
    txtwidth = bounds.w - 1 * BROWSEBTN_WIDTH - (1 * 5);
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, txtwidth);
    int result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                                  state->newfiltertext, sizearray(state->newfiltertext),
                                  nk_filter_ascii, "New filter (case-sensitive)");
    nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
    if ((button_symbol_tooltip(ctx, NK_SYMBOL_PLUS, NK_KEY_NONE, "Add filter")
         || (result & NK_EDIT_COMMITED))
        && strlen(state->newfiltertext) > 0)
    {
      /* add row */
      if (state->filterlistsize > 0) {
        /* make sure there is an extra entry at the top of the array, for
           a NULL terminator */
        assert(state->filtercount < state->filterlistsize);
        if (state->filtercount + 1 == state->filterlistsize) {
          int newsize = 2 * state->filterlistsize;
          TRACEFILTER *newlist = malloc(newsize * sizeof(TRACEFILTER));
          if (newlist != NULL) {
            assert(state->filterlist != NULL);
            memset(newlist, 0, newsize * sizeof(TRACEFILTER));  /* set all new entries to NULL */
            memcpy(newlist, state->filterlist, state->filterlistsize * sizeof(TRACEFILTER));
            free(state->filterlist);
            state->filterlist = newlist;
            state->filterlistsize = newsize;
          }
        }
      }
      if (state->filtercount + 1 < state->filterlistsize) {
        state->filterlist[state->filtercount].expr = malloc(sizearray(state->newfiltertext) * sizeof(char));
        if (state->filterlist[state->filtercount].expr != NULL) {
          strcpy(state->filterlist[state->filtercount].expr, state->newfiltertext);
          state->filterlist[state->filtercount].enabled = 1;
          state->filtercount += 1;
          state->newfiltertext[0] = '\0';
        }
      }
    }
    nk_tree_state_pop(ctx);
  }
}

static void channel_options(struct nk_context *ctx, APPSTATE *state,
                            enum nk_collapse_states tab_states[TAB_COUNT])
{
  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Channels", &tab_states[TAB_CHANNELS])) {
    float labelwidth = tracelog_labelwidth(opt_fontsize) + 10;
    struct nk_style_button stbtn = ctx->style.button;
    stbtn.border = 0;
    stbtn.rounding = 0;
    stbtn.padding.x = stbtn.padding.y = 0;
    for (int chan = 0; chan < NUM_CHANNELS; chan++) {
      char label[32];
      int enabled;
      struct nk_color clrtxt, clrbk;
      nk_layout_row_begin(ctx, NK_STATIC, opt_fontsize, 2);
      nk_layout_row_push(ctx, 3 * opt_fontsize);
      sprintf(label, "%2d", chan);
      enabled = channel_getenabled(chan);
      if (checkbox_tooltip(ctx, label, &enabled, NK_TEXT_LEFT, "Enable/disable this channel")) {
        /* enable/disable channel in the target */
        channel_setenabled(chan, enabled);
        if (state->init_target) {
          if (enabled)
            state->channelmask |= (1 << chan);
          else
            state->channelmask &= ~(1 << chan);
          if (state->trace_status != TRACESTAT_NO_CONNECT) {
            const DWARF_SYMBOLLIST *symbol = dwarf_sym_from_name(&dwarf_symboltable, "TRACESWO_TER", -1, -1);
            unsigned long params[2];
            params[0] = state->channelmask;
            params[1] = (symbol != NULL) ? (unsigned long)symbol->data_addr : ~0;
            bmp_runscript("swo_channels", state->mcu_driver, state->mcu_architecture, params);
          }
        }
      }
      clrbk = channel_getcolor(chan);
      clrtxt = (clrbk.r + 2 * clrbk.g + clrbk.b < 700) ? nk_rgb(255,255,255) : nk_rgb(20,29,38);
      stbtn.normal.data.color = stbtn.hover.data.color
        = stbtn.active.data.color = stbtn.text_background = clrbk;
      stbtn.text_normal = stbtn.text_active = stbtn.text_hover = clrtxt;
      nk_layout_row_push(ctx, labelwidth);
      struct nk_rect bounds = nk_widget_bounds(ctx);
      if (nk_button_label_styled(ctx, &stbtn, channel_getname(chan, NULL, 0))) {
        /* we want a contextual pop-up (that you can simply click away
           without needing a close button), so we simulate a right-mouse
           click */
        nk_input_motion(ctx, bounds.x, bounds.y + bounds.h - 1);
        nk_input_button(ctx, NK_BUTTON_RIGHT, bounds.x, bounds.y + bounds.h - 1, 1);
        nk_input_button(ctx, NK_BUTTON_RIGHT, bounds.x, bounds.y + bounds.h - 1, 0);
      }
      nk_layout_row_end(ctx);
      if (nk_contextual_begin(ctx, 0, nk_vec2(9*opt_fontsize, 5*ROW_HEIGHT), bounds)) {
        nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
        clrbk.r = (nk_byte)nk_propertyi(ctx, "#R", 0, clrbk.r, 255, 1, 1);
        nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
        clrbk.g = (nk_byte)nk_propertyi(ctx, "#G", 0, clrbk.g, 255, 1, 1);
        nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
        clrbk.b = (nk_byte)nk_propertyi(ctx, "#B", 0, clrbk.b, 255, 1, 1);
        channel_setcolor(chan, clrbk);
        /* the name in the channels array must only be changed on closing
           the popup, so it is copied to a local variable on first opening */
        if (state->cur_chan_edit == -1) {
          state->cur_chan_edit = chan;
          channel_getname(chan, state->chan_str, sizearray(state->chan_str));
        }
        nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 2, nk_ratio(2, 0.35, 0.65));
        nk_label(ctx, "name", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD|NK_EDIT_CLIPBOARD,
                                       state->chan_str, sizearray(state->chan_str),
                                       nk_filter_ascii);
        nk_contextual_end(ctx);
      } else if (state->cur_chan_edit == chan) {
        /* contextual popup is closed, copy the name back */
        if (strlen(state->chan_str) == 0) {
          channel_setname(chan, NULL);
        } else {
          char *pspace;
          while ((pspace = strchr(state->chan_str, ' ')) != NULL)
            *pspace = '-'; /* can't handle spaces in the channel names */
          channel_setname(chan, state->chan_str);
        }
        state->cur_chan_edit = -1;
      }
    }
    nk_tree_state_pop(ctx);
  }
}

static void button_bar(struct nk_context *ctx, APPSTATE *state)
{
  nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 7, nk_ratio(7, 0.19, 0.08, 0.19, 0.08, 0.19, 0.08, 0.19));
  const char *ptr = state->trace_running ? "Stop" : tracestring_isempty() ? "Start" : "Resume";
  if (nk_button_label(ctx, ptr) || nk_input_is_key_pressed(&ctx->input, NK_KEY_F5)) {
    state->trace_running = !state->trace_running;
    if (state->trace_running && state->trace_status != TRACESTAT_OK) {
      state->trace_status = trace_init(state->trace_endpoint, (state->probe == state->netprobe) ? state->IPaddr : NULL);
      if (state->trace_status != TRACESTAT_OK)
        state->trace_running = nk_false;
    }
  }
  nk_spacing(ctx, 1);
  if (nk_button_label(ctx, "Clear")) {
    tracestring_clear();
    state->cur_match_line = -1;
  }
  nk_spacing(ctx, 1);
  if (nk_button_label(ctx, "Search") || nk_input_is_key_pressed(&ctx->input, NK_KEY_FIND))
    state->find_popup = 1;
  nk_spacing(ctx, 1);
  if (nk_button_label(ctx, "Save") || nk_input_is_key_pressed(&ctx->input, NK_KEY_SAVE)) {
    const char *s = noc_file_dialog_open(NOC_FILE_DIALOG_SAVE,
                                         "CSV files\0*.csv\0All files\0*.*\0",
                                         NULL, NULL, NULL, guidriver_apphandle());
    if (s != NULL) {
      trace_save(s);
      free((void*)s);
    }
  }
}

static void handle_stateaction(APPSTATE *state)
{
  if (state->reinitialize == 1) {
    int result;
    char msg[100];
    tracelog_statusclear();
    tracestring_clear();
    if ((state->cpuclock = strtol(state->cpuclock_str, NULL, 10)) == 0)
      state->cpuclock = 48000000;
    if (state->mode == MODE_MANCHESTER || (state->bitrate = strtol(state->bitrate_str, NULL, 10)) == 0)
      state->bitrate = 100000;
    if (state->init_target || state->init_bmp) {
      /* open/reset the serial port/device if any initialization must be done */
      if (bmp_comport() != NULL)
        bmp_break();
      result = bmp_connect(state->probe, (state->probe == state->netprobe) ? state->IPaddr : NULL);
      if (result) /* bmp_connect() also opens the (virtual) serial port/device */
        result = bmp_attach(2, state->connect_srst,
                            state->mcu_driver, sizearray(state->mcu_driver),
                            state->mcu_architecture, sizearray(state->mcu_architecture));
      else
        state->trace_status = TRACESTAT_NO_CONNECT;
      if (result) {
        /* overrule any default protocol setting, if the debug probe can be
           verified */
        state->probe_type = bmp_checkversionstring();
        if (state->probe_type == PROBE_ORG_BMP)
          state->mode = MODE_MANCHESTER;
        else if (state->probe_type == PROBE_CTXLINK)
         state->mode = MODE_ASYNC;
      }
      if (result && state->init_target) {
        /* initialize the target (target-specific configuration, generic
           configuration and channels */
        unsigned long params[4];
        const DWARF_SYMBOLLIST *symbol;
        bmp_runscript("swo_device", state->mcu_driver, state->mcu_architecture, NULL);
        assert(state->mode == MODE_MANCHESTER || state->mode == MODE_ASYNC);
        symbol = dwarf_sym_from_name(&dwarf_symboltable, "TRACESWO_BPS", -1, -1);
        params[0] = state->mode;
        params[1] = state->cpuclock / state->bitrate - 1;
        params[2] = state->bitrate;
        params[3] = (symbol != NULL) ? (unsigned long)symbol->data_addr : ~0;
        bmp_runscript("swo_generic", state->mcu_driver, state->mcu_architecture, params);
        /* enable active channels in the target (disable inactive channels) */
        state->channelmask = 0;
        for (int chan = 0; chan < NUM_CHANNELS; chan++)
          if (channel_getenabled(chan))
            state->channelmask |= (1 << chan);
        symbol = dwarf_sym_from_name(&dwarf_symboltable, "TRACESWO_TER", -1, -1);
        params[0] = state->channelmask;
        params[1] = (symbol != NULL) ? (unsigned long)symbol->data_addr : ~0;
        bmp_runscript("swo_channels", state->mcu_driver, state->mcu_architecture, params);
      }
    } else if (bmp_isopen()) {
      /* no initialization is requested, if the serial port is open, close it
         (so that the gdbserver inside the BMP is available for debugging) */
      bmp_disconnect();
      result = 1; /* flag status = ok, to drop into the next "if" */
    }
    if (result) {
      if (state->init_bmp)
        bmp_enabletrace((state->mode == MODE_ASYNC) ? state->bitrate : 0, &state->trace_endpoint);
      /* trace_init() does nothing if initialization had already succeeded */
      if (state->probe == state->netprobe)
        state->trace_status = trace_init(BMP_PORT_TRACE, state->IPaddr);
      else
        state->trace_status = trace_init(state->trace_endpoint, NULL);
      bmp_restart();
    }
    state->trace_running = (state->trace_status == TRACESTAT_OK);
    switch (state->trace_status) {
    case TRACESTAT_OK:
      if (state->init_target || state->init_bmp) {
        assert(strlen(state->mcu_driver) > 0);
        sprintf(msg, "Connected [%s]", state->mcu_driver);
        tracelog_statusmsg(TRACESTATMSG_BMP, msg, BMPSTAT_SUCCESS);
      } else {
        tracelog_statusmsg(TRACESTATMSG_BMP, "Listening (passive mode)...", BMPSTAT_SUCCESS);
      }
      break;
    case TRACESTAT_INIT_FAILED:
    case TRACESTAT_NO_INTERFACE:
    case TRACESTAT_NO_DEVPATH:
    case TRACESTAT_NO_PIPE:
      strlcpy(msg, "Trace interface not available", sizearray(msg));
      if (state->probe == state->netprobe && state->mode != MODE_ASYNC)
        strlcat(msg, "; try NRZ/Async mode", sizearray(msg));
      tracelog_statusmsg(TRACESTATMSG_BMP, msg, BMPERR_GENERAL);
      break;
    case TRACESTAT_NO_ACCESS:
      { int loc;
        unsigned long error = trace_errno(&loc);
        sprintf(msg, "Trace access denied (error %d:%lu)", loc, error);
        tracelog_statusmsg(TRACESTATMSG_BMP, msg, BMPERR_GENERAL);
      }
      break;
    case TRACESTAT_NO_THREAD:
      { int loc;
        unsigned long error = trace_errno(&loc);
        sprintf(msg, "Multi-threading set-up failure (error %d:%lu)", loc, error);
        tracelog_statusmsg(TRACESTATMSG_BMP, msg, BMPERR_GENERAL);
      }
      break;
    case TRACESTAT_NO_CONNECT:
      tracelog_statusmsg(TRACESTATMSG_BMP, "Failed to \"attach\" to Black Magic Probe", BMPERR_GENERAL);
      break;
    }
    state->reinitialize = nk_false;
  } else if (state->reinitialize > 0) {
    state->reinitialize -= 1;
  }

  if (state->reload_format) {
    ctf_parse_cleanup();
    ctf_decode_cleanup();
    tracestring_clear();
    dwarf_cleanup(&dwarf_linetable, &dwarf_symboltable, &dwarf_filetable);
    state->cur_match_line = -1;
    state->error_flags = 0;
    if (strlen(state->TSDLfile) > 0)
      state->error_flags |= ERROR_NO_TSDL;
    if (strlen(state->TSDLfile)> 0 && access(state->TSDLfile, 0)== 0) {
      if (ctf_parse_init(state->TSDLfile) && ctf_parse_run()) {
        const CTF_STREAM *stream;
        int seqnr;
        /* stream names overrule configured channel names */
        for (seqnr = 0; (stream = stream_by_seqnr(seqnr)) != NULL; seqnr++)
          if (stream->name != NULL && strlen(stream->name) > 0)
            channel_setname(seqnr, stream->name);
        state->error_flags &= ~ERROR_NO_TSDL;
        tracelog_statusmsg(TRACESTATMSG_CTF, "CTF mode active", BMPSTAT_SUCCESS);
      } else {
        ctf_parse_cleanup();
      }
    }
    if (strlen(state->ELFfile) > 0)
      state->error_flags |= ERROR_NO_ELF;
    if (strlen(state->ELFfile) > 0 && access(state->ELFfile, 0) == 0) {
      FILE *fp = fopen(state->ELFfile, "rb");
      if (fp != NULL) {
        int address_size;
        dwarf_read(fp, &dwarf_linetable, &dwarf_symboltable, &dwarf_filetable, &address_size);
        fclose(fp);
        state->error_flags &= ~ERROR_NO_ELF;
      }
    }
    state->reload_format = nk_false;
  }
}

int main(int argc, char *argv[])
{
  struct nk_context *ctx;
  SPLITTERBAR splitter_hor, splitter_ver;
  int canvas_width, canvas_height;
  enum nk_collapse_states tab_states[TAB_COUNT];
  APPSTATE appstate;
  char txtConfigFile[_MAX_PATH], valstr[128] = "";
  int waitidle;
  char opt_fontstd[64] = "", opt_fontmono[64] = "";

  /* global defaults */
  memset(&appstate, 0, sizeof appstate);
  appstate.reinitialize = nk_true;
  appstate.reload_format = nk_true;
  appstate.trace_status = TRACESTAT_NOT_INIT;
  appstate.trace_running = nk_true;
  appstate.mode = MODE_MANCHESTER;
  appstate.probe_type = PROBE_UNKNOWN;
  appstate.trace_endpoint = BMP_EP_TRACE;
  appstate.init_target = nk_true;
  appstate.init_bmp = nk_true;
  appstate.connect_srst = nk_false;
  appstate.cur_chan_edit = -1;
  appstate.cur_match_line = -1;
  /* locate the configuration file for settings */
  get_configfile(txtConfigFile, sizearray(txtConfigFile), "bmtrace.ini");

  /* read channel configuration */
  for (int chan = 0; chan < NUM_CHANNELS; chan++) {
    char key[41];
    unsigned clr;
    int enabled, result;
    channel_set(chan, (chan == 0), NULL, nk_rgb(190, 190, 190)); /* preset: port 0 is enabled by default, others disabled by default */
    sprintf(key, "chan%d", chan);
    ini_gets("Channels", key, "", valstr, sizearray(valstr), txtConfigFile);
    result = sscanf(valstr, "%d #%x %40s", &enabled, &clr, key);
    if (result >= 2)
      channel_set(chan, enabled, (result >= 3) ? key : NULL, nk_rgb(clr >> 16,(clr >> 8) & 0xff, clr & 0xff));
  }
  /* read filters (initialize the filter list) */
  appstate.filtercount = ini_getl("Filters", "count", 0, txtConfigFile);;
  appstate.filterlistsize = appstate.filtercount + 1; /* at least 1 extra, for a NULL sentinel */
  appstate.filterlist = malloc(appstate.filterlistsize * sizeof(TRACEFILTER));  /* make sure unused entries are NULL */
  if (appstate.filterlist != NULL) {
    memset(appstate.filterlist, 0, appstate.filterlistsize * sizeof(TRACEFILTER));
    int idx;
    for (idx = 0; idx < appstate.filtercount; idx++) {
      char key[40], *ptr;
      appstate.filterlist[idx].expr = malloc(sizearray(appstate.newfiltertext) * sizeof(char));
      if (appstate.filterlist[idx].expr == NULL)
        break;
      sprintf(key, "filter%d", idx + 1);
      ini_gets("Filters", key, "", appstate.newfiltertext, sizearray(appstate.newfiltertext), txtConfigFile);
      appstate.filterlist[idx].enabled = (int)strtol(appstate.newfiltertext, &ptr, 10);
      assert(ptr != NULL && *ptr != '\0');  /* a comma should be found */
      if (*ptr == ',')
        ptr += 1;
      strcpy(appstate.filterlist[idx].expr, ptr);
    }
    appstate.filtercount = idx;
  } else {
    appstate.filtercount = appstate.filterlistsize = 0;
  }
  appstate.newfiltertext[0] = '\0';

  /* other configuration */
  appstate.probe = (int)ini_getl("Settings", "probe", 0, txtConfigFile);
  ini_gets("Settings", "ip-address", "127.0.0.1", appstate.IPaddr, sizearray(appstate.IPaddr), txtConfigFile);
  appstate.mode = (int)ini_getl("Settings", "mode", MODE_MANCHESTER, txtConfigFile);
  appstate.init_target = (int)ini_getl("Settings", "init-target", 1, txtConfigFile);
  appstate.init_bmp = (int)ini_getl("Settings", "init-bmp", 1, txtConfigFile);
  if (appstate.mode == 0) {  /* legacy: appstate.mode == 0 was MODE_PASSIVE */
    appstate.mode = MODE_MANCHESTER;
    appstate.init_target = 0;
    appstate.init_bmp = 0;
  }
  appstate.connect_srst = (int)ini_getl("Settings", "connect-srst", 0, txtConfigFile);
  appstate.datasize = (int)ini_getl("Settings", "datasize", 1, txtConfigFile);
  ini_gets("Settings", "tsdl", "", appstate.TSDLfile, sizearray(appstate.TSDLfile), txtConfigFile);
  ini_gets("Settings", "elf", "", appstate.ELFfile, sizearray(appstate.ELFfile), txtConfigFile);
  ini_gets("Settings", "mcu-freq", "48000000", appstate.cpuclock_str, sizearray(appstate.cpuclock_str), txtConfigFile);
  ini_gets("Settings", "bitrate", "100000", appstate.bitrate_str, sizearray(appstate.bitrate_str), txtConfigFile);
  ini_gets("Settings", "size", "", valstr, sizearray(valstr), txtConfigFile);
  opt_fontsize = ini_getf("Settings", "fontsize", FONT_HEIGHT, txtConfigFile);
  ini_gets("Settings", "fontstd", "", opt_fontstd, sizearray(opt_fontstd), txtConfigFile);
  ini_gets("Settings", "fontmono", "", opt_fontmono, sizearray(opt_fontmono), txtConfigFile);
  if (sscanf(valstr, "%d %d", &canvas_width, &canvas_height) != 2 || canvas_width < 100 || canvas_height < 50) {
    canvas_width = WINDOW_WIDTH;
    canvas_height = WINDOW_HEIGHT;
  }
  ini_gets("Settings", "timeline", "", valstr, sizearray(valstr), txtConfigFile);
  if (strlen(valstr) > 0) {
    double spacing;
    unsigned long scale, delta;
    if (sscanf(valstr, "%lf %lu %lu", &spacing, &scale, &delta) == 3)
      timeline_setconfig(spacing, scale, delta);
  }

  ini_gets("Settings", "splitter", "", valstr, sizearray(valstr), txtConfigFile);
  splitter_hor.ratio = splitter_ver.ratio = 0.0;
  sscanf(valstr, "%f %f", &splitter_hor.ratio, &splitter_ver.ratio);
  if (splitter_hor.ratio < 0.05 || splitter_hor.ratio > 0.95)
    splitter_hor.ratio = 0.70;
  if (splitter_ver.ratio < 0.05 || splitter_ver.ratio > 0.95)
    splitter_ver.ratio = 0.70;
  #define SEPARATOR_HOR 4
  #define SEPARATOR_VER 4
  #define SPACING       4
  nk_splitter_init(&splitter_hor, canvas_width - 3 * SPACING, SEPARATOR_HOR, splitter_hor.ratio);
  nk_splitter_init(&splitter_ver, canvas_height - (ROW_HEIGHT + 8 * SPACING), SEPARATOR_VER, splitter_ver.ratio);

  for (int idx = 0; idx < TAB_COUNT; idx++) {
    char key[40];
    int opened, result;
    tab_states[idx] = (idx == TAB_CONFIGURATION) ? NK_MAXIMIZED : NK_MINIMIZED;
    sprintf(key, "view%d", idx);
    ini_gets("Settings", key, "", valstr, sizearray(valstr), txtConfigFile);
    result = sscanf(valstr, "%d", &opened);
    if (result >= 1)
      tab_states[idx] = opened;
  }

  for (int idx = 1; idx < argc; idx++) {
    if (IS_OPTION(argv[idx])) {
      const char *ptr;
      float h;
      switch (argv[idx][1]) {
      case '?':
      case 'h':
        usage(NULL);
        return 0;
      case 'f':
        ptr = &argv[idx][2];
        if (*ptr == '=' || *ptr == ':')
          ptr++;
        h = (float)strtod(ptr, (char**)&ptr);
        if (h >= 8.0)
          opt_fontsize = h;
        if (*ptr == ',') {
          char *mono;
          ptr++;
          if ((mono = strchr(ptr, ',')) != NULL)
            *mono++ = '\0';
          if (*ptr != '\0')
            strlcpy(opt_fontstd, ptr, sizearray(opt_fontstd));
          if (mono != NULL && *mono == '\0')
            strlcpy(opt_fontmono, mono, sizearray(opt_fontmono));
        }
        break;
      case 't':
        ptr = &argv[idx][2];
        if (*ptr == '=' || *ptr == ':')
          ptr++;
        if (access(ptr, 0) == 0)
          strlcpy(appstate.TSDLfile, ptr, sizearray(appstate.TSDLfile));
        break;
      default:
        usage(argv[idx]);
        return EXIT_FAILURE;
      }
    } else if (access(argv[idx], 0) == 0) {
      /* parameter is a filename, test whether that is an ELF file */
      FILE *fp = fopen(argv[idx], "rb");
      if (fp != NULL) {
        int err = elf_info(fp, NULL, NULL, NULL, NULL);
        if (err == ELFERR_NONE) {
          strlcpy(appstate.ELFfile, argv[idx], sizearray(appstate.ELFfile));
          if (access(appstate.TSDLfile, 0) != 0) {
            /* see whether there is a TSDL file with a matching name */
            char *ext;
            strlcpy(appstate.TSDLfile, appstate.ELFfile, sizearray(appstate.TSDLfile));
            ext = strrchr(appstate.TSDLfile, '.');
            if (ext != NULL && strpbrk(ext, "\\/") == NULL)
              *ext = '\0';
            strlcat(appstate.TSDLfile, ".tsdl", sizearray(appstate.TSDLfile));
            if (access(appstate.TSDLfile, 0) != 0)
              appstate.TSDLfile[0] = '\0';  /* newly constructed file not found, clear name */
          }
        }
        fclose(fp);
      }
    }
  }

  /* collect debug probes, initialize interface */
  appstate.probelist = get_probelist(&appstate.probe, &appstate.netprobe);
  trace_setdatasize((appstate.datasize == 3) ? 4 : (short)appstate.datasize);
  tcpip_init();
  bmp_setcallback(bmp_callback);
  appstate.reinitialize = 2; /* skip first iteration, so window is updated */
  tracelog_statusmsg(TRACESTATMSG_BMP, "Initializing...", BMPSTAT_SUCCESS);

  ctx = guidriver_init("BlackMagic Trace Viewer", canvas_width, canvas_height,
                       GUIDRV_RESIZEABLE | GUIDRV_TIMER, opt_fontstd, opt_fontmono, opt_fontsize);
  nuklear_style(ctx);

  waitidle = 1;
  for ( ;; ) {
    /* handle state, (re-)connect and/or (re-)load of CTF definitions */
    handle_stateaction(&appstate);

    /* Input */
    nk_input_begin(ctx);
    if (!guidriver_poll(waitidle))
      break;
    nk_input_end(ctx);

    /* GUI */
    guidriver_appsize(&canvas_width, &canvas_height);
    if (nk_begin(ctx, "MainPanel", nk_rect(0, 0, canvas_width, canvas_height), NK_WINDOW_NO_SCROLLBAR)) {
      nk_splitter_resize(&splitter_hor, canvas_width - 3 * SPACING, RESIZE_TOPLEFT);
      nk_splitter_resize(&splitter_ver, canvas_height - (ROW_HEIGHT + 8 * SPACING), RESIZE_TOPLEFT);
      nk_hsplitter_layout(ctx, &splitter_hor, canvas_height - 2 * SPACING);
      ctx->style.window.padding.x = 2;
      ctx->style.window.padding.y = 2;
      ctx->style.window.group_padding.x = 0;
      ctx->style.window.group_padding.y = 0;

      /* left column */
      if (nk_group_begin(ctx, "left", NK_WINDOW_NO_SCROLLBAR)) {
        /* trace log */
        if (appstate.trace_status == TRACESTAT_OK && tracestring_isempty() && trace_getpacketerrors() > 0) {
          char msg[100];
          sprintf(msg, "SWO packet errors (%d), verify data size", trace_getpacketerrors());
          tracelog_statusmsg(TRACESTATMSG_BMP, msg, BMPERR_GENERAL);
        }
        waitidle = tracestring_process(appstate.trace_running) == 0;
        nk_layout_row_dynamic(ctx, nk_vsplitter_rowheight(&splitter_ver, 0), 1);
        tracelog_widget(ctx, "tracelog", opt_fontsize, appstate.cur_match_line, appstate.filterlist, NK_WINDOW_BORDER);

        /* vertical splitter */
        nk_vsplitter(ctx, &splitter_ver);

        /* timeline & button bar */
        nk_layout_row_dynamic(ctx, nk_vsplitter_rowheight(&splitter_ver, 1), 1);
        double click_time = timeline_widget(ctx, "timeline", opt_fontsize, NK_WINDOW_BORDER);
        appstate.cur_match_line = (click_time >= 0.0) ? tracestring_findtimestamp(click_time) : -1;

        nk_layout_row_dynamic(ctx, SPACING, 1);
        button_bar(ctx, &appstate);

        nk_group_end(ctx);
      }

      /* column splitter */
      nk_hsplitter(ctx, &splitter_hor);

      /* right column */
      if (nk_group_begin(ctx, "right", NK_WINDOW_BORDER)) {
        panel_options(ctx, &appstate, tab_states, nk_hsplitter_colwidth(&splitter_hor, 1));
        filter_options(ctx, &appstate, tab_states);
        channel_options(ctx, &appstate, tab_states);
        nk_group_end(ctx);
      }

      /* popup dialogs */
      find_popup(ctx, &appstate, canvas_width, canvas_height);

      /* mouse cursor shape */
      if (nk_is_popup_open(ctx))
        pointer_setstyle(CURSOR_NORMAL);
      else if (splitter_ver.hover)
        pointer_setstyle(CURSOR_UPDOWN);
      else if (splitter_hor.hover)
        pointer_setstyle(CURSOR_LEFTRIGHT);
      else
        pointer_setstyle(CURSOR_NORMAL);
    }
    nk_end(ctx);

    /* Draw */
    guidriver_render(nk_rgb(30,30,30));
  }

  /* save configuration */
  for (int chan = 0; chan < NUM_CHANNELS; chan++) {
    char key[40];
    struct nk_color color = channel_getcolor(chan);
    sprintf(key, "chan%d", chan);
    sprintf(valstr, "%d #%06x %s", channel_getenabled(chan),
            ((int)color.r << 16) | ((int)color.g << 8) | color.b,
            channel_getname(chan, NULL, 0));
    ini_puts("Channels", key, valstr, txtConfigFile);
  }
  ini_putl("Filters", "count", appstate.filtercount, txtConfigFile);
  for (int idx = 0; idx < appstate.filtercount; idx++) {
    char key[40], expr[FILTER_MAXSTRING+10];
    assert(appstate.filterlist != NULL && appstate.filterlist[idx].expr != NULL);
    sprintf(key, "filter%d", idx + 1);
    sprintf(expr, "%d,%s", appstate.filterlist[idx].enabled, appstate.filterlist[idx].expr);
    ini_puts("Filters", key, expr, txtConfigFile);
    free(appstate.filterlist[idx].expr);
  }
  if (appstate.filterlist != NULL)
    free(appstate.filterlist);
  sprintf(valstr, "%.2f %.2f", splitter_hor.ratio, splitter_ver.ratio);
  ini_puts("Settings", "splitter", valstr, txtConfigFile);
  for (int idx = 0; idx < TAB_COUNT; idx++) {
    char key[40];
    sprintf(key, "view%d", idx);
    sprintf(valstr, "%d", tab_states[idx]);
    ini_puts("Settings", key, valstr, txtConfigFile);
  }
  ini_putf("Settings", "fontsize", opt_fontsize, txtConfigFile);
  ini_puts("Settings", "fontstd", opt_fontstd, txtConfigFile);
  ini_puts("Settings", "fontmono", opt_fontmono, txtConfigFile);
  ini_putl("Settings", "mode", appstate.mode, txtConfigFile);
  ini_putl("Settings", "init-target", appstate.init_target, txtConfigFile);
  ini_putl("Settings", "init-bmp", appstate.init_bmp, txtConfigFile);
  ini_putl("Settings", "connect-srst", appstate.connect_srst, txtConfigFile);
  ini_putl("Settings", "datasize", appstate.datasize, txtConfigFile);
  ini_puts("Settings", "tsdl", appstate.TSDLfile, txtConfigFile);
  ini_puts("Settings", "elf", appstate.ELFfile, txtConfigFile);
  ini_putl("Settings", "mcu-freq", appstate.cpuclock, txtConfigFile);
  ini_putl("Settings", "bitrate", appstate.bitrate, txtConfigFile);
  sprintf(valstr, "%d %d", canvas_width, canvas_height);
  ini_puts("Settings", "size", valstr, txtConfigFile);
  {
    double spacing;
    unsigned long scale, delta;
    timeline_getconfig(&spacing, &scale, &delta);
    sprintf(valstr, "%.2f %lu %lu", spacing, scale, delta);
    ini_puts("Settings", "timeline", valstr, txtConfigFile);
  }
  if (bmp_is_ip_address(appstate.IPaddr))
    ini_puts("Settings", "ip-address", appstate.IPaddr, txtConfigFile);
  ini_putl("Settings", "probe", (appstate.probe == appstate.netprobe) ? 99 : appstate.probe, txtConfigFile);

  clear_probelist(appstate.probelist, appstate.netprobe);
  trace_close();
  guidriver_close();
  tracestring_clear();
  bmscript_clear();
  gdbrsp_packetsize(0);
  ctf_parse_cleanup();
  ctf_decode_cleanup();
  dwarf_cleanup(&dwarf_linetable, &dwarf_symboltable, &dwarf_filetable);
  bmp_disconnect();
  tcpip_cleanup();
  return EXIT_SUCCESS;
}

