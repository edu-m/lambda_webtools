#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 8192

static void html_escape(const char *in, char *out, size_t out_sz) {
  size_t i = 0, j = 0;
  while (in[i] && j + 7 < out_sz) {
    switch (in[i]) {
    case '&':
      if (j + 5 < out_sz) {
        memcpy(out + j, "&amp;", 5);
        j += 5;
      }
      break;
    case '<':
      if (j + 4 < out_sz) {
        memcpy(out + j, "&lt;", 4);
        j += 4;
      }
      break;
    case '>':
      if (j + 4 < out_sz) {
        memcpy(out + j, "&gt;", 4);
        j += 4;
      }
      break;
    case '"':
      if (j + 6 < out_sz) {
        memcpy(out + j, "&quot;", 6);
        j += 6;
      }
      break;
    default:
      out[j++] = in[i];
      break;
    }
    i++;
  }
  out[j] = '\0';
}

static int is_escaped_at(const char *s, size_t pos) {
  if (pos == 0)
    return 0;
  size_t k = pos, backslashes = 0;
  while (k > 0 && s[k - 1] == '\\') {
    backslashes++;
    k--;
  }
  return (backslashes % 2) == 1;
}

static const char *find_unescaped(const char *s, size_t idx, char ch) {
  for (size_t i = idx; s[i]; i++) {
    if (s[i] == ch && !is_escaped_at(s, i))
      return s + i;
  }
  return NULL;
}

static void unescape_backslashes(const char *src, char *dest, size_t dest_sz) {
  size_t i = 0, j = 0;
  while (src[i] && j + 1 < dest_sz) {
    if (src[i] == '\\' && src[i + 1] != '\0') {
      dest[j++] = src[++i];
      i++;
    } else {
      dest[j++] = src[i++];
    }
  }
  dest[j] = '\0';
}

static void inline_format(const char *src, char *dest, size_t dest_sz) {
  char esc[BUFFER_SIZE];
  size_t i = 0, j = 0;

  while (src[i] && j + 16 < dest_sz) {
    if (src[i] == '\\' && src[i + 1]) {
      char tmp[2] = {src[i + 1], 0};
      html_escape(tmp, esc, sizeof(esc));
      size_t el = strlen(esc);
      if (j + el >= dest_sz)
        break;
      memcpy(dest + j, esc, el);
      j += el;
      i += 2;
      continue;
    }

    if (src[i] == '*' && src[i + 1] == '*' && !is_escaped_at(src, i)) {
      size_t start = i + 2;
      const char *endp = NULL;
      for (size_t k = start; src[k]; k++) {
        if (src[k] == '*' && src[k + 1] == '*' && !is_escaped_at(src, k)) {
          endp = src + k;
          break;
        }
      }
      size_t len = endp ? (size_t)(endp - (src + start)) : strlen(src + start);
      char inner[BUFFER_SIZE], unesc[BUFFER_SIZE];
      snprintf(inner, sizeof(inner), "%.*s", (int)len, src + start);
      unescape_backslashes(inner, unesc, sizeof(unesc));
      html_escape(unesc, esc, sizeof(esc));
      int n = snprintf(dest + j, dest_sz - j, "<strong>%s</strong>", esc);
      if (n < 0)
        break;
      j += (size_t)n;
      if (endp)
        i = (size_t)(endp - src) + 2;
      else
        break;
      continue;
    }

    if (src[i] == '*' && !is_escaped_at(src, i)) {
      size_t start = i + 1;
      const char *endp = find_unescaped(src, start, '*');
      size_t len = endp ? (size_t)(endp - (src + start)) : strlen(src + start);
      char inner[BUFFER_SIZE], unesc[BUFFER_SIZE];
      snprintf(inner, sizeof(inner), "%.*s", (int)len, src + start);
      unescape_backslashes(inner, unesc, sizeof(unesc));
      html_escape(unesc, esc, sizeof(esc));
      int n = snprintf(dest + j, dest_sz - j, "<em>%s</em>", esc);
      if (n < 0)
        break;
      j += (size_t)n;
      if (endp)
        i = (size_t)(endp - src) + 1;
      else
        break;
      continue;
    }

    char tmp[2] = {src[i++], 0};
    html_escape(tmp, esc, sizeof(esc));
    size_t el = strlen(esc);
    if (j + el >= dest_sz)
      break;
    memcpy(dest + j, esc, el);
    j += el;
  }

  dest[j] = '\0';
}

static void markdown_to_html(FILE *in, FILE *out) {
  char line[BUFFER_SIZE], buf[BUFFER_SIZE], outbuf[BUFFER_SIZE];

  while (fgets(line, sizeof(line), in)) {
    line[strcspn(line, "\r\n")] = '\0';

    if (line[0] == '#') {
      int lvl = 0;
      while (line[lvl] == '#')
        lvl++;
      const char *txt = line + lvl;
      while (*txt == ' ')
        txt++;
      inline_format(txt, buf, sizeof(buf));
      fprintf(out, "<h%d>%s</h%d>\n", lvl, buf, lvl);
      continue;
    }

    if (line[0] == '\0') {
      fputc('\n', out);
      continue;
    }

    size_t i = 0;
    const char *p = find_unescaped(line, i, '[');
    if (p) {
      const char *q = find_unescaped(line, (size_t)(p - line) + 1, ']');
      size_t after_q = q ? (size_t)(q - line) + 1 : 0;
      while (q && (line[after_q] == ' ' || line[after_q] == '\t'))
        after_q++;
      const char *r = q ? find_unescaped(line, after_q, '(') : NULL;
      const char *s =
          r ? find_unescaped(line, (size_t)(r - line) + 1, ')') : NULL;

      if (q && r && s && q < r && r < s) {
        char before[BUFFER_SIZE];
        snprintf(before, sizeof(before), "%.*s", (int)(p - line), line);
        inline_format(before, outbuf, sizeof(outbuf));
        fputs(outbuf, out);

        char linktxt[BUFFER_SIZE], linktxt_fmt[BUFFER_SIZE];
        snprintf(linktxt, sizeof(linktxt), "%.*s", (int)(q - p - 1), p + 1);
        inline_format(linktxt, linktxt_fmt, sizeof(linktxt_fmt));

        char urlraw[BUFFER_SIZE], url_unesc[BUFFER_SIZE], url_attr[BUFFER_SIZE];
        snprintf(urlraw, sizeof(urlraw), "%.*s", (int)(s - r - 1), r + 1);
        unescape_backslashes(urlraw, url_unesc, sizeof(url_unesc));
        html_escape(url_unesc, url_attr, sizeof(url_attr));
        fprintf(out, "<a href=\"%s\">%s</a>", url_attr, linktxt_fmt);

        char after[BUFFER_SIZE];
        snprintf(after, sizeof(after), "%s", s + 1);
        inline_format(after, outbuf, sizeof(outbuf));
        fputs(outbuf, out);

        fputc('\n', out);
        continue;
      }
    }

    inline_format(line, buf, sizeof(buf));
    fprintf(out, "<p>%s</p>\n", buf);
  }
}

int main(void) {
  markdown_to_html(stdin, stdout);
  return 0;
}
