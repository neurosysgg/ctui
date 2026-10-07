#ifndef CTUI_JSON_JSONEDIT_H
#define CTUI_JSON_JSONEDIT_H

#include "json/json.h"

#include <stddef.h>

/* (Promoted from ctui-wm's base/jsonedit, its second user ctui-mus.)
 *
 * Edits of a hand-written config (JSON with comments, json.h) in place:
 * an edit touches only the bytes of the member it's about, so comments,
 * layout and every other byte stay as the user wrote them. A path is the
 * object keys from the top, NULL-terminated; an array is a value like any
 * other (a changed one is written again whole, comments between its items
 * lost), or edited item by item: a path element "[N]" steps into an
 * array's item N (from 0), and insert / swap below work on items, their
 * comments kept. Every result is parsed again and compared with the tree the
 * edit should have made; one that doesn't match is refused, not returned. */

/* text with path's value set to v: replaced where it is, else added as
 * the last member of its object (with the objects on the way that are
 * missing), indented like its siblings. A new string, or NULL with a
 * message in err (a path through a non-object, text that doesn't parse) */
char *ctui_json_edit_set(const char *text, const char *const *path,
                         const CTUI_JSON *v, char *err, size_t err_cap);

/* text without path's member (its line too, when it had one of its own;
 * comments above it stay): a new string (a copy when there's no such
 * member), or NULL with err */
char *ctui_json_edit_remove(const char *text, const char *const *path,
                            char *err, size_t err_cap);

/* path ending in "[N]": set replaces item N, remove takes it out with the
 * comment lines right above it (a widget's doc comment). */

/* text with v as item at of the array at path (at = its count: after the
 * last), before the item there and its comment, indented like the items:
 * a new string, or NULL with err */
char *ctui_json_edit_insert(const char *text, const char *const *path, int at,
                            const CTUI_JSON *v, char *err, size_t err_cap);

/* text with items i and i + 1 of the array at path swapped, each with the
 * comment lines above it; the commas stay where they were: a new string,
 * or NULL with err */
char *ctui_json_edit_swap(const char *text, const char *const *path, int i,
                          char *err, size_t err_cap);

/* v as a config writes it, its lines after the first indented by indent:
 * a short array or object on one line ({ "a": 1 }), a longer one a member
 * per line; strings keep their UTF-8, numbers the fewest digits that read
 * back the same */
void ctui_json_edit_write(CTUI_BUF *b, const CTUI_JSON *v, const char *indent);

/* path's contents: a new string, or NULL with err */
char *ctui_json_edit_load(const char *path, char *err, size_t err_cap);

/* text over the file at path (a symlink's target): parsed first (text
 * that doesn't parse isn't written), the old contents kept in
 * PATH.bak, the new one written to a temp file and renamed in, the old
 * file's mode kept. 0, or -1 with err */
int ctui_json_edit_save(const char *path, const char *text, char *err,
                        size_t err_cap);

#endif
