#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BUFFER_SIZE 16384
#define CUSTOM_MSG                                                             \
  "<footer><hr><p>Fornito da... Assolutamente niente! Non c'è di "             \
  "che.</p><hr></footer>"

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

static void send_header(int fd, int code, const char *status, const char *ctype,
                        ssize_t length) {
  char header[BUFFER_SIZE];
  int n = snprintf(header, sizeof(header),
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: %s; charset=utf-8\r\n"
                   "Connection: close\r\n",
                   code, status, ctype);
  if (length >= 0)
    n += snprintf(header + n, sizeof(header) - n, "Content-Length: %zd\r\n",
                  length);
  n += snprintf(header + n, sizeof(header) - n, "\r\n");
  send(fd, header, n, 0);
}

static void send_html_head(int fd) {
  char header[BUFFER_SIZE];
  int n = snprintf(header, sizeof(header),
                   "<!DOCTYPE html>\r\n"
                   "<head>\r\n<meta charset=\"utf-8\"/>\r\n"
                   "</head>\r\n");
  send(fd, header, n, 0);
}

static void url_decode(const char *src, char *dest, size_t dsz) {
  size_t i = 0, j = 0;
  while (src[i] && j + 1 < dsz) {
    if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) &&
        isxdigit((unsigned char)src[i + 2])) {
      char hex[3] = {src[i + 1], src[i + 2], 0};
      dest[j++] = (char)strtol(hex, NULL, 16);
      i += 3;
    } else if (src[i] == '+') {
      dest[j++] = ' ';
      i++;
    } else {
      dest[j++] = src[i++];
    }
  }
  dest[j] = '\0';
}

static bool ends_with_slash(const char *s) {
  size_t n = s ? strlen(s) : 0;
  return n && s[n - 1] == '/';
}
static void ensure_trailing_slash(char *s, size_t cap) {
  size_t len = strnlen(s, cap);
  if (len == 0 || len >= cap)
    return;
  if (s[len - 1] == '/')
    return;
  if (len + 1 < cap) {
    s[len] = '/';
    s[len + 1] = '\0';
  }
}

static int safe_copy(char *dst, size_t dstsz, const char *src) {
  if (!dst || dstsz == 0)
    return -1;
  size_t sl = strnlen(src ? src : "", dstsz);
  if (sl >= dstsz) {
    dst[0] = '\0';
    return -1;
  }
  memcpy(dst, src ? src : "", sl);
  dst[sl] = '\0';
  return (int)sl;
}

static int safe_join(char *dst, size_t dstsz, const char *a, const char *b) {
  if (!dst || dstsz == 0)
    return -1;
  size_t alen = strnlen(a ? a : "", dstsz);
  size_t blen = strlen(b ? b : "");
  if (alen >= dstsz || alen + blen >= dstsz) {
    dst[0] = '\0';
    return -1;
  }
  memcpy(dst, a ? a : "", alen);
  memcpy(dst + alen, b ? b : "", blen);
  dst[alen + blen] = '\0';
  return (int)(alen + blen);
}

static int path_join(char *dst, size_t dstsz, const char *a, const char *b,
                     bool ensure_trailing) {
  if (!dst || dstsz == 0)
    return -1;
  const char *A = a ? a : "";
  const char *B = b ? b : "";
  size_t alen = strnlen(A, dstsz);
  size_t blen = strlen(B);
  int a_has = (alen > 0 && A[alen - 1] == '/');
  int b_has_lead = (blen > 0 && B[0] == '/');

  size_t blen_trim = blen ? (b_has_lead ? blen - 1 : blen) : 0;
  size_t need = alen + (a_has || b_has_lead || alen == 0 ? 0 : 1) + blen_trim +
                (ensure_trailing ? 1 : 0) + 1;
  if (need > dstsz) {
    dst[0] = '\0';
    return -1;
  }

  size_t pos = 0;
  if (alen) {
    memcpy(dst + pos, A, alen);
    pos += alen;
  }
  if (!(a_has || b_has_lead) && !(alen == 0 && blen_trim == 0))
    dst[pos++] = '/';
  if (blen_trim) {
    const char *bstart = b_has_lead ? (B + 1) : B;
    memcpy(dst + pos, bstart, blen_trim);
    pos += blen_trim;
  }
  if (ensure_trailing && (pos == 0 || dst[pos - 1] != '/'))
    dst[pos++] = '/';
  dst[pos] = '\0';
  return (int)pos;
}

static void send_redirect(int fd, const char *location) {
  char hdr[BUFFER_SIZE];
  int n = snprintf(hdr, sizeof(hdr),
                   "HTTP/1.1 301 Moved Permanently\r\n"
                   "Location: %s\r\n"
                   "Content-Length: 0\r\n"
                   "Connection: close\r\n\r\n",
                   location);
  send(fd, hdr, n, 0);
}

static int pick_page(const char *dirpath, char chosen[BUFFER_SIZE],
                     bool *is_markdown) {
  DIR *d = opendir(dirpath);
  if (!d)
    return 0;
  struct dirent *ent;
  char first_md[BUFFER_SIZE] = {0};
  char first_html[BUFFER_SIZE] = {0};
  bool has_index_md = false;
  bool has_index_html = false;
  while ((ent = readdir(d))) {
    if (ent->d_name[0] == '.')
      continue;
    const char *dot = strrchr(ent->d_name, '.');
    if (!dot)
      continue;
    if (strcmp(dot, ".md") == 0) {
      if (strcmp(ent->d_name, "index.md") == 0) {
        has_index_md = true;
        continue;
      }
      if (!first_md[0] || strcmp(ent->d_name, first_md) < 0) {
        safe_copy(first_md, sizeof(first_md), ent->d_name);
      }
    } else if (strcmp(dot, ".html") == 0) {
      if (strcmp(ent->d_name, "index.html") == 0) {
        has_index_html = true;
        continue;
      }
      if (!first_html[0] || strcmp(ent->d_name, first_html) < 0) {
        safe_copy(first_html, sizeof(first_html), ent->d_name);
      }
    }
  }
  closedir(d);
  const char *pick = NULL;
  bool pick_md = false;
  if (has_index_md) {
    pick = "index.md";
    pick_md = true;
  } else if (has_index_html) {
    pick = "index.html";
    pick_md = false;
  } else if (first_md[0]) {
    pick = first_md;
    pick_md = true;
  } else if (first_html[0]) {
    pick = first_html;
    pick_md = false;
  }
  if (pick) {
    if (path_join(chosen, BUFFER_SIZE, dirpath, pick, false) < 0)
      return 0;
    if (is_markdown)
      *is_markdown = pick_md;
    return 1;
  }
  return 0;
}

static int stream_parser_output(int out_fd, const char *filepath,
                                char *const parser_argv[]) {
  int inpipe[2], outpipe[2];
  if (pipe(inpipe) || pipe(outpipe))
    return -1;

  pid_t pid = fork();
  if (pid < 0)
    return -1;

  if (pid == 0) {
    dup2(inpipe[0], STDIN_FILENO);
    dup2(outpipe[1], STDOUT_FILENO);
    close(inpipe[0]);
    close(inpipe[1]);
    close(outpipe[0]);
    close(outpipe[1]);
    execvp(parser_argv[0], parser_argv);
    _exit(127);
  }

  close(inpipe[0]);
  close(outpipe[1]);

  FILE *in = fopen(filepath, "rb");
  if (!in) {
    close(inpipe[1]);
    close(outpipe[0]);
    waitpid(pid, NULL, 0);
    return -1;
  }

  char buf[BUFFER_SIZE];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    ssize_t off = 0;
    while (off < (ssize_t)n) {
      ssize_t w = write(inpipe[1], buf + off, n - off);
      if (w < 0) {
        fclose(in);
        close(inpipe[1]);
        close(outpipe[0]);
        waitpid(pid, NULL, 0);
        return -1;
      }
      off += w;
    }
  }
  fclose(in);
  close(inpipe[1]);

  ssize_t r;
  while ((r = read(outpipe[0], buf, sizeof(buf))) > 0) {
    ssize_t off = 0;
    while (off < r) {
      ssize_t w = send(out_fd, buf + off, r - off, 0);
      if (w <= 0)
        break;
      off += w;
    }
  }
  close(outpipe[0]);

  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void emit_subdirs_recursive(int fd, const char *fsroot,
                                   const char *rel_dir, int depth) {
  if (depth < 0)
    return;

  char dirp[BUFFER_SIZE];
  if (safe_join(dirp, sizeof(dirp), fsroot, rel_dir) < 0)
    return;

  DIR *d = opendir(dirp);
  if (!d)
    return;

  struct dirent *ent;
  char fp[BUFFER_SIZE];
  bool opened_ul = false;

  while ((ent = readdir(d))) {
    if (ent->d_name[0] == '.')
      continue;

    if (path_join(fp, sizeof(fp), dirp, ent->d_name, false) < 0)
      continue;
    struct stat st;
    if (stat(fp, &st) != 0)
      continue;
    if (!S_ISDIR(st.st_mode))
      continue;

    if (!opened_ul) {
      send(fd, "<ul>\n", strlen("<ul>\n"), 0);
      opened_ul = true;
    }

    char href[BUFFER_SIZE];
    if (path_join(href, sizeof(href), rel_dir, ent->d_name, true) < 0)
      continue;

    char li[BUFFER_SIZE];
    int n = snprintf(li, sizeof(li), "<li><a href=\"%s\">%s</a>", href,
                     ent->d_name);
    send(fd, li, n, 0);

    emit_subdirs_recursive(fd, fsroot, href, depth - 1);

    send(fd, "</li>\n", strlen("</li>\n"), 0);
  }

  if (opened_ul) {
    send(fd, "</ul>\n", strlen("</ul>\n"), 0);
  }

  closedir(d);
}

static void emit_related_for_dir(int fd, const char *fsroot,
                                 const char *rel_dir) {
  if (strcmp(rel_dir, "/") == 0) {
    send(fd, "<h2>Articoli</h2>\n", strlen("<h2>Articoli</h2>\n"), 0);
  } else {
    send(fd, "<h2>Articoli correlati</h2>\n",
         strlen("<h2>Articoli correlati</h2>\n"), 0);
  }

  if (strcmp(rel_dir, "/") == 0) {
    emit_subdirs_recursive(fd, fsroot, "/", 8);
    return;
  }

  char dirp[BUFFER_SIZE];
  if (safe_join(dirp, sizeof(dirp), fsroot, rel_dir) < 0)
    return;

  DIR *d = opendir(dirp);
  if (!d)
    return;

  send(fd, "<ul>\n", strlen("<ul>\n"), 0);

  struct dirent *ent;
  char fp[BUFFER_SIZE];
  while ((ent = readdir(d))) {
    if (ent->d_name[0] == '.')
      continue;
    if (path_join(fp, sizeof(fp), dirp, ent->d_name, false) < 0)
      continue;
    struct stat st;
    if (stat(fp, &st) == 0 && S_ISDIR(st.st_mode)) {
      char href[BUFFER_SIZE];
      if (path_join(href, sizeof(href), rel_dir, ent->d_name, true) < 0)
        continue;
      char li[BUFFER_SIZE];
      int n = snprintf(li, sizeof(li), "<li><a href=\"%s\">%s</a></li>\n", href,
                       ent->d_name);
      send(fd, li, n, 0);
    }
  }

  closedir(d);
  send(fd, "</ul>\n", strlen("</ul>\n"), 0);
}

static void serve_markdown_page(int fd, const char *fsroot, const char *rel_dir,
                                const char *rel_file,
                                char *const parser_argv[]) {
  char full[BUFFER_SIZE];
  if (safe_join(full, sizeof(full), fsroot, rel_file) < 0) {
    send_header(fd, 500, "Internal Server Error", "text/plain", -1);
    send(fd, "path too long\n", 14, 0);
    return;
  }

  send_header(fd, 200, "OK", "text/html", -1);
  send_html_head(fd);
  send(fd, "<html><body>", strlen("<html><body>"), 0);

  if (strcmp(rel_dir, "/") != 0) {
    const char *pre = "<p><a href=\"/\">Ritorna all'inizio</a></p><hr>\n";
    send(fd, pre, strlen(pre), 0);
  }

  if (stream_parser_output(fd, full, parser_argv) != 0) {
    const char *msg =
        "<p>Errore: il parser markdown sembra avere problemi.</p>\n";
    send(fd, msg, strlen(msg), 0);
  }

  emit_related_for_dir(fd, fsroot, rel_dir);

  const char *post = CUSTOM_MSG "\n</body></html>";
  send(fd, post, strlen(post), 0);
}

static void serve_directory_listing(int fd, const char *fsroot,
                                    const char *rel) {
  char dirp[BUFFER_SIZE];
  if (safe_join(dirp, sizeof(dirp), fsroot, rel) < 0) {
    send_header(fd, 500, "Internal Server Error", "text/plain", -1);
    send(fd, "path too long\n", 14, 0);
    return;
  }
  DIR *d = opendir(dirp);

  send_header(fd, 200, "OK", "text/html", -1);
  if (strcmp(rel, "/") == 0) {
    send(fd, "<html><body>", strlen("<html><body>"), 0);
  } else {
    send(
        fd, "<html><body><p><a href=\"/\">Ritorna all'inizio</a></p><hr>\n",
        strlen("<html><body><p><a href=\"/\">Ritorna all'inizio</a></p><hr>\n"),
        0);
  }

  if (d) {
    struct dirent *ent;
    char fp[BUFFER_SIZE];
    while ((ent = readdir(d))) {
      if (ent->d_name[0] == '.')
        continue;
      if (path_join(fp, sizeof(fp), dirp, ent->d_name, false) < 0)
        continue;
      struct stat st;
      if (stat(fp, &st) == 0 && !S_ISDIR(st.st_mode)) {
        const char *dot = strrchr(ent->d_name, '.');
        if (dot && (strcmp(dot, ".md") == 0 || strcmp(dot, ".html") == 0)) {
          char href[BUFFER_SIZE];
          if (path_join(href, sizeof(href), rel, ent->d_name, false) < 0)
            continue;
          char item[BUFFER_SIZE];
          int n = snprintf(item, sizeof(item), "<p><a href=\"%s\">%s</a></p>\n",
                           href, ent->d_name);
          send(fd, item, n, 0);
        }
      }
    }

    emit_related_for_dir(fd, fsroot, rel);
    closedir(d);
  }

  send(fd, CUSTOM_MSG "\n</body></html>", strlen(CUSTOM_MSG "\n</body></html>"),
       0);
}

static void serve_file_raw(int fd, const char *fsroot, const char *rel,
                           const char *ctype) {
  char full[BUFFER_SIZE];
  if (safe_join(full, sizeof(full), fsroot, rel) < 0) {
    send_header(fd, 500, "Internal Server Error", "text/plain", -1);
    send(fd, "path too long\n", 14, 0);
    return;
  }

  int f = open(full, O_RDONLY);
  if (f < 0) {
    send_header(fd, 404, "Not Found", "text/plain", -1);
    const char *msg = "404 not found\n";
    send(fd, msg, strlen(msg), 0);
    return;
  }
  struct stat st;
  fstat(f, &st);
  send_header(fd, 200, "OK", ctype, st.st_size);
  off_t off = 0;
  sendfile(fd, f, &off, st.st_size);
  close(f);
}

static void dirname_rel(const char *rel_file, char out[BUFFER_SIZE]) {
  const char *slash = strrchr(rel_file, '/');
  if (!slash || slash == rel_file) {
    safe_copy(out, BUFFER_SIZE, "/");
    return;
  }
  size_t len = (size_t)(slash - rel_file) + 1;
  if (len >= BUFFER_SIZE)
    len = BUFFER_SIZE - 1;
  memcpy(out, rel_file, len);
  out[len] = '\0';
}

static void handle_client(int fd, const char *fsroot,
                          char *const parser_argv[]) {
  char buf[BUFFER_SIZE];
  ssize_t r = recv(fd, buf, sizeof(buf) - 1, 0);
  if (r <= 0) {
    close(fd);
    return;
  }
  buf[r] = 0;

  char method[16], path[BUFFER_SIZE];
  if (sscanf(buf, "%15s %16383s", method, path) != 2) {
    close(fd);
    return;
  }
  if (strcmp(method, "GET") != 0) {
    send_header(fd, 405, "Method Not Allowed", "text/plain", -1);
    close(fd);
    return;
  }

  char decoded[BUFFER_SIZE];
  url_decode(path, decoded, sizeof(decoded)); // starts with '/'

  char rootcanon[BUFFER_SIZE];
  if (!realpath(fsroot, rootcanon)) {
    send_header(fd, 500, "Internal Server Error", "text/plain", -1);
    close(fd);
    return;
  }

  char joined[BUFFER_SIZE];
  if (safe_join(joined, sizeof(joined), rootcanon, decoded) < 0) {
    send_header(fd, 414, "URI Too Long", "text/plain", -1);
    send(fd, "path too long\n", 14, 0);
    close(fd);
    return;
  }

  char canon[BUFFER_SIZE];
  if (!realpath(joined, canon) ||
      strncmp(canon, rootcanon, strlen(rootcanon)) != 0) {
    send_header(fd, 403, "Forbidden", "text/plain", -1);
    close(fd);
    return;
  }

  struct stat st;
  if (stat(canon, &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      if (!ends_with_slash(decoded)) {
        char want[BUFFER_SIZE];
        if (safe_copy(want, sizeof(want), decoded) < 0) {
          close(fd);
          return;
        }
        ensure_trailing_slash(want, sizeof(want));
        send_redirect(fd, want);
        close(fd);
        return;
      }

      const char *rf = canon + strlen(rootcanon);
      char rel_dir[BUFFER_SIZE];
      if (!*rf) {
        safe_copy(rel_dir, sizeof(rel_dir), "/");
      } else {
        if (*rf != '/') {
          char tmp[BUFFER_SIZE] = "/";
          safe_join(rel_dir, sizeof(rel_dir), tmp, rf);
        } else {
          safe_copy(rel_dir, sizeof(rel_dir), rf);
        }
        ensure_trailing_slash(rel_dir, sizeof(rel_dir));
      }

      char pagepath[BUFFER_SIZE];
      bool is_markdown = false;
      if (pick_page(canon, pagepath, &is_markdown)) {
        const char *rf2 = pagepath + strlen(rootcanon);
        char rel_file[BUFFER_SIZE];
        if (*rf2 == '/')
          safe_copy(rel_file, sizeof(rel_file), rf2);
        else
          safe_copy(rel_file, sizeof(rel_file), "/");
        if (is_markdown) {
          serve_markdown_page(fd, rootcanon, rel_dir, rel_file, parser_argv);
        } else {
          serve_file_raw(fd, rootcanon, rel_file, "text/html");
        }
      } else {
        serve_directory_listing(fd, rootcanon, rel_dir);
      }

    } else {
      const char *rf = canon + strlen(rootcanon);
      char relfile[BUFFER_SIZE];
      if (*rf == '/')
        safe_copy(relfile, sizeof(relfile), rf);
      else
        safe_copy(relfile, sizeof(relfile), "/");

      const char *dot = strrchr(relfile, '.');
      if (dot && strcmp(dot, ".md") == 0) {
        char rel_dir[BUFFER_SIZE];
        dirname_rel(relfile, rel_dir);
        serve_markdown_page(fd, rootcanon, rel_dir, relfile, parser_argv);
      } else if (dot && strcmp(dot, ".html") == 0) {
        serve_file_raw(fd, rootcanon, relfile, "text/html");
      } else {
        serve_file_raw(fd, rootcanon, relfile, "application/octet-stream");
      }
    }
  } else {
    send_header(fd, 404, "Not Found", "text/plain", -1);
    const char *msg = "404 not found\n";
    send(fd, msg, strlen(msg), 0);
  }

  close(fd);
}

int main(int argc, char **argv) {
  int port = 8080;
  const char *root = ".";
  const char *parser = "mdparse";
  int opt;
  while ((opt = getopt(argc, argv, "p:r:x:")) != -1) {
    switch (opt) {
    case 'p':
      port = atoi(optarg);
      break;
    case 'r':
      root = optarg;
      break;
    case 'x':
      parser = optarg;
      break;
    default:
      fprintf(stderr, "Usage: %s [-p port] [-r root] [-x parser]\n", argv[0]);
      exit(1);
    }
  }

  char *pargv[2] = {(char *)parser, NULL};

  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0)
    die("socket: %s", strerror(errno));
  int optval = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = INADDR_ANY;
  if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0)
    die("bind: %s", strerror(errno));
  if (listen(s, 64) < 0)
    die("listen: %s", strerror(errno));

  printf("Serving %s on port %d using parser '%s'\n", root, port, parser);

  struct sigaction sa = {0};
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = SA_RESTART;
  sigaction(SIGCHLD, &sa, NULL);

  while (1) {
    struct sockaddr_in c;
    socklen_t l = sizeof(c);
    int fd = accept(s, (struct sockaddr *)&c, &l);
    if (fd < 0)
      continue;
    pid_t pid = fork();
    if (pid == 0) {
      close(s);
      handle_client(fd, root, pargv);
      _exit(0);
    }
    close(fd);
  }
}
