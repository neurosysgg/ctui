#include "picto_internal.h"

#include <string.h>

/* The art: 16 rows of 16 role letters (paint.h). Beyond the roles, a
 * template may hold '1'-'3' (the speaker's waves: shown up to its value)
 * and '*' (the battery's inside: accent up to its value, else groove);
 * '+' and '|' are the dot's and the expander's (at their art). */

#define N CTUI_LOOK_PICTO_GRID
#define BIG CTUI_LOOK_PICTO_GRID_BIG
#define ART(name, ...)                                                         \
  static const char name[] = __VA_ARGS__;                                      \
  _Static_assert(sizeof name == N * N + 1, #name " is 16x16")

ART(SPEAKER, "................"
             "................"
             "......t.....3..."
             ".....tt......3.."
             "....tht...2...3."
             "tttthht....2...3"
             "thhhhht.1...2..3"
             "thhhhht..1..2..3"
             "thhhhst..1..2..3"
             "thhhhst.1...2..3"
             "ttttsst....2...3"
             "....tst...2...3."
             ".....tt......3.."
             "......t.....3..."
             "................"
             "................");

/* over a muted speaker's waves */
ART(CROSS, "................"
           "................"
           "................"
           "................"
           "................"
           ".........tt..tt."
           "..........tttt.."
           "...........tt..."
           "...........tt..."
           "..........tttt.."
           ".........tt..tt."
           "................"
           "................"
           "................"
           "................"
           "................");

ART(MIC, "................"
         "......tttt......"
         ".....thhhst....."
         ".....thhhst....."
         ".....thhhst....."
         ".....thhhst....."
         "...t.thhhst.t..."
         "...t.thhhst.t..."
         "...t.tsssst.t..."
         "....t.tttt.t...."
         ".....tt..tt....."
         ".......tt......."
         ".......tt......."
         ".......tt......."
         ".....tttttt....."
         "................");

/* through a muted mic */
ART(SLASH, "................"
           ".tt............."
           "..tt............"
           "...tt..........."
           "....tt.........."
           ".....tt........."
           "......tt........"
           ".......tt......."
           "........tt......"
           ".........tt....."
           "..........tt...."
           "...........tt..."
           "............tt.."
           ".............tt."
           "..............tt"
           "................");

ART(BATTERY, "................"
             "................"
             "................"
             "tttttttttttttt.."
             "tggggggggggggt.."
             "tg**********gt.."
             "tg**********gttt"
             "tg**********gttt"
             "tg**********gttt"
             "tg**********gttt"
             "tg**********gt.."
             "tggggggggggggt.."
             "tttttttttttttt.."
             "................"
             "................"
             "................");

ART(BOLT, "................"
          ".......tttttt..."
          "......taaaat...."
          ".....taaaat....."
          "....taaaat......"
          "...taaaatttt...."
          "..taaaaaaaat...."
          "..tttttaaaat...."
          "......taaat....."
          ".....taaat......"
          "....taat........"
          "....tat........."
          "...tt..........."
          "...t............"
          "................"
          "................");

/* the rune on its oval; off: the rune alone */
ART(BLUETOOTH, "................"
               ".....aaaaa......"
               "...aaaaAaaaa...."
               "..aaaaaAAaaaa..."
               "..aaAaaAaAaaa..."
               "..aaaAaAaaAaa..."
               "..aaaaAAaAaaa..."
               "..aaaaaAAaaaa..."
               "..aaaaaAAaaaa..."
               "..aaaaAAaAaaa..."
               "..aaaAaAaaAaa..."
               "..aaAaaAaAaaa..."
               "..aaaaaAAaaaa..."
               "...aaaaAaaaa...."
               ".....aaaaa......"
               "................");

ART(MOUSE, "................"
           ".....ttttt......"
           "....thhthht....."
           "...thhhthhht...."
           "...thhhthhst...."
           "...thhhthhst...."
           "...ttttttttt...."
           "...thhhhhhst...."
           "...thhhhhhst...."
           "...thhhhhhst...."
           "...thhhhhhst...."
           "...thhhhhhst...."
           "...thhhhhsst...."
           "....tssssst....."
           ".....ttttt......"
           "................");

ART(KEYBOARD, "................"
              "................"
              "................"
              "................"
              "tttttttttttttttt"
              "thhhhhhhhhhhhhht"
              "thshshshshshshht"
              "thhhhhhhhhhhhhht"
              "thhshshshshshsht"
              "thhhhhhhhhhhhhht"
              "thhhsssssssshhht"
              "thhhhhhhhhhhhhht"
              "tttttttttttttttt"
              "................"
              "................"
              "................");

ART(HEADPHONES, "................"
                ".....tttttt....."
                "...tt......tt..."
                "..t..........t.."
                ".t............t."
                ".t............t."
                "t..............t"
                "t..............t"
                "tttt........tttt"
                "thht........thht"
                "thht........thht"
                "thst........thst"
                "thst........thst"
                "tttt........tttt"
                "................"
                "................");

ART(PHONE, "....tttttttt...."
           "....thhhhhht...."
           "....taaaaaat...."
           "....taaaaaat...."
           "....taaaaaat...."
           "....taaaaaat...."
           "....taaaaaat...."
           "....taaaaaat...."
           "....taaaaaat...."
           "....taaaaaat...."
           "....taaaaaat...."
           "....taaaaaat...."
           "....thhhhhht...."
           "....thhtthht...."
           "....thhhhhht...."
           "....tttttttt....");

ART(GAMEPAD, "................"
             "................"
             "................"
             "................"
             "...tttttttttt..."
             "..thhhhhhhhhht.."
             ".thhthhhhhhahht."
             ".thttthhhhahaht."
             ".thhthhhhhhahht."
             ".thhhhhhhhhhhst."
             ".thhhht..thhhst."
             ".thhhst..thhsst."
             "..tttt....tttt.."
             "................"
             "................"
             "................");

ART(COMPUTER, "................"
              "................"
              "..tttttttttttt.."
              "..taaaaaaaaaat.."
              "..taaaaaaaaaat.."
              "..taaaaaaaaaat.."
              "..taaaaaaaaaat.."
              "..taaaaaaaaaat.."
              "..taaaaaaaaaat.."
              "..taaaaaaaaaat.."
              "..tttttttttttt.."
              ".thhhhhhhhhhhht."
              "thhhhhhhhhhhhhst"
              "tttttttttttttttt"
              "................"
              "................");

ART(PLUG, "................"
          ".....t....t....."
          ".....t....t....."
          "...tttttttttt..."
          "...thhhhhhhst..."
          "...thhhhhhhst..."
          "...thhhhhhhst..."
          "...thhhhhhhst..."
          "...tsssssssst..."
          "....tttttttt...."
          "......tttt......"
          ".......tt......."
          ".......tt......."
          "........tt......"
          ".........tt....."
          "................");

ART(PEN, "................"
         "............tst."
         "...........tst.."
         "..........tat..."
         ".........tat...."
         "........tat....."
         ".......tat......"
         "......tat......."
         ".....tat........"
         "....tat........."
         "...tht.........."
         "..tht..........."
         ".tt............."
         ".t.............."
         "................"
         "................");

ART(TOUCHPAD, "................"
              "................"
              "................"
              ".tttttttttttttt."
              ".thhhhhhhhhhhst."
              ".thhhhhhhhhhhst."
              ".thhhhhhhhhhhst."
              ".thhhhhhhhhhhst."
              ".thhhhhhhhhhhst."
              ".thhhhhhhhhhhst."
              ".tttttttttttttt."
              ".thhhhhtthhhhst."
              ".tttttttttttttt."
              "................"
              "................"
              "................");

ART(CUP, "................"
         ".....t...t......"
         "....t...t......."
         ".....t...t......"
         "....t...t......."
         "................"
         "..tttttttttt...."
         "..thhhhhhhhtttt."
         "..thhhhhhhst..t."
         "..thhhhhhhst..t."
         "..thhhhhhhsttt.."
         "...thhhhhst....."
         "....tttttt......"
         ".tttttttttttt..."
         "................"
         "................");

ART(WIRED, "................"
           "......tttt......"
           "......taat......"
           "......taat......"
           "......tttt......"
           ".......t........"
           ".......t........"
           "..tttttttttttt.."
           "..t..........t.."
           "ttttt......ttttt"
           "taaat......taaat"
           "taaat......taaat"
           "ttttt......ttttt"
           "................"
           "................"
           "................");

ART(TRANSFER, "................"
              "................"
              ".....t....t....."
              "....ttt...t....."
              "...t.t.t..t....."
              "..t..t..t.t....."
              ".....t....t....."
              ".....t....t....."
              ".....t....t....."
              ".....t....t....."
              ".....t.t..t..t.."
              ".....t..t.t.t..."
              ".....t...ttt...."
              ".....t....t....."
              "................"
              "................");

ART(SUN, "................"
         ".......t........"
         "...t...t...t...."
         "....t.....t....."
         "......ttt......."
         ".....thhht......"
         "....thhhhht....."
         "tt..thhhhst..tt."
         "....thhhsst....."
         ".....tssst......"
         "......ttt......."
         "....t.....t....."
         "...t...t...t...."
         ".......t........"
         "................"
         "................");

/* a bulb: the glass is the light's colour */
ART(LAMP, "................"
          "......tttt......"
          "....tthaaatt...."
          "...thaaaaaaat..."
          "...taaaaaaaat..."
          "..thaaaaaaaaat.."
          "..taaaaaaaaaat.."
          "..taaaaaaaaaat.."
          "...taaaaaaaat..."
          "....taaaaaat...."
          ".....taaaat....."
          ".....tttttt....."
          ".....tsssst....."
          ".....tttttt....."
          "......tsst......"
          ".......tt.......");

/* a floppy: accent body, the shutter, the label */
ART(DISK, "................"
          ".ttttttttttttt.."
          ".taashhhhsaaaat."
          ".taashhtssaaaat."
          ".taashhtssaaaat."
          ".taasssssaaaaat."
          ".taaaaaaaaaaaat."
          ".taahhhhhhhhaat."
          ".taahhhhhhhhaat."
          ".taahssssssthaat"
          ".taahhhhhhhhaat."
          ".taahssssssthat."
          ".taahhhhhhhhaat."
          ".tttttttttttttt."
          "................"
          "................");

ART(DISC, "................"
          ".....ssssss....."
          "...sslllllhss..."
          "..slllllllhhhs.."
          ".sllllllllhhhhs."
          ".slllllsshhhhhs."
          "sllllls..shhhhhd"
          "slllls....shhhhd"
          "slllls....shhhhd"
          "shhhhhs..slllhld"
          ".shhhhhsslllllt."
          ".shhhhhllllllld."
          "..shhhhlllllld.."
          "...ddhhlllldd..."
          ".....dddddd....."
          "................");

ART(LOCK, "................"
          "......tttt......"
          ".....tsssst....."
          "....ts....st...."
          "....ts....st...."
          "....ts....st...."
          "..tttttttttttt.."
          "..thhhhhhhhhst.."
          "..thhhhhhhhhst.."
          "..thhhhtthhhst.."
          "..thhhhtthhhst.."
          "..thhhhhthhhst.."
          "..thhhhhhhhhst.."
          "..tssssssssssst."
          "..tttttttttttt.."
          "................");

ART(NOTE, "................"
          "................"
          ".......tt......."
          ".......ttt......"
          ".......tttt....."
          ".......t.ttt...."
          ".......t..tt...."
          ".......t...t...."
          ".......t...t...."
          ".......t........"
          "....tttt........"
          "...ttttt........"
          "...ttttt........"
          "....ttt........."
          "................"
          "................");

ART(DROP, "................"
          ".......a........"
          ".......a........"
          "......aaa......."
          "......aaa......."
          ".....aaaaa......"
          ".....aaaaa......"
          "....aaaaaaa....."
          "....ahaaaaa....."
          "...aahaaaaaa...."
          "...aahaaaaaa...."
          "...aaahaaaaa...."
          "....aaaaaaa....."
          ".....aaaaa......"
          "................"
          "................");

ART(LOGO, "................"
          ".tttttttttttttt."
          ".taaaaaaaaaaaat."
          ".tAAAAAaaaaAaAt."
          ".tttttttttttttt."
          ".tddddddddddddt."
          ".tdhddddddddddt."
          ".tddhdddddddddt."
          ".tdddhddddddddt."
          ".tddhdddddddddt."
          ".tdhddhhhhddddt."
          ".tddddddddddddt."
          ".tddddddddddddt."
          ".tttttttttttttt."
          "................"
          "................");

/* ctui-web's toolbar (a 90s browser's): back, forward, reload, home, stop */
ART(BACK, ".......t........"
          "......tt........"
          ".....tht........"
          "....that........"
          "...thaat........"
          "..thaaahtttttttt"
          ".thaaaaahhhhhhht"
          "thaaaaaaaaaaaaat"
          "tsaaaaaaaaaaaaat"
          ".tsaaaaassssssst"
          "..tsaaastttttttt"
          "...tsaat........"
          "....tsat........"
          ".....tst........"
          "......tt........"
          ".......t........");

ART(FORWARD, "........t......."
             "........tt......"
             "........tht....."
             "........taht...."
             "........taaht..."
             "tttttttthaaaht.."
             "thhhhhhhaaaaaht."
             "taaaaaaaaaaaaaht"
             "taaaaaaaaaaaaast"
             "tsssssssaaaaast."
             "ttttttttsaaast.."
             "........taast..."
             "........tast...."
             "........tst....."
             "........tt......"
             "........t.......");

ART(RELOAD, "................"
            ".....tt........."
            "....taat........"
            "...taaat....tt.."
            "..taaatt....tt.."
            ".taaat.....taat."
            ".taat.....taaaat"
            ".taat....ttaaaat"
            ".taat......taat."
            ".taat......taat."
            ".taaat....taaat."
            "..taaattttaaat.."
            "...taaaaaaaat..."
            "....taaaaaat...."
            ".....tttttt....."
            "................");

ART(HOME, "................"
          ".......tt......."
          "......thht......"
          ".....thaaht....."
          "....thaaaaht...."
          "...thaaaaaaht..."
          "..thaaaaaaaaht.."
          ".thaaaaaaaaaaht."
          "ttsaaaaaaaaaastt"
          "..thhhhhssshht.."
          "..thaahhssshht.."
          "..thaahhssshht.."
          "..thhhhhssshht.."
          "..thhhhhssshht.."
          "..tttttttttttt.."
          "................");

/* its accent the button's tint (red) */
ART(STOP, ".....tttttt....."
          "....taaaaaat...."
          "...taaaaaaaat..."
          "..taaaaaaaaaat.."
          ".taaaaaaaaaaaat."
          "taaaaaaaaaaaaaat"
          "taaahhhhhhhhhaat"
          "taaahhhhhhhhhaat"
          "taaahhhhhhhhhaat"
          "taaaaaaaaaaaaaat"
          "taaaaaaaaaaaaaat"
          ".taaaaaaaaaaaat."
          "..taaaaaaaaaat.."
          "...taaaaaaaat..."
          "....taaaaaat...."
          ".....tttttt.....");

/* a page's security: whole for https */
ART(KEY, "................"
         "................"
         "................"
         "................"
         "..tttt.........."
         ".twwwwt........."
         "twwttwwtttttttt."
         "twt..twwwwwwwwwt"
         "twt..twwwwwwwwwt"
         "twwttwwtttwwttwt"
         ".twwwwt..twwtttt"
         "..tttt...tttt..."
         "................"
         "................"
         "................"
         "................");

/* and broken for http (Netscape's) */
ART(KEY_BROKEN, "................"
                "................"
                "................"
                "................"
                "..tttt.........."
                ".twwwwt........."
                "twwttwwt........"
                "twt..twt........"
                "twt..twt.tttttt."
                "twwttwwt.twwwwwt"
                ".twwwwt..twwwwwt"
                "..tttt...twwttwt"
                ".........twwtttt"
                ".........tttt..."
                "................"
                "................");

/* a message box's icons (95's): information, its i in the accent (tinted blue),
 * a warning, an error (its disc tinted red) */
ART(INFO, "................"
          ".....tttttt....."
          "...tthhaahhtt..."
          "..thhhhaahhhht.."
          ".thhhhhhhhhhhht."
          ".thhhhhhhhhhhht."
          ".thhhhhaahhhhht."
          "thhhhhhaahhhhhht"
          ".thhhhhaahhhhht."
          ".thhhhhaahhhhht."
          "..thhhhaahhhht.."
          "...thhhaahhht..."
          "....tttttttt...."
          "....t..........."
          "................"
          "................");

ART(WARNING, ".......tt......."
             ".......tt......."
             "......twwt......"
             "......twwt......"
             ".....twwwwt....."
             "....twwttwwt...."
             "....twwttwwt...."
             "...twwwttwwwt..."
             "...twwwttwwwt..."
             "..twwwwttwwwwt.."
             "..twwwwttwwwwt.."
             ".twwwwwwwwwwwwt."
             "twwwwwwttwwwwwwt"
             "twwwwwwttwwwwwwt"
             "twwwwwwwwwwwwwwt"
             "tttttttttttttttt");

ART(ERROR, ".......tt......."
           "....tttaattt...."
           "...taaaaaaaat..."
           "..taaaaaaaaaat.."
           ".taahhaaaaahhat."
           ".taaahhaaahhaat."
           ".taaaahhahhaaat."
           "taaaaaahhhaaaaat"
           "taaaaaahhhaaaaat"
           ".taaaahhahhaaat."
           ".taaahhaaahhaat."
           ".taahhaaaaahhat."
           "..taaaaaaaaaat.."
           "...taaaaaaaat..."
           "....tttaattt...."
           ".......tt.......");

/* --- the sky's parts (CTUI_LOOK_PICTO_SKY), layered by compose_sky() --- */

ART(SKY_SUN, "................"
             ".......ww......."
             "..w....ww....w.."
             "...w........w..."
             "......tttt......"
             ".....twwwwt....."
             "....twwwwwwt...."
             "ww..twwwwwwt..ww"
             "ww..twwwwwwt..ww"
             "....twwwwwwt...."
             ".....twwwwt....."
             "......tttt......"
             "...w........w..."
             "..w....ww....w.."
             ".......ww......."
             "................");

ART(SKY_MOON, "................"
              ".....tttt......."
              "...ttwwt........"
              "..twwwt........."
              "..twwt.........."
              ".twwwt.........."
              ".twwt..........."
              ".twwt..........."
              ".twwt..........."
              ".twwwt.........."
              "..twwwt.....tt.."
              "...twwwttttwwt.."
              "....ttwwwwwtt..."
              "......ttttt....."
              "................"
              "................");

ART(SKY_PEEK_SUN, "....w..........."
                  ".w.....w........"
                  "....ttt........."
                  "...twwwt........"
                  "w.twwwwwt.w....."
                  "..twwwwwt......."
                  "..twwwwwt......."
                  "...twwwt........"
                  ".w..ttt..w......"
                  "................"
                  "................"
                  "................"
                  "................"
                  "................"
                  "................"
                  "................");

ART(SKY_PEEK_MOON, "................"
                   "...tttt........."
                   "..twwt.........."
                   ".twwt..........."
                   ".twt............"
                   ".twt............"
                   ".twwt....t......"
                   "..twwttttt......"
                   "...tttt........."
                   "................"
                   "................"
                   "................"
                   "................"
                   "................"
                   "................"
                   "................");

/* rows 4-12; up 3 when something falls */
ART(SKY_CLOUD, "................"
               "................"
               "................"
               "................"
               ".......tttt....."
               "......thhhht...."
               "..ttt.thhhhht..."
               ".thhhthhhhhhttt."
               ".thhhhhhhhhhhhht"
               "thhhhhhhhhhhhhht"
               "thhhhhhhhhhhhhht"
               "tsssssssssssssst"
               ".tttttttttttttt."
               "................"
               "................"
               "................");

ART(SKY_CLOUDLET, "................"
                  "................"
                  "................"
                  "................"
                  "................"
                  "................"
                  "................"
                  "................"
                  "................"
                  "...........ttt.."
                  "..........thhht."
                  ".......tt.thhhht"
                  "......thhthhhhht"
                  "......thhhhhhhht"
                  "......tsssssssst"
                  ".......tttttttt.");

/* under the high cloud (its last row 9) */
ART(SKY_DRIZZLE, "................"
                 "................"
                 "................"
                 "................"
                 "................"
                 "................"
                 "................"
                 "................"
                 "................"
                 "................"
                 "................"
                 ".......a........"
                 "......a........."
                 "..............a."
                 ".a...........a.."
                 "a...............");

ART(SKY_RAIN, "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              "....a...a...a..."
              "...a...a...a...."
              "..a...a...a....."
              "................"
              "................");

ART(SKY_HEAVY, "................"
               "................"
               "................"
               "................"
               "................"
               "................"
               "................"
               "................"
               "................"
               "................"
               "................"
               "..............a."
               ".............a.."
               "............a..."
               ".a...a...a......"
               "a...a...a.......");

ART(SKY_SNOW, "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              "...h.......h...."
              "..hhh.....hhh..."
              "...h...h...h...."
              "......hhh......."
              ".......h........");

ART(SKY_BOLT, "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              "................"
              ".......ww......."
              "......ww........"
              ".....wwwww......"
              ".......ww......."
              "......ww........"
              ".....w..........");

ART(SKY_FOG, "................"
             "................"
             "................"
             "..tttttttttt...."
             "................"
             "....tttttttttttt"
             "................"
             "tttttttttttt...."
             "................"
             "...ttttttttttt.."
             "................"
             ".ttttttttttt...."
             "................"
             "................"
             "................"
             "................");

/* the parts of sky layered into px (N * N + 1) */
static void compose_sky(unsigned sky, char *px) {
  static const struct {
    unsigned bit, need; /* need: another bit too (0: none) */
    const char *art;
  } layers[] = {
      {CTUI_LOOK_SKY_SUN, 0, SKY_SUN},
      {CTUI_LOOK_SKY_MOON, 0, SKY_MOON},
      {CTUI_LOOK_SKY_FOG, 0, SKY_FOG},
      {CTUI_LOOK_SKY_CLOUD, 0, SKY_CLOUD},
      {CTUI_LOOK_SKY_CLOUDLET, 0, SKY_CLOUDLET},
      {CTUI_LOOK_SKY_DRIZZLE, 0, SKY_DRIZZLE},
      {CTUI_LOOK_SKY_RAIN, 0, SKY_RAIN},
      {CTUI_LOOK_SKY_HEAVY, CTUI_LOOK_SKY_RAIN, SKY_HEAVY},
      {CTUI_LOOK_SKY_SNOW, 0, SKY_SNOW},
      {CTUI_LOOK_SKY_BOLT, 0, SKY_BOLT},
  };
  memset(px, '.', N * N);
  px[N * N] = '\0';
  unsigned falls = CTUI_LOOK_SKY_DRIZZLE | CTUI_LOOK_SKY_RAIN |
                   CTUI_LOOK_SKY_SNOW | CTUI_LOOK_SKY_BOLT;
  if (sky & CTUI_LOOK_SKY_PEEK) {
    /* the small sun / moon replaces the big one, behind the cloud */
    const char *art = (sky & CTUI_LOOK_SKY_MOON) ? SKY_PEEK_MOON : SKY_PEEK_SUN;
    memcpy(px, art, N * N);
    sky &= ~(unsigned)(CTUI_LOOK_SKY_SUN | CTUI_LOOK_SKY_MOON);
  }
  for (size_t i = 0; i < sizeof layers / sizeof *layers; i++) {
    if (!(sky & layers[i].bit) || (layers[i].need && !(sky & layers[i].need))) {
      continue;
    }
    int up = layers[i].bit == CTUI_LOOK_SKY_CLOUD && (sky & falls) ? 3 : 0;
    for (int j = 0; j < N * N; j++) {
      char ch = layers[i].art[j];
      int to = j - up * N;
      if (ch != '.' && to >= 0) {
        px[to] = ch;
      }
    }
  }
}

/* a page's picture that isn't there (Netscape's broken image): a sheet
 * torn in two, a sun and a hill on it */
ART(IMAGE, "................"
           ".tttttttttt....."
           ".thhhhhhhhtt...."
           ".thwwhhhhhtht..."
           ".twwwwhhhhtttt.."
           ".thwwhhhhhhhhht."
           ".thhhhhhhhhhhht."
           ".thhhhhhat.tttt."
           ".thhhhhaat..thht"
           ".thhhhaaat.thhht"
           ".thhhaaat..thaat"
           ".thhaaaat.thaaat"
           ".thaaaaat.taaaat"
           ".tttttttt.tttttt"
           "................"
           "................");

ART(ENVELOPE, "................"
              "................"
              "................"
              "tttttttttttttttt"
              "tthhhhhhhhhhhhtt"
              "ththhhhhhhhhhtht"
              "thhthhhhhhhhthht"
              "thhhthhhhhhthhht"
              "thhhhthhhhthhhht"
              "thhhhhtttthhhhht"
              "thhhhhhhhhhhhhst"
              "thhhhhhhhhhhhhst"
              "tsssssssssssssst"
              "tttttttttttttttt"
              "................"
              "................");

ART(STAR, "................"
          ".......tt......."
          "......twwt......"
          "......thwt......"
          ".....twwwwt....."
          "tttttwwwwwwttttt"
          ".twwwwwwwwwwwwt."
          "..twwwwwwwwwwt.."
          "...twwwwwwwwt..."
          "....twwwwwwt...."
          "....twwwwwwt...."
          "...twwwttwwwt..."
          "...twwt..twwt..."
          "..twt......twt.."
          "..tt........tt.."
          "................");

ART(REPLY, "................"
           "................"
           ".....t.........."
           "....tt.........."
           "...tat.........."
           "..taattttttt...."
           ".taaaaaaaaaatt.."
           "taaaaaaaaaaaaat."
           ".taaaaaaaaaaaaat"
           "..taatttttttaaat"
           "...tat......taat"
           "....tt......taat"
           ".....t.....taat."
           "..........taat.."
           ".........ttt...."
           "................");

ART(FORWARDED, "................"
               "................"
               "..........t....."
               "..........tt...."
               "..........tat..."
               "....tttttttaat.."
               "..ttaaaaaaaaaat."
               ".taaaaaaaaaaaaat"
               "taaaaaaaaaaaaat."
               "taaatttttttaat.."
               "taat......tat..."
               "taat......tt...."
               ".taat.....t....."
               "..taat.........."
               "....ttt........."
               "................");

ART(FOLDER, "................"
            "................"
            "................"
            ".tttttt........."
            "thhhhhht........"
            "thwwwwwwttttttt."
            "thwwwwwwwwwwwwst"
            "thwwwwwwwwwwwwst"
            "thwwwwwwwwwwwwst"
            "thwwwwwwwwwwwwst"
            "thwwwwwwwwwwwwst"
            "thwwwwwwwwwwwwst"
            "tsssssssssssssst"
            "tttttttttttttttt"
            "................"
            "................");

/* the same open: its front flap down */
ART(FOLDER_OPEN, "................"
                 "................"
                 "................"
                 ".tttttt........."
                 "thhhhhht........"
                 "thsssssstttttt.."
                 "thsssssssssssst."
                 "thsttttttttttttt"
                 "thtwwwwwwwwwwwt."
                 "thtwwwwwwwwwwt.."
                 "ttwwwwwwwwwwwt.."
                 "twwwwwwwwwwwt..."
                 "twwwwwwwwwwst..."
                 "tttttttttttt...."
                 "................"
                 "................");

ART(INBOX, "......tttt......"
           "......taat......"
           "......taat......"
           "...ttttaatttt..."
           "....taaaaaat...."
           ".....taaaat....."
           "......taat......"
           ".......tt......."
           "t......tt......t"
           "th............st"
           "thhhhhggggghhhst"
           "thhhhhhhhhhhhhst"
           "thhhhhhhhhhhhhst"
           "tsssssssssssssst"
           "tttttttttttttttt"
           "................");

ART(SENT, ".......tt......."
          "......taat......"
          ".....taaaat....."
          "....taaaaaat...."
          "...ttttaatttt..."
          "......taat......"
          "......taat......"
          "......taat......"
          "t.....taat.....t"
          "th............st"
          "thhhhhggggghhhst"
          "thhhhhhhhhhhhhst"
          "thhhhhhhhhhhhhst"
          "tsssssssssssssst"
          "tttttttttttttttt"
          "................");

ART(DRAFTS, "..ttttttttttt..."
            "..thhhhhhhhst..."
            "..thttttthhst..."
            "..thhhhhhhhst..."
            "..thtttttthst..."
            "..thhhhhhhhst..."
            "..thttttthhst.tt"
            "..thhhhhhhhsttat"
            "..thtttttttat..."
            "..thhhhhhtatt..."
            "..thhhhhtatst..."
            "..thhhhtathst..."
            "..thhhtthhhst..."
            "..tsssssssssst.."
            "..ttttttttttt..."
            "................");

ART(ARCHIVE, "................"
             "................"
             ".tttttttttttttt."
             ".thhhhhhhhhhhst."
             ".tsssssssssssst."
             "..thhhhhhhhhst.."
             "..thhhhhhhhhst.."
             "..thhhggggghst.."
             "..thhhhhhhhhst.."
             "..thhhhhhhhhst.."
             "..thhhhhhhhhst.."
             "..thhhhhhhhhst.."
             "..tssssssssssst."
             "..ttttttttttttt."
             "................"
             "................");

ART(JUNK, "................"
          "................"
          "................"
          "tttttttttttttttt"
          "tthhhhhhhhhhhhtt"
          "ththhhhhhhhhhtht"
          "thhthhhhhhhhthht"
          "thhhthhhhhhthhht"
          "thhhhthhhaahhhaa"
          "thhhhhttttaahaat"
          "thhhhhhhhhhaaast"
          "thhhhhhhhhhaaast"
          "tsssssssssaasaat"
          "tttttttttaatttaa"
          "................"
          "................");

ART(TRASH, "................"
           "......tttt......"
           ".tttttttttttttt."
           ".thhhhhhhhhhhst."
           ".tttttttttttttt."
           "..thhthhthhtst.."
           "..thhthhthhtst.."
           "..thhthhthhtst.."
           "..thhthhthhtst.."
           "..thhthhthhtst.."
           "..thhthhthhtst.."
           "..thhthhthhtst.."
           "..thhthhthhtst.."
           "..tssssssssssst."
           "...tttttttttt..."
           "................");

/* '+' is filled unless it is a ring (0 of 1) */
ART(DOT, "................"
         "................"
         ".....tttttt....."
         "...ttaaaaaatt..."
         "..taaaaaaaaaat.."
         "..taa++++++aat.."
         ".taa++++++++aat."
         ".taa++++++++aat."
         ".taa++++++++aat."
         ".taa++++++++aat."
         ".taa++++++++aat."
         "..taa++++++aat.."
         "..taaaaaaaaaat.."
         "...ttaaaaaatt..."
         ".....tttttt....."
         "................");

/* '|' is the plus's upright, gone when open */
ART(EXPANDER, "................"
              "................"
              "................"
              "...ttttttttt...."
              "...thhhhhhht...."
              "...thhh|hhht...."
              "...thhh|hhht...."
              "...thtttttht...."
              "...thhh|hhht...."
              "...thhh|hhht...."
              "...thhhhhhht...."
              "...ttttttttt...."
              "................"
              "................"
              "................"
              "................");

static const char *const ARTS[CTUI_LOOK_PICTOS] = {
    [CTUI_LOOK_PICTO_SPEAKER] = SPEAKER,
    [CTUI_LOOK_PICTO_MIC] = MIC,
    [CTUI_LOOK_PICTO_BATTERY] = BATTERY,
    [CTUI_LOOK_PICTO_BOLT] = BOLT,
    [CTUI_LOOK_PICTO_BLUETOOTH] = BLUETOOTH,
    [CTUI_LOOK_PICTO_MOUSE] = MOUSE,
    [CTUI_LOOK_PICTO_KEYBOARD] = KEYBOARD,
    [CTUI_LOOK_PICTO_HEADPHONES] = HEADPHONES,
    [CTUI_LOOK_PICTO_PHONE] = PHONE,
    [CTUI_LOOK_PICTO_GAMEPAD] = GAMEPAD,
    [CTUI_LOOK_PICTO_COMPUTER] = COMPUTER,
    [CTUI_LOOK_PICTO_PLUG] = PLUG,
    [CTUI_LOOK_PICTO_PEN] = PEN,
    [CTUI_LOOK_PICTO_TOUCHPAD] = TOUCHPAD,
    [CTUI_LOOK_PICTO_CUP] = CUP,
    [CTUI_LOOK_PICTO_WIRED] = WIRED,
    [CTUI_LOOK_PICTO_TRANSFER] = TRANSFER,
    [CTUI_LOOK_PICTO_SUN] = SUN,
    [CTUI_LOOK_PICTO_LAMP] = LAMP,
    [CTUI_LOOK_PICTO_DISK] = DISK,
    [CTUI_LOOK_PICTO_DISC] = DISC,
    [CTUI_LOOK_PICTO_LOCK] = LOCK,
    [CTUI_LOOK_PICTO_NOTE] = NOTE,
    [CTUI_LOOK_PICTO_DROP] = DROP,
    [CTUI_LOOK_PICTO_LOGO] = LOGO,
    [CTUI_LOOK_PICTO_BACK] = BACK,
    [CTUI_LOOK_PICTO_FORWARD] = FORWARD,
    [CTUI_LOOK_PICTO_RELOAD] = RELOAD,
    [CTUI_LOOK_PICTO_HOME] = HOME,
    [CTUI_LOOK_PICTO_STOP] = STOP,
    [CTUI_LOOK_PICTO_KEY] = KEY,
    [CTUI_LOOK_PICTO_INFO] = INFO,
    [CTUI_LOOK_PICTO_WARNING] = WARNING,
    [CTUI_LOOK_PICTO_ERROR] = ERROR,
    [CTUI_LOOK_PICTO_IMAGE] = IMAGE,
    [CTUI_LOOK_PICTO_ENVELOPE] = ENVELOPE,
    [CTUI_LOOK_PICTO_STAR] = STAR,
    [CTUI_LOOK_PICTO_REPLY] = REPLY,
    [CTUI_LOOK_PICTO_FORWARDED] = FORWARDED,
    [CTUI_LOOK_PICTO_FOLDER] = FOLDER,
    [CTUI_LOOK_PICTO_INBOX] = INBOX,
    [CTUI_LOOK_PICTO_SENT] = SENT,
    [CTUI_LOOK_PICTO_DRAFTS] = DRAFTS,
    [CTUI_LOOK_PICTO_ARCHIVE] = ARCHIVE,
    [CTUI_LOOK_PICTO_JUNK] = JUNK,
    [CTUI_LOOK_PICTO_TRASH] = TRASH,
    [CTUI_LOOK_PICTO_DOT] = DOT,
    [CTUI_LOOK_PICTO_EXPANDER] = EXPANDER,
};

/* a busy logo's lines of output: as long as these eighths of its screen,
 * one more printed each frame */
static const unsigned char LINE_LEN[] = {6, 3, 7, 2, 5, 8, 4, 6};
#define LINE_KINDS (int)sizeof LINE_LEN

/* the logo's screen in px (n x n, its 'd' box) as frame `frame` of it
 * busy: lines of text in highlight scrolling up it, a cursor blinking at
 * the end of the last */
static void logo_busy(char *px, int n, int frame) {
  int x0 = n, y0 = n, x1 = -1, y1 = -1;
  for (int i = 0; i < n * n; i++) {
    if (px[i] == 'd') {
      x0 = i % n < x0 ? i % n : x0;
      x1 = i % n > x1 ? i % n : x1;
      y0 = i / n < y0 ? i / n : y0;
      y1 = i / n > y1 ? i / n : y1;
    }
  }
  if (x1 < x0 + 3 || y1 < y0 + 2) {
    return;
  }
  for (int y = y0; y <= y1; y++) {
    memset(px + y * n + x0, 'd', (size_t)(x1 - x0 + 1));
  }
  int pitch = n >= CTUI_LOOK_PICTO_GRID_BIG ? 3 : 2, room = x1 - x0 - 1;
  int last = y0 + 1 + (y1 - 1 - (y0 + 1)) / pitch * pitch;
  for (int y = y0 + 1, j = 0; y < y1; y += pitch, j++) {
    int len = LINE_LEN[(j + frame) % LINE_KINDS] * room / 8;
    if (y == last) {
      len = len > room - 2 ? room - 2 : len;
      if (frame & 1) {
        memset(px + y * n + x0 + 1 + len + 1, 'h', 1); /* the cursor */
      }
    }
    memset(px + y * n + x0 + 1, 'h', (size_t)len);
  }
}

/* the battery's charge, in steps (the 16 px one's inside columns) */
#define CHARGE_COLS 10
#define WAVES 3

/* the sky's state is its parts, not a share of max */
static void snap_sky(int *value, int *max) {
  *value = *value > 0 ? *value & CTUI_LOOK_SKY_ALL : 0;
  *max = 1;
}

void ctui_look_picto_snap(CTUI_LOOK_PICTO p, int off, int *value, int *max) {
  if (p == CTUI_LOOK_PICTO_SKY) {
    snap_sky(value, max);
    return;
  }
  int v = *value, m = *max;
  *value = *max = 0;
  if (m <= 0) {
    return;
  }
  v = v < 0 ? 0 : v > m ? m : v;
  if (p == CTUI_LOOK_PICTO_LOGO && v > 0) {
    /* a busy frame: which of the lines show, the cursor's blink */
    *value = (v - 1) % (LINE_KINDS * 2) + 1;
    *max = LINE_KINDS * 2;
  } else if (p == CTUI_LOOK_PICTO_SPEAKER && !off) {
    /* any level shows a wave; each third another */
    *value = (int)(((long long)v * WAVES + m - 1) / m);
    *max = WAVES;
  } else if (p == CTUI_LOOK_PICTO_FOLDER || p == CTUI_LOOK_PICTO_DOT ||
             p == CTUI_LOOK_PICTO_EXPANDER) {
    *value = v > 0;
    *max = 1;
  } else if (p == CTUI_LOOK_PICTO_BATTERY) {
    *value = (int)(((long long)v * CHARGE_COLS + m / 2) / m);
    *max = CHARGE_COLS;
  }
}

/* the grid p is drawn on in a w x h box: the big art where there is one
 * and it comes out larger than the small one's whole multiple */
static int grid_for(CTUI_LOOK_PICTO p, int w, int h) {
  int side = w < h ? w : h;
  int small = side / N * N, big = side / BIG * BIG;
  return p != CTUI_LOOK_PICTO_SKY && ctui_look_picto_big[p] && big >= small &&
                 big > 0
             ? BIG
             : N;
}

static void paint(CTUI_LOOK_CANVAS *c, int x, int y, int w, int h,
                  const char *px, int n, int off, const CTUI_LOOK *l) {
  CTUI_LOOK_MASK m = {n, n, px};
  ctui_look_paint_mask(c, x, y, w, h, &m, l, off);
}

void ctui_look_picto_paint(CTUI_LOOK_CANVAS *c, int x, int y, int w, int h,
                           CTUI_LOOK_PICTO p, int value, int max, int off,
                           const CTUI_LOOK *l) {
  if (p <= CTUI_LOOK_PICTO_NONE || p >= CTUI_LOOK_PICTOS) {
    return;
  }
  ctui_look_picto_snap(p, off, &value, &max); /* the same for snapped ones */
  /* the template made concrete for this state */
  char px[BIG * BIG + 1];
  if (p == CTUI_LOOK_PICTO_SKY) {
    compose_sky((unsigned)value, px);
    paint(c, x, y, w, h, px, N, off, l);
    return;
  }
  int n = grid_for(p, w, h), big = n == BIG;
  const char *art = big ? ctui_look_picto_big[p]
                   : p == CTUI_LOOK_PICTO_FOLDER && value > 0 ? FOLDER_OPEN
                                                              : ARTS[p];
  if (p == CTUI_LOOK_PICTO_KEY && off) {
    /* broken, in colour: what it says isn't "disabled" */
    paint(c, x, y, w, h, big ? ctui_look_picto_big_key_broken : KEY_BROKEN, n,
          0, l);
    return;
  }
  /* the battery's inside: where its row of '*' starts, how wide it is */
  const char *in = strchr(art, '*');
  int in_x = in ? (int)(in - art) % n : 0;
  int in_w = in ? (int)strspn(in, "*") : 0;
  for (int i = 0; i < n * n; i++) {
    char ch = art[i];
    if (ch >= '1' && ch <= '0' + WAVES) {
      ch = max > 0 && ch - '0' <= value ? 't' : '.';
    } else if (ch == '*') {
      int lit = max > 0 ? value * in_w / CHARGE_COLS : 0;
      ch = i % n - in_x < lit ? 'a' : 'g';
    } else if (ch == '+') {
      ch = max > 0 && !value ? '.' : 'a';
    } else if (ch == '|') {
      ch = max > 0 && value ? 'h' : 't';
    } else if (ch == 'a' && off &&
               (p == CTUI_LOOK_PICTO_BLUETOOTH || p == CTUI_LOOK_PICTO_LAMP)) {
      ch = '.';
    }
    px[i] = ch;
  }
  px[n * n] = '\0';
  if (p == CTUI_LOOK_PICTO_LOGO && max > 0 && value > 0) {
    logo_busy(px, n, value - 1);
  }
  paint(c, x, y, w, h, px, n, off, l);
  if (off && p == CTUI_LOOK_PICTO_SPEAKER) {
    paint(c, x, y, w, h, big ? ctui_look_picto_big_cross : CROSS, n, 0, l);
  } else if (off && p == CTUI_LOOK_PICTO_MIC) {
    paint(c, x, y, w, h, big ? ctui_look_picto_big_slash : SLASH, n, 0, l);
  }
}
