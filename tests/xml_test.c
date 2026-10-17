/* json/xml.h: the pull reader -- the declaration, a doctype's subset and
 * comments skipped, attributes however quoted, text, CDATA, a stray '<',
 * content plain and raw -- and the entities, XML's own or a caller's. */
#include "json/xml.h"

#include "ctui.h"
#include "ctui_test.h"

#include <stdlib.h>
#include <string.h>

static void test_tokens(void) {
  const char *doc = "<?xml version=\"1.0\" encoding='ISO-8859-1'?>\n"
                    "<!DOCTYPE x [<!ENTITY a \"b\">]><!-- c > d -->"
                    "<rss a=\"1 > 2\" b='x' c=plain><t>A &amp; B &#233;</t>"
                    "<e/><![CDATA[<raw> & ]]>1 < 2</rss>";
  CTUI_XML x;
  ctui_xml_init(&x, doc, strlen(doc));
  int t = ctui_xml_next(&x);
  while (t == CTUI_XML_TEXT) {
    t = ctui_xml_next(&x);
  }
  const char *v;
  size_t len;
  CTUI_TEST_ASSERT(t == CTUI_XML_OPEN && ctui_xml_is(&x, "RSS") &&
                       !strcmp(x.encoding, "ISO-8859-1"),
                   "declaration, doctype subset and comment skipped; its "
                   "encoding kept");
  CTUI_TEST_ASSERT(ctui_xml_attr(&x, "a", &v, &len) && len == 5 &&
                       !strncmp(v, "1 > 2", 5) &&
                       ctui_xml_attr(&x, "b", &v, &len) && len == 1 &&
                       ctui_xml_attr(&x, "c", &v, &len) && len == 5 &&
                       !ctui_xml_attr(&x, "d", &v, &len),
                   "attributes: quoted (a '>' inside), single, bare");
  ctui_xml_next(&x);
  CTUI_BUF b = {0};
  ctui_xml_content(&x, 0, &b);
  CTUI_TEST_ASSERT(b.s && !strcmp(b.s, "A & B \xc3\xa9"),
                   "an element's text, entities decoded");
  free(b.s);
  t = ctui_xml_next(&x);
  CTUI_TEST_ASSERT(t == CTUI_XML_OPEN && x.self && ctui_xml_is(&x, "e"),
                   "a self-closing element");
  t = ctui_xml_next(&x);
  CTUI_TEST_ASSERT(t == CTUI_XML_CDATA && x.text_len == 8 &&
                       !strncmp(x.text, "<raw> & ", 8),
                   "CDATA raw");
  t = ctui_xml_next(&x);
  CTUI_TEST_ASSERT(t == CTUI_XML_TEXT && x.text_len == 2, "text up to a '<'");
  t = ctui_xml_next(&x);
  CTUI_TEST_ASSERT(t == CTUI_XML_TEXT && !strncmp(x.text, "< 2", 3),
                   "a '<' that starts no tag is text");
  t = ctui_xml_next(&x);
  CTUI_TEST_ASSERT(t == CTUI_XML_CLOSE && ctui_xml_is(&x, "rss") &&
                       ctui_xml_next(&x) == CTUI_XML_END,
                   "the close, then the end");

  const char *nest = "<c type=\"xhtml\"><div><c>in</c><br>x</div></c>after";
  ctui_xml_init(&x, nest, strlen(nest));
  ctui_xml_next(&x);
  b = (CTUI_BUF){0};
  ctui_xml_content(&x, 1, &b);
  CTUI_TEST_ASSERT(b.s && !strcmp(b.s, "<div><c>in</c><br>x</div>") &&
                       ctui_xml_next(&x) == CTUI_XML_TEXT &&
                       !strncmp(x.text, "after", 5),
                   "raw content up to its own close, the same name nested "
                   "and a stray <br> inside");
  free(b.s);
}

static char *unescaped(const char *s, CTUI_XML_ENTITY entity) {
  CTUI_BUF b = {0};
  ctui_xml_unescape(s, strlen(s), entity, &b);
  ctui_buf_add(&b, "", 0);
  return b.s;
}

/* a caller's table: one name of HTML's, the rest XML's */
static size_t nbsp(const char *s, size_t n, char out[8]) {
  if (n >= 6 && !memcmp(s, "&nbsp;", 6)) {
    strcpy(out, "\xc2\xa0");
    return 6;
  }
  return ctui_xml_entity(s, n, out);
}

static void test_entities(void) {
  char *s = unescaped("&lt;a&gt; &quot;&apos;&amp;", NULL);
  CTUI_TEST_ASSERT(s && !strcmp(s, "<a> \"'&"), "the five named ones");
  free(s);
  s = unescaped("&#65;&#x42;&#X43;&#x1F600;", NULL);
  CTUI_TEST_ASSERT(s && !strcmp(s, "ABC\xf0\x9f\x98\x80"),
                   "numeric, decimal and hex");
  free(s);
  s = unescaped("&#0;&#xD800;&#x110000;", NULL);
  CTUI_TEST_ASSERT(s && !strcmp(s, "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"),
                   "zero, a surrogate, past U+10FFFF: U+FFFD");
  free(s);
  s = unescaped("&nbsp; &amp &#x; &#12 & &unknown;", NULL);
  CTUI_TEST_ASSERT(s && !strcmp(s, "&nbsp; &amp &#x; &#12 & &unknown;"),
                   "what isn't an entity stays as written");
  free(s);
  s = unescaped("a&nbsp;b&amp;", nbsp);
  CTUI_TEST_ASSERT(s && !strcmp(s, "a\xc2\xa0"
                                   "b&"),
                   "a caller's entities");
  free(s);

  const char *doc = "<t>&nbsp;&lt;</t>";
  CTUI_XML x;
  ctui_xml_init(&x, doc, strlen(doc));
  x.entity = nbsp;
  ctui_xml_next(&x);
  CTUI_BUF b = {0};
  ctui_xml_content(&x, 0, &b);
  CTUI_TEST_ASSERT(b.s && !strcmp(b.s, "\xc2\xa0<"),
                   "content through the reader's entity hook");
  free(b.s);
}

int main(void) {
  ctui_log_init(E_ALL);
  test_tokens();
  test_entities();
  return ctui_test_summary();
}
