#ifndef CTUI_TERM_H
#define CTUI_TERM_H

/* terminal lifecycle; verbosity is a bitmask of E_DBG/E_WRN/E_INF/E_ERR
 * (see logger.h) controlling which ctui_log/ctui_logf calls actually get
 * written */
int ctui_init(int verbosity);
void ctui_shutdown(void);
void ctui_get_termsize(int *rows, int *cols);

#endif
