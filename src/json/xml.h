#ifndef CTUI_JSON_XML_H
#define CTUI_JSON_XML_H

#include "buf.h"

#include <stddef.h>

/* A lenient pull reader for XML (feeds, OPML, a .dawproject's
 * project.xml): tags, text and CDATA one token at a time, pointing into
 * the document (nothing copied). Comments, doctypes and processing instructions
 * are skipped (the declaration's encoding is kept); names are as written,
 * prefix and all ("media:content"); nothing is checked for balance, a '<' that
 * starts no tag is text. Pure: the tests and the fuzzer take it as is. */

enum {
  CTUI_XML_END = 0,
  CTUI_XML_OPEN,  /* <name attrs> or <name attrs/> (self) */
  CTUI_XML_CLOSE, /* </name> */
  CTUI_XML_TEXT,  /* text as written (its entities not decoded) */
  CTUI_XML_CDATA, /* <![CDATA[...]]>'s inside, raw */
};

/* an entity at s[0..n) ('&' first): its length with its UTF-8 (NUL-
 * terminated) in out, or 0 if it isn't one */
typedef size_t (*CTUI_XML_ENTITY)(const char *s, size_t n, char out[8]);

typedef struct {
  const char *s;
  size_t n, at; /* the document, and where the next token starts */
  /* the token read last: */
  int type;
  size_t tok_at;    /* where it started */
  const char *name; /* OPEN, CLOSE */
  size_t name_len;
  const char *attrs; /* OPEN: the attributes as written */
  size_t attrs_len;
  int self;         /* OPEN: <x/>, no CLOSE follows */
  const char *text; /* TEXT, CDATA */
  size_t text_len;
  char encoding[32]; /* <?xml ... encoding="..."?>, "" if none came */
  /* content's entities (set after init); NULL: ctui_xml_entity's */
  CTUI_XML_ENTITY entity;
} CTUI_XML;

void ctui_xml_init(CTUI_XML *x, const char *s, size_t n);
/* the next token: its type, CTUI_XML_END at the end */
int ctui_xml_next(CTUI_XML *x);

/* 1 if the token's name is name (any case) */
int ctui_xml_is(const CTUI_XML *x, const char *name);
/* the OPEN token's attribute name (any case): 1 with its value as
 * written (entities not decoded) in *v / *len, or 0 */
int ctui_xml_attr(const CTUI_XML *x, const char *name, const char **v,
                  size_t *len);

/* XML's own entities: the five named ones and &#N; / &#xN; (a code
 * point out of range or a surrogate as U+FFFD) */
size_t ctui_xml_entity(const char *s, size_t n, char out[8]);
/* s[0..n) with its entities decoded by entity (NULL: ctui_xml_entity),
 * appended to b */
void ctui_xml_unescape(const char *s, size_t n, CTUI_XML_ENTITY entity,
                       CTUI_BUF *b);

/* after an OPEN: the element's content appended to b, read up to its
 * CLOSE (the next </name> of the same name as deep as it opened: stray
 * tags inside don't matter). raw = 0: its text only (entities decoded,
 * CDATA as is, tags dropped); raw = 1: every byte between the tags as
 * written (XHTML content). A self-closing OPEN has none. */
void ctui_xml_content(CTUI_XML *x, int raw, CTUI_BUF *b);
/* after an OPEN: everything up to its CLOSE skipped */
void ctui_xml_skip(CTUI_XML *x);

#endif
