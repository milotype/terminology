#include "private.h"
#include <Elementary.h>
#include "termpty.h"
#include "backlog.h"
#include "termiolink.h"
#include "termio.h"
#include "sb.h"
#include "utf8.h"
#include "utils.h"

static Eina_Bool
_isspace_unicode(const int codepoint)
{
   switch (codepoint)
     {
      case 9: // character tabulation
         EINA_FALLTHROUGH;
      case 10: // line feed
         EINA_FALLTHROUGH;
      case 11: // line tabulation
         EINA_FALLTHROUGH;
      case 12: // form feed
         EINA_FALLTHROUGH;
      case 13: // carriage return
         EINA_FALLTHROUGH;
      case 32: // space
         EINA_FALLTHROUGH;
      case 133: // next line
         EINA_FALLTHROUGH;
      case 160: // no-break space
         EINA_FALLTHROUGH;
      case 5760: // ogham space mark
         EINA_FALLTHROUGH;
      case 6158: // mongolian vowel separator
         EINA_FALLTHROUGH;
      case 8192: // en quad
         EINA_FALLTHROUGH;
      case 8193: // em quad
         EINA_FALLTHROUGH;
      case 8194: // en space
         EINA_FALLTHROUGH;
      case 8195: // em space
         EINA_FALLTHROUGH;
      case 8196: // three-per-em space
         EINA_FALLTHROUGH;
      case 8197: // four-per-em space
         EINA_FALLTHROUGH;
      case 8198: // six-per-em space
         EINA_FALLTHROUGH;
      case 8199: // figure space
         EINA_FALLTHROUGH;
      case 8200: // puncturation space
         EINA_FALLTHROUGH;
      case 8201: // thin space
         EINA_FALLTHROUGH;
      case 8202: // hair space
         EINA_FALLTHROUGH;
      case 8203: // zero width space
         EINA_FALLTHROUGH;
      case 8204: // zero width non-joiner
         EINA_FALLTHROUGH;
      case 8205: // zero width joiner
         EINA_FALLTHROUGH;
      case 8232: // line separator
         EINA_FALLTHROUGH;
      case 8233: // paragraph separator
         EINA_FALLTHROUGH;
      case 8239: // narrow no-break space
         EINA_FALLTHROUGH;
      case 8287: // medium mathematical space
         EINA_FALLTHROUGH;
      case 8288: // word joiner
         EINA_FALLTHROUGH;
      case 12288: // ideographic space
         EINA_FALLTHROUGH;
      case 65279: // zero width non-breaking space
         return EINA_TRUE;
     }
   return EINA_FALSE;
}


static char *
_cwd_path_get(const Evas_Object *obj, const char *relpath)
{
   char cwdpath[PATH_MAX], tmppath[PATH_MAX];

   if (!termio_cwd_get(obj, cwdpath, sizeof(cwdpath)))
     return NULL;

   eina_str_join(tmppath, sizeof(tmppath), '/', cwdpath, relpath);
   return strdup(tmppath);
}

static char *
_home_path_get(const Evas_Object *_obj EINA_UNUSED,
               const char *relpath)
{
   char tmppath[PATH_MAX], homepath[PATH_MAX];

   if (!homedir_get(homepath, sizeof(homepath)))
     return NULL;

   eina_str_join(tmppath, sizeof(tmppath), '/', homepath, relpath);
   return strdup(tmppath);
}

static char *
_local_path_get(const Evas_Object *obj, const char *relpath)
{
   if (relpath[0] == '/')
     return strdup(relpath);
   else if (eina_str_has_prefix(relpath, "~/"))
     return _home_path_get(obj, relpath + 2);
   else
     return _cwd_path_get(obj, relpath);
}

Eina_Bool
link_is_protocol(const char *str)
{
   const char *p = str;
   int c;

   if (!str)
     return EINA_FALSE;

   c = *p;
   if (!isalpha(c))
     return EINA_FALSE;

   /* Try to follow RFC3986 a bit
    * URI         = scheme ":" hier-part [ "?" query ] [ "#" fragment ]
    * hier-part   = "//" authority path-abempty
    *             [...] other stuff not taken into account
    * scheme      = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
    */

   do
     {
        p++;
        c = *p;
     }
   while (isalpha(c) || (c == '.') || (c == '-') || (c == '+'));

   return (p[0] == ':') && (p[1] == '/') && (p[2] == '/');
}

Eina_Bool
link_is_url(const char *str)
{
   if (!str)
     return EINA_FALSE;

   if (link_is_protocol(str) ||
       casestartswith(str, "www.") ||
       casestartswith(str, "ftp."))
     return EINA_TRUE;
   return EINA_FALSE;
}

Eina_Bool
link_is_email(const char *str)
{
   const char *at;

   if (!str)
     return EINA_FALSE;

   at = strchr(str, '@');
   if (at && strchr(at + 1, '.'))
     return EINA_TRUE;
   if (casestartswith(str, "mailto:"))
     return EINA_TRUE;
   return EINA_FALSE;
}

Eina_Bool
link_is_file(const char *str)
{
   if (!str)
     return EINA_FALSE;

   switch (str[0])
     {
      case '/':
        return EINA_TRUE;
      case '~':
        if (str[1] == '/')
          return EINA_TRUE;
        return EINA_FALSE;
      case '.':
        if (str[1] == '/')
          return EINA_TRUE;
        else if ((str[1] == '.') && (str[2] == '/'))
          return EINA_TRUE;
        return EINA_FALSE;
      default:
        return EINA_FALSE;
     }
}

static int
_txt_at(Termpty *ty, int *x, int *y, char *txt, int *txtlenp, int *codepointp)
{
   Termcell *cells = NULL;
   Termcell cell;
   ssize_t w;

   cells = termpty_cellrow_get(ty, *y, &w);
   if (!cells || !w)
     goto bad;
   if ((*x >= w))
     goto empty;
   cell = cells[*x];
   if ((cell.codepoint == 0) && (cell.att.dblwidth))
     {
        (*x)--;
        if (*x < 0)
          goto bad;
        cell = cells[*x];
     }

   if ((cell.codepoint == 0) || (cell.att.link_id))
     goto empty;

   *txtlenp = codepoint_to_utf8(cell.codepoint, txt);
   *codepointp = cell.codepoint;

   return 0;

empty:
        txt[0] = '\0';
        *txtlenp = 0;
        *codepointp = 0;
   return 0;

bad:
   *txtlenp = 0;
   txt[0] = '\0';
   return -1;
}

static int
_txt_prev_at(Termpty *ty, int *x, int *y, char *txt, int *txtlenp,
             int *codepointp)
{
   Termcell *cells = NULL;
   Termcell cell;
   ssize_t w;

   (*x)--;
   if ((*x) < 0)
     {
        (*y)--;
        *x = ty->w-1;
        cells = termpty_cellrow_get(ty, *y, &w);
        if (!cells || !w)
          goto bad;
        if ((*x) >= w)
          goto empty;
        cell = cells[*x];
        /* Either the cell is in the normal screen and needs to have
         * autowrapped flag or is in the backlog and its length is larger than
         * the screen, spanning multiple lines */
        if (((!cell.att.autowrapped) && (*y) >= 0)
            || (w < ty->w))
          goto empty;
     }
   else
     {
        cells = termpty_cellrow_get(ty, *y, &w);
        if (!cells || !w)
          goto bad;
        if ((*x) >= w)
          goto empty;

        cell = cells[*x];
     }
   if ((cell.codepoint == 0) && (cell.att.dblwidth))
     {
        (*x)--;
        if (*x < 0)
          goto bad;
        cell = cells[*x];
     }

   if ((cell.codepoint == 0) || (cell.att.link_id))
     goto empty;

   *txtlenp = codepoint_to_utf8(cell.codepoint, txt);
   *codepointp = cell.codepoint;

   return 0;

empty:
   txt[0] = '\0';
   *txtlenp = 0;
   *codepointp = 0;
   return 0;

bad:
   *txtlenp = 0;
   txt[0] = '\0';
   return -1;
}

static int
_txt_next_at(Termpty *ty, int *x, int *y, char *txt, int *txtlenp,
             int *codepointp)
{
   Termcell *cells = NULL;
   Termcell cell;
   ssize_t w;

   (*x)++;
   cells = termpty_cellrow_get(ty, *y, &w);
   if (!cells || !w)
     goto bad;
   if ((*x) >= w)
     {
        (*y)++;
        if ((*x) <= ty->w)
          {
             cell = cells[w-1];
             if (!cell.att.autowrapped)
               goto empty;
          }

        *x = 0;
        cells = termpty_cellrow_get(ty, *y, &w);
        if (!cells || !w)
          goto bad;
     }

   cell = cells[*x];
   if ((cell.codepoint == 0) && (cell.att.dblwidth))
     {
        (*x)++;
        if (*x >= w)
          {
             cell = cells[w-1];
             if (!cell.att.autowrapped && w == ty->w)
               goto empty;
             (*y)++;
             *x = 0;
             cells = termpty_cellrow_get(ty, *y, &w);
             if (!cells || !w)
               goto bad;
          }
          goto bad;
     }

   cell = cells[*x];
   if ((cell.codepoint == 0) || (cell.att.link_id))
     goto empty;

   *txtlenp = codepoint_to_utf8(cell.codepoint, txt);
   *codepointp = cell.codepoint;

   return 0;

empty:
   txt[0] = '\0';
   *txtlenp = 0;
   *codepointp = 0;
   return 0;

bad:
   *txtlenp = 0;
   txt[0] = '\0';
   return -1;
}

/* returned string must be freed */
char *
termio_link_find(const Evas_Object *obj, int cx, int cy,
                 int *x1r, int *y1r, int *x2r, int *y2r)
{
   char *s = NULL;
   int endmatch1 = 0, endmatch2 = 0;
   int x1, x2, y1, y2, w = 0, h = 0, sc;
   Eina_Bool goback = EINA_TRUE,
             goforward = EINA_FALSE,
             escaped = EINA_FALSE;
   struct ty_sb sb = {.buf = NULL, .gap = 0, .len = 0, .alloc = 0};
   Termpty *ty = termio_pty_get(obj);
   int res;
   char txt[8];
   int txtlen = 0;
   int codepoint = 0;
   Eina_Bool was_protocol = EINA_FALSE;

   EINA_SAFETY_ON_NULL_RETURN_VAL(ty, NULL);

   x1 = x2 = cx;
   y1 = y2 = cy;
   termio_size_get(obj, &w, &h);
   if ((w <= 0) || (h <= 0)) goto end;

   sc = termio_scroll_get(obj);

   termpty_backlog_lock();

   y1 -= sc;
   y2 -= sc;

   res = _txt_at(ty, &x1, &y1, txt, &txtlen, &codepoint);
   if ((res != 0) || (txtlen == 0)) goto end;
   if (_isspace_unicode(codepoint))
     goto end;
   res = ty_sb_add(&sb, txt, txtlen);
   if (res < 0) goto end;

   while (goback)
     {
        int new_x1 = x1, new_y1 = y1;

        res = _txt_prev_at(ty, &new_x1, &new_y1, txt, &txtlen, &codepoint);
        if ((res != 0) || (txtlen == 0))
          {
             goback = EINA_FALSE;
             goforward = EINA_TRUE;
             break;
          }
        res = ty_sb_prepend(&sb, txt, txtlen);
        if (res < 0) goto end;
        if (_isspace_unicode(codepoint))
          {
             int old_txtlen = txtlen;
             res = _txt_prev_at(ty, &new_x1, &new_y1, txt, &txtlen, &codepoint);
             if ((res != 0) || (txtlen == 0) || (codepoint != '\\'))
               {
                  ty_sb_lskip(&sb, old_txtlen);
                  goback = EINA_FALSE;
                  goforward = EINA_TRUE;
                  break;
               }
          }
        switch (codepoint)
          {
           case '"':  endmatch1 = endmatch2 = '"'; break;
           case '\'': endmatch1 = endmatch2 = '\''; break;
           case '`':  endmatch1 = endmatch2 = '`'; break;
           case '<':  endmatch1 = endmatch2 = '>'; break;
           case '[':  endmatch1 = endmatch2 = ']'; break;
           case ']':  endmatch1 = endmatch2 = '['; break;
           case '{':  endmatch1 = endmatch2 = '}'; break;
           case '(':  endmatch1 = endmatch2 = ')'; break;
           case '|':  endmatch1 = endmatch2 = '|'; break;
           case 0xab:  endmatch1 = endmatch2 = 0xbb; break; // french « »
           case 0xbb:  endmatch1 = endmatch2 = 0xab; break; // swedish » «
           case 0x2018:  endmatch1 = endmatch2 = 0x2019; break;  // ‘ ’
           case 0x201b:  endmatch1 = endmatch2 = 0x2019; break;  // ‛ ’
           case 0x201c:  endmatch1 = endmatch2 = 0x201d; break;  // “ ”
           case 0x201e:  endmatch1 = 0x201c; endmatch2 = 0x201d; break;  // „ “”
           case 0x2039: endmatch1 = endmatch2 = 0x203a; break; // ‹›
           case 0x27e6:  endmatch1 = endmatch2 = 0x27e7; break; // ⟦ ⟧
           case 0x27e8:  endmatch1 = endmatch2 = 0x27e9; break; // ⟨ ⟩
           case 0x2329:  endmatch1 = endmatch2 = 0x232a; break; // 〈 〉
           case 0x231c:  endmatch1 = 0x231d; endmatch2 = 0x231f; break;  // ⌜⌝⌟
           case 0x231e:  endmatch1 = 0x231d; endmatch2 = 0x231f; break;  // ⌞⌝⌟
           case 0x2308:  endmatch1 = 0x2309; endmatch2 = 0x230b; break;  // ⌈⌉⌋
           case 0x230a:  endmatch1 = 0x2309; endmatch2 = 0x230b; break;  // ⌊⌉⌋
          }
        if (endmatch1)
          {
             ty_sb_lskip(&sb, txtlen);
             goback = EINA_FALSE;
             goforward = EINA_TRUE;
             break;
          }

        if (!link_is_protocol(sb.buf))
          {
             if (was_protocol)
               {
                  if (!_isspace_unicode(codepoint))
                    endmatch1 = endmatch2 = codepoint;
                  ty_sb_lskip(&sb, txtlen);
                  goback = EINA_FALSE;
                  goforward = EINA_TRUE;
                  break;
               }
          }
        else
          {
             was_protocol = EINA_TRUE;
          }
        x1 = new_x1;
        y1 = new_y1;
     }

   while (goforward)
     {
        int new_x2 = x2, new_y2 = y2;
        /* Check if the previous char is a delimiter */
        res = _txt_next_at(ty, &new_x2, &new_y2, txt, &txtlen, &codepoint);
        if ((res != 0) || (txtlen == 0))
          {
             goforward = EINA_FALSE;
             break;
          }
        /* escaping */
        if (codepoint == '\\')
          {
             x2 = new_x2;
             y2 = new_y2;
             escaped = EINA_TRUE;
             continue;
          }
        if (escaped)
          {
             x2 = new_x2;
             y2 = new_y2;
             escaped = EINA_FALSE;
          }

        if (_isspace_unicode(codepoint) || (codepoint == endmatch1)
            || (codepoint == endmatch2))
          {
             goforward = EINA_FALSE;
             break;
          }
        switch (codepoint)
          {
           case '"':
           case '\'':
           case '`':
           case '<':
           case '>':
           case '[':
           case ']':
           case '{':
           case '}':
           case '|':
           case 0xab:
           case 0xbb:
           case 0x2018:
           case 0x2019:
           case 0x201b:
           case 0x201c:
           case 0x201d:
           case 0x201e:
           case 0x2039:
           case 0x203a:
           case 0x2308:
           case 0x2309:
           case 0x230a:
           case 0x230b:
           case 0x231c:
           case 0x231d:
           case 0x231e:
           case 0x231f:
           case 0x2329:
           case 0x232a:
           case 0x27e6:
           case 0x27e7:
           case 0x27e8:
           case 0x27e9:
             goto out;
          }

        res = ty_sb_add(&sb, txt, txtlen);
        if (res < 0) goto end;

        if (!link_is_protocol(sb.buf))
          {
             if (was_protocol)
               {
                  ty_sb_rskip(&sb, txtlen);
                  goback = EINA_FALSE;
               }
          }
        else
          {
             was_protocol = EINA_TRUE;
          }
        x2 = new_x2;
        y2 = new_y2;
     }

out:
   if (sb.len)
     {
        Eina_Bool is_file = link_is_file(sb.buf);

        if (is_file ||
            link_is_email(sb.buf) ||
            link_is_url(sb.buf))
          {
             if (x1r) *x1r = x1;
             if (y1r) *y1r = y1 + sc;
             if (x2r) *x2r = x2;
             if (y2r) *y2r = y2 + sc;

             if (is_file && (sb.buf[0] != '/'))
               {
                  char *local= _local_path_get(obj, (const char*)sb.buf);
                  ty_sb_free(&sb);
                  s = local;
                  goto end;
               }
             else
               {
                  s = ty_sb_steal_buf(&sb);
               }

             goto end;
          }
     }
end:
   termpty_backlog_unlock();
   ty_sb_free(&sb);
   return s;
}

static Eina_Bool
_is_authorized_in_color(const int codepoint)
{
   switch (codepoint)
     {
      case '#': EINA_FALLTHROUGH;
      case '0': EINA_FALLTHROUGH;
      case '1': EINA_FALLTHROUGH;
      case '2': EINA_FALLTHROUGH;
      case '3': EINA_FALLTHROUGH;
      case '4': EINA_FALLTHROUGH;
      case '5': EINA_FALLTHROUGH;
      case '6': EINA_FALLTHROUGH;
      case '7': EINA_FALLTHROUGH;
      case '8': EINA_FALLTHROUGH;
      case '9': EINA_FALLTHROUGH;
      case 'a': EINA_FALLTHROUGH;
      case 'A': EINA_FALLTHROUGH;
      case 'b': EINA_FALLTHROUGH;
      case 'B': EINA_FALLTHROUGH;
      case 'c': EINA_FALLTHROUGH;
      case 'C': EINA_FALLTHROUGH;
      case 'd': EINA_FALLTHROUGH;
      case 'D': EINA_FALLTHROUGH;
      case 'e': EINA_FALLTHROUGH;
      case 'E': EINA_FALLTHROUGH;
      case 'f': EINA_FALLTHROUGH;
      case 'F':
         return EINA_TRUE;
     }
   return EINA_FALSE;
}

static Eina_Bool
_parse_hex(char c, uint8_t *v)
{
   if (c < '0')
     return EINA_FALSE;
   if (c <= '9')
     {
        *v = c - '0';
        return EINA_TRUE;
     }
   if (c < 'A')
     return EINA_FALSE;
   if (c <= 'F')
     {
        *v = 10 + c - 'A';
        return EINA_TRUE;
     }
   if (c < 'a')
     return EINA_FALSE;
   if (c <= 'f')
     {
        *v = 10 + c - 'a';
        return EINA_TRUE;
     }
   return EINA_FALSE;
}

static Eina_Bool
_parse_2hex(char *s, uint8_t *v)
{
   uint8_t v0, v1;
   if (!_parse_hex(s[0], &v0))
     return EINA_FALSE;
   if (!_parse_hex(s[1], &v1))
     return EINA_FALSE;
   *v = v0 << 4 | v1;
   return EINA_TRUE;
}

Eina_Bool
termio_color_find(const Evas_Object *obj, int cx, int cy,
                  int *x1r, int *y1r, int *x2r, int *y2r,
                  uint8_t *rp, uint8_t *gp, uint8_t *bp, uint8_t *ap)
{
   int x1, x2, y1, y2, w = 0, h = 0, sc;
   //const char authorized[] = "#0123456789aAbBcCdDeEfFrghsoltun() ,+/";
   Eina_Bool goback = EINA_TRUE,
             goforward = EINA_FALSE;
   struct ty_sb sb = {.buf = NULL, .gap = 0, .len = 0, .alloc = 0};
   Termpty *ty = termio_pty_get(obj);
   int res;
   char txt[8];
   int txtlen = 0;
   Eina_Bool found = EINA_FALSE;
   int codepoint;
   uint8_t r, g, b, a = 255;


   EINA_SAFETY_ON_NULL_RETURN_VAL(ty, EINA_FALSE);

   x1 = x2 = cx;
   y1 = y2 = cy;
   termio_size_get(obj, &w, &h);
   if ((w <= 0) || (h <= 0)) goto end;

   sc = termio_scroll_get(obj);

   termpty_backlog_lock();

   y1 -= sc;
   y2 -= sc;

   /* TODO: boris */
   res = _txt_at(ty, &x1, &y1, txt, &txtlen, &codepoint);
   if ((res != 0) || (txtlen == 0))
     goto end;
   if (!_is_authorized_in_color(codepoint))
     goto end;
   res = ty_sb_add(&sb, txt, txtlen);
   if (res < 0) goto end;

   while (goback)
     {
        int new_x1 = x1, new_y1 = y1;

        res = _txt_prev_at(ty, &new_x1, &new_y1, txt, &txtlen, &codepoint);
        if ((res != 0) || (txtlen == 0))
          {
             goback = EINA_FALSE;
             goforward = EINA_TRUE;
             break;
          }
        res = ty_sb_prepend(&sb, txt, txtlen);
        if (res < 0) goto end;
        if (!_is_authorized_in_color(codepoint))
          {
             ty_sb_lskip(&sb, txtlen);
             goback = EINA_FALSE;
             goforward = EINA_TRUE;
             break;
          }

        x1 = new_x1;
        y1 = new_y1;
     }

   while (goforward)
     {
        int new_x2 = x2, new_y2 = y2;

        /* Check if the previous char is a delimiter */
        res = _txt_next_at(ty, &new_x2, &new_y2, txt, &txtlen, &codepoint);
        if ((res != 0) || (txtlen == 0))
          {
             goforward = EINA_FALSE;
             break;
          }

        if (!_is_authorized_in_color(codepoint))
          {
             goforward = EINA_FALSE;
             break;
          }

        res = ty_sb_add(&sb, txt, txtlen);
        if (res < 0) goto end;

        x2 = new_x2;
        y2 = new_y2;
     }

   if (!sb.len)
     goto end;

   if (sb.buf[0] == '#')
     {
        ty_sb_lskip(&sb, 1);
        switch (sb.len)
          {
           case 3:
              if ((!_parse_hex(sb.buf[0], &r)) ||
                  (!_parse_hex(sb.buf[1], &g)) ||
                  (!_parse_hex(sb.buf[2], &b)))
                goto end;
              r <<= 4;
              g <<= 4;
              b <<= 4;
              break;
           case 4:
              if ((!_parse_hex(sb.buf[0], &r)) ||
                  (!_parse_hex(sb.buf[1], &g)) ||
                  (!_parse_hex(sb.buf[2], &b)) ||
                  (!_parse_hex(sb.buf[3], &a)))
                goto end;
              r <<= 4;
              g <<= 4;
              b <<= 4;
              a <<= 4;
              break;
           case 6:
              if ((!_parse_2hex(&sb.buf[0], &r)) ||
                  (!_parse_2hex(&sb.buf[2], &g)) ||
                  (!_parse_2hex(&sb.buf[4], &b)))
                goto end;
              break;
           case 8:
              if ((!_parse_2hex(&sb.buf[0], &r)) ||
                  (!_parse_2hex(&sb.buf[2], &g)) ||
                  (!_parse_2hex(&sb.buf[4], &b)) ||
                  (!_parse_2hex(&sb.buf[6], &a)))
                goto end;
              break;
           default:
              goto end;
          }
        found = EINA_TRUE;
     }

end:
   termpty_backlog_unlock();
   ty_sb_free(&sb);
   if (found)
     {
        if (rp) *rp = r;
        if (gp) *gp = g;
        if (bp) *bp = b;
        if (ap) *ap = a;
        if (x1r) *x1r = x1;
        if (y1r) *y1r = y1 + sc;
        if (x2r) *x2r = x2;
        if (y2r) *y2r = y2 + sc;
     }
   return found;
}
