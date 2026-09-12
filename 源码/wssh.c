/*
 * wssh.c — WebSocket Shell（wssh）
 * GET / 或 /wssh.html → 静态页；/xterm.js、/xterm-fit.js → 本地前端库；
 * GET Upgrade: websocket → PTY 桥接交互 shell（xterm.js 前端，窗口自适应）。
 * 静态文件白名单：STATIC_FILES[]；父进程先查白名单，命中才 fork。
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <signal.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 00400000
#endif
#ifndef O_NOCTTY
#define O_NOCTTY   00000400
#endif
#ifndef TIOCGPTN
#define TIOCGPTN   0x80045430
#endif
#ifndef TIOCSPTLCK
#define TIOCSPTLCK 0x40045431
#endif
#ifndef TIOCSCTTY
#define TIOCSCTTY  0x540E
#endif
#ifndef TIOCSWINSZ
#define TIOCSWINSZ 0x5414        /* asm-generic/ioctls.h：0x5414（0x5413 是 TIOCGWINSZ） */
#endif

#define WS_GUID    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define PORT         2333
#define BIND_IP      "192.168.0.1"
#define ALLOW_NET    0xC0A80000u
#define ALLOW_MASK   0xFFFFFF00u
#define MAX_CHILDREN 3
#define MAX_FD_CAP   65536
#define HEAD_MAX     (16 * 1024)
#define GET_MAX_SIZE (1024 * 1024)
#define IO_TIMEOUT_SEC 5
#define TOTAL_TIMEOUT_SEC 10
#define SESSION_IDLE_SEC 300
#define SESSION_MAX_SEC  1800
#define SESS_CPU_SEC      60
#define SESS_FSIZE        (16 << 20)

typedef struct {
    const char *url;
    const char *path;
    const char *ctype;
    const char *cache;   /* NULL = no-store；否则如 "max-age=31536000" */
} sfile_t;

static const sfile_t STATIC_FILES[] = {
    {"/",            "/etc_ro/web/wssh.html", "text/html; charset=utf-8", NULL},
    {"/wssh.html",   "/etc_ro/web/wssh.html", "text/html; charset=utf-8", NULL},
    {"/favicon.ico", "/etc_ro/web/favicon.ico", "image/x-icon", "max-age=31536000"},
    {"/xterm.js",    "/etc_ro/web/js/xterm.min.js", "application/javascript", "max-age=31536000"},
    {"/xterm-fit.js","/etc_ro/web/js/xterm-addon-fit.min.js", "application/javascript", "max-age=31536000"},
    {"/xterm.css",   "/etc_ro/web/css/xterm.min.css", "text/css", "max-age=31536000"},
    {NULL, NULL, NULL, NULL}
};

static int active_children = 0;
static char hbuf[HEAD_MAX];

/* ---------------- 基础工具 ---------------- */

static int setup_signals(void)
{
    struct sigaction sa;

    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    return sigaction(SIGPIPE, &sa, NULL);
}

static void reap_children(void)
{
    int st;
    while (waitpid(-1, &st, WNOHANG) > 0) {
        if (active_children > 0)
            active_children--;
    }
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int recv_exact(int fd, void *buf, size_t n)
{
    size_t got = 0;
    unsigned char *p = buf;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static ssize_t find_head_end(const char *p, ssize_t n, ssize_t from)
{
    ssize_t i;
    if (from < 0) from = 0;
    for (i = from; i + 3 < n; i++)
        if (memcmp(p + i, "\r\n\r\n", 4) == 0)
            return i + 4;
    for (i = from; i + 1 < n; i++)
        if (p[i] == '\n' && p[i + 1] == '\n')
            return i + 2;
    return -1;
}

static ssize_t read_headers(int c, ssize_t *total, time_t deadline)
{
    ssize_t filled = 0, last = 0, endl, n;

    for (;;) {
        if (filled >= HEAD_MAX) return -2;
        if (time(NULL) > deadline) return -1;
        n = recv(c, hbuf + filled, (size_t)(HEAD_MAX - filled), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        last = filled;
        filled += n;
        endl = find_head_end(hbuf, filled, last - 3);
        if (endl >= 0) {
            *total = filled;
            return endl;
        }
    }
}

static int get_hdr(const char *p, ssize_t n, const char *name, char *val, size_t vsz)
{
    ssize_t st, e, a, b, k, m;
    size_t nlen = strlen(name);

    st = 0;
    while (st < n) {
        for (e = st; e < n && p[e] != '\n'; e++)
            ;
        a = st; b = e;
        if (b > a && p[b - 1] == '\r') b--;
        if (b - a >= (ssize_t)nlen + 1 &&
            strncasecmp(p + a, name, nlen) == 0 && p[a + nlen] == ':') {
            k = a + nlen + 1;
            while (k < b && (p[k] == ' ' || p[k] == '\t')) k++;
            m = 0;
            while (k < b && m + 1 < (ssize_t)vsz) val[m++] = p[k++];
            val[m] = '\0';
            return (int)m;
        }
        if (e >= n) break;
        st = e + 1;
    }
    if (vsz > 0) val[0] = '\0';
    return -1;
}

/* 长度感知的 ci 包含判断：hn 由调用方保证，不依赖 NUL */
static int ci_has_n(const char *hay, size_t hn, const char *need)
{
    size_t i, j, nn = strlen(need);

    for (i = 0; i + nn <= hn; i++) {
        int ok = 1;
        for (j = 0; j < nn; j++) {
            char a = hay[i + j], b = need[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

/* NUL 语义包装：仅用于已保证 '\0' 结尾的字符串（如 get_hdr 结果） */
static int ci_has(const char *hay, const char *need)
{
    return ci_has_n(hay, strlen(hay), need);
}

static void http_error(int c, const char *status)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %s\r\n"
                     "Server: wssh\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "Connection: close\r\n"
                     "Content-Length: 0\r\n"
                     "\r\n", status);
    if (n < 0) return;
    if ((size_t)n >= sizeof hdr)
        n = (int)sizeof hdr - 1;
    write_all(c, hdr, (size_t)n);
}

static int send_head(int c, const char *ctype, const char *cache, long size)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\n"
                     "Server: wssh\r\n"
                     "Content-Type: %s\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "Cache-Control: %s\r\n"
                     "Content-Length: %ld\r\n"
                     "Connection: close\r\n"
                     "\r\n", ctype, cache ? cache : "no-store", size);
    if (n < 0) return -1;
    if ((size_t)n >= sizeof hdr)
        n = (int)sizeof hdr - 1;
    return write_all(c, hdr, (size_t)n);
}

/* 白名单精确匹配；未命中返回 NULL */
static const sfile_t *static_lookup(const char *req_path, size_t req_pathlen)
{
    size_t i, ulen;

    for (i = 0; STATIC_FILES[i].url; i++) {
        ulen = strlen(STATIC_FILES[i].url);
        if (ulen == req_pathlen && memcmp(STATIC_FILES[i].url, req_path, ulen) == 0)
            return &STATIC_FILES[i];
    }
    return NULL;
}

static void serve_file(int c, const sfile_t *f)
{
    int fd;
    struct stat st;
    char iobuf[4096];
    ssize_t r;

    fd = open(f->path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) { http_error(c, "404 Not Found"); return; }
    if (fstat(fd, &st) < 0) { close(fd); http_error(c, "500 Internal Server Error"); return; }
    if (st.st_size > GET_MAX_SIZE) { close(fd); http_error(c, "500 Internal Server Error"); return; }
    if (send_head(c, f->ctype, f->cache, (long)st.st_size) < 0) { close(fd); return; }
    while ((r = read(fd, iobuf, sizeof iobuf)) > 0) {
        if (write_all(c, iobuf, (size_t)r) < 0)
            break;
    }
    close(fd);
}

/* ---------------- SHA-1 / Base64 ---------------- */

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    unsigned char buf[64];
} sha1_ctx;

static uint32_t rol32(uint32_t x, int n)
{
    return (uint32_t)((x << n) | (x >> (32 - n)));
}

static void sha1_blk(uint32_t s[5], const unsigned char b[64])
{
    uint32_t w[80];
    uint32_t a, bb, c, d, e, f, k, t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[i*4] << 24) | ((uint32_t)b[i*4+1] << 16) |
               ((uint32_t)b[i*4+2] << 8)  |  (uint32_t)b[i*4+3];
    for (i = 16; i < 80; i++)
        w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    a = s[0]; bb = s[1]; c = s[2]; d = s[3]; e = s[4];
    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (bb & c) | ((~bb) & d); k = 0x5A827999u; }
        else if (i < 40) { f = bb ^ c ^ d;              k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = bb ^ c ^ d;              k = 0xCA62C1D6u; }
        t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(bb, 30); bb = a; a = t;
    }
    s[0] += a; s[1] += bb; s[2] += c; s[3] += d; s[4] += e;
}

static void sha1_init(sha1_ctx *x)
{
    x->state[0] = 0x67452301u;
    x->state[1] = 0xEFCDAB89u;
    x->state[2] = 0x98BADCFEu;
    x->state[3] = 0x10325476u;
    x->state[4] = 0xC3D2E1F0u;
    x->count[0] = x->count[1] = 0;
}

static void sha1_upd(sha1_ctx *x, const unsigned char *p, size_t n)
{
    size_t i = (size_t)((x->count[0] >> 3) & 63);
    uint32_t v = (uint32_t)(n << 3);

    if ((x->count[0] + v) < x->count[0])
        x->count[1]++;
    x->count[0] += v;
    x->count[1] += (uint32_t)(n >> 29);

    if (i) {
        size_t need = 64 - i;
        if (n < need) { memcpy(x->buf + i, p, n); return; }
        memcpy(x->buf + i, p, need);
        sha1_blk(x->state, x->buf);
        p += need;
        n -= need;
    }
    while (n >= 64) {
        sha1_blk(x->state, p);
        p += 64;
        n -= 64;
    }
    if (n)
        memcpy(x->buf, p, n);
}

static void sha1_fin(sha1_ctx *x, unsigned char out[20])
{
    unsigned char data[72];
    uint64_t bits;
    size_t i = (size_t)((x->count[0] >> 3) & 63);
    size_t padlen = (i < 56) ? (56 - i) : (120 - i);
    int j;

    memset(data, 0, sizeof data);
    data[0] = 0x80;
    bits = ((uint64_t)x->count[1] << 32) | (uint64_t)x->count[0];
    for (j = 0; j < 8; j++)
        data[padlen + 7 - j] = (unsigned char)(bits >> (j * 8));
    sha1_upd(x, data, padlen + 8);
    for (j = 0; j < 5; j++) {
        out[j*4]   = (unsigned char)(x->state[j] >> 24);
        out[j*4+1] = (unsigned char)(x->state[j] >> 16);
        out[j*4+2] = (unsigned char)(x->state[j] >> 8);
        out[j*4+3] = (unsigned char)(x->state[j]);
    }
}

static const char B64_TAB[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_enc(const unsigned char *in, size_t inlen, char *out, size_t outsz)
{
    size_t i, o = 0;
    if (outsz < ((inlen + 2) / 3) * 4 + 1)
        return -1;
    for (i = 0; i + 2 < inlen; i += 3) {
        out[o++] = B64_TAB[(in[i] >> 2) & 63];
        out[o++] = B64_TAB[((in[i] & 3) << 4) | (in[i+1] >> 4)];
        out[o++] = B64_TAB[((in[i+1] & 15) << 2) | (in[i+2] >> 6)];
        out[o++] = B64_TAB[in[i+2] & 63];
    }
    if (i < inlen) {
        out[o++] = B64_TAB[(in[i] >> 2) & 63];
        if (i + 1 < inlen) {
            out[o++] = B64_TAB[((in[i] & 3) << 4) | (in[i+1] >> 4)];
            out[o++] = B64_TAB[(in[i+1] & 15) << 2];
        } else {
            out[o++] = B64_TAB[(in[i] & 3) << 4];
        }
        while (o & 3)
            out[o++] = '=';
    }
    out[o] = '\0';
    return (int)o;
}

/* ---------------- WebSocket ---------------- */

static int ws_handshake(int c, ssize_t hlen)
{
    char key[256], up[32], conn[256], acc[64], rsp[512];
    unsigned char sha[20];
    sha1_ctx ctx;
    size_t klen;
    int kl, n;

    if (hlen < 5 || memcmp(hbuf, "GET ", 4) != 0)
        return -1;
    if (get_hdr(hbuf, hlen, "Upgrade", up, sizeof up) < 0 ||
        strcasecmp(up, "websocket") != 0)
        return -1;
    if (get_hdr(hbuf, hlen, "Connection", conn, sizeof conn) < 0 ||
        !ci_has(conn, "upgrade"))        /* conn 已 NUL 结尾，走包装即可 */
        return -1;
    kl = get_hdr(hbuf, hlen, "sec-websocket-key", key, sizeof key);
    klen = (kl > 0) ? (size_t)kl : 0;
    if (klen != 24 || key[22] != '=' || key[23] != '=')
        return -1;

    sha1_init(&ctx);
    sha1_upd(&ctx, (const unsigned char *)key, klen);
    sha1_upd(&ctx, (const unsigned char *)WS_GUID, sizeof(WS_GUID) - 1);
    sha1_fin(&ctx, sha);
    if (b64_enc(sha, sizeof sha, acc, sizeof acc) < 0)
        return -1;

    n = snprintf(rsp, sizeof rsp,
                 "HTTP/1.1 101 Switching Protocols\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: %s\r\n"
                 "\r\n", acc);
    if (n < 0 || (size_t)n >= sizeof rsp)
        return -1;
    if (write_all(c, rsp, (size_t)n) < 0)
        return -1;
    return 0;
}

static int ws_send(int fd, unsigned char first, const char *p, size_t n)
{
    unsigned char hdr[10];
    int h = 0, i;

    hdr[h++] = first;
    if (n < 126) {
        hdr[h++] = (unsigned char)n;
    } else if (n < 65536) {
        hdr[h++] = 126;
        hdr[h++] = (unsigned char)(n >> 8);
        hdr[h++] = (unsigned char)(n & 0xff);
    } else {
        uint64_t v = (uint64_t)n;
        hdr[h++] = 127;
        for (i = 7; i >= 0; i--)
            hdr[h++] = (unsigned char)(v >> (i * 8));
    }
    if (write_all(fd, hdr, (size_t)h) < 0)
        return -1;
    if (n > 0 && write_all(fd, p, n) < 0)
        return -1;
    return 0;
}

static int ws_read_msg(int fd, char *pay, size_t cap, unsigned char *op)
{
    unsigned char hdr[2], ext[8], mask[4];
    uint64_t len;
    int masked, i;

    if (recv_exact(fd, hdr, 2) < 0)
        return -1;
    *op = hdr[0] & 0x0f;
    len = hdr[1] & 0x7f;
    masked = (hdr[1] & 0x80) ? 1 : 0;
    if (!masked)
        return -1;

    if (len == 126) {
        if (recv_exact(fd, ext, 2) < 0) return -1;
        len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        if (recv_exact(fd, ext, 8) < 0) return -1;
        len = 0;
        for (i = 0; i < 8; i++)
            len = (len << 8) | ext[i];
    }

    if (recv_exact(fd, mask, 4) < 0) return -1;
    if (len > (uint64_t)cap)
        return -1;
    if (len == 0)
        return 0;
    if (recv_exact(fd, (unsigned char *)pay, (size_t)len) < 0)
        return -1;
    for (i = 0; i < (int)len; i++)
        pay[i] = (char)((unsigned char)pay[i] ^ mask[i & 3]);
    return (int)len;
}

/* ---------------- PTY + 会话 ---------------- */

static void pty_resize(int ptm, unsigned short cols, unsigned short rows)
{
    struct winsize w;
    w.ws_row = rows;
    w.ws_col = cols;
    w.ws_xpixel = 0;
    w.ws_ypixel = 0;
    ioctl(ptm, TIOCSWINSZ, &w);   /* 失败忽略 */
}

/* 极简 JSON 数值提取：找 "key": 后面的十进制数 */
static long json_num(const char *s, int n, const char *key)
{
    const char *p, *end = s + n, *q;
    size_t kl = strlen(key);
    char tmp[32];
    char *ep;
    long v;
    int i;

    for (p = s; (size_t)(end - p) >= kl; p++) {
        if (memcmp(p, key, kl) != 0)
            continue;
        q = p + kl;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        i = 0;
        while (q < end && i < (int)sizeof(tmp) - 1 && *q >= '0' && *q <= '9')
            tmp[i++] = *q++;
        tmp[i] = '\0';
        if (i == 0) continue;
        v = strtol(tmp, &ep, 10);
        if (*ep == '\0' && v > 0)
            return v;
    }
    return -1;
}

static int open_pty(int *master, int *slave)
{
    int m, s, n, r, zero = 0;
    char name[64];

    m = open("/dev/ptmx", O_RDWR);
    if (m < 0) return -1;
    r = ioctl(m, TIOCGPTN, &n);
    if (r < 0) { close(m); return -1; }
    ioctl(m, TIOCSPTLCK, &zero);   /* unlockpt 等价；≥3.8 内核需解锁否则 EACCES，本机 3.4 无此锁→ENOTTY 无害 */
    snprintf(name, sizeof name, "/dev/pts/%d", n);
    s = open(name, O_RDWR | O_NOCTTY);
    if (s < 0) { close(m); return -1; }
    *master = m;
    *slave = s;
    return 0;
}

static void ws_session(int c)
{
    int ptm = -1, pts = -1, ret, n, fl;
    pid_t shpid;
    struct pollfd fds[2];
    char iobuf[16384];
    unsigned char op;
    time_t start, now, idle, maxd;

    if (open_pty(&ptm, &pts) < 0) { close(c); return; }
    /* 初始尺寸，别让 sh 及其启动的工具读到 0×0 */
    {
        struct winsize wz;
        wz.ws_row = 24; wz.ws_col = 80; wz.ws_xpixel = 0; wz.ws_ypixel = 0;
        ioctl(ptm, TIOCSWINSZ, &wz);
    }

    fl = fcntl(ptm, F_GETFL);
    if (fl < 0 || fcntl(ptm, F_SETFL, fl | O_NONBLOCK) < 0) {
        close(ptm); close(pts); close(c); return;
    }

    shpid = fork();
    if (shpid == 0) {
        char *sh_argv[] = { "-sh", (char *)0 };
        char *sh_envp[] = {
            "TERM=xterm-256color",
            "PATH=/usr/sbin:/usr/bin:/sbin:/bin",
            "HOME=/",
            (char *)0
        };
        struct rlimit rl2;

        close(ptm);
        close(c);
        dup2(pts, 0);
        dup2(pts, 1);
        dup2(pts, 2);
        if (pts > 2) close(pts);
        if (setsid() < 0) _exit(125);
        if (ioctl(0, TIOCSCTTY, 0) < 0) _exit(126);
        if (chdir("/") < 0) _exit(124);     /* 固定登录后工作目录 */

        rl2.rlim_cur = rl2.rlim_max = SESS_CPU_SEC;
        setrlimit(RLIMIT_CPU, &rl2);
        rl2.rlim_cur = rl2.rlim_max = SESS_FSIZE;
        setrlimit(RLIMIT_FSIZE, &rl2);

        execve("/bin/sh", sh_argv, sh_envp);
        _exit(127);
    }
    if (shpid < 0) { close(ptm); close(pts); close(c); return; }
    close(pts);

    start = time(NULL);
    idle = start + SESSION_IDLE_SEC;
    maxd = start + SESSION_MAX_SEC;

    fds[0].fd = c;    fds[0].events = POLLIN | POLLHUP;
    fds[1].fd = ptm;  fds[1].events = POLLIN | POLLHUP;

    for (;;) {
        ret = poll(fds, 2, 1000);
        now = time(NULL);
        if (now >= maxd) break;
        if (now >= idle) break;
        if (ret < 0) { if (errno == EINTR) continue; break; }
        if (ret == 0) continue;

        if (fds[0].revents & POLLIN) {
            n = ws_read_msg(c, iobuf, sizeof iobuf, &op);
            if (n < 0) break;
            if (op == 0x8) { ws_send(c, 0x88, "", 0); break; }
            if (op == 0x9) { ws_send(c, 0x8A, iobuf, (size_t)n); idle = now + SESSION_IDLE_SEC; continue; }
            if (op == 0x1 && n > 0 && iobuf[0] == '{') {
                long cols = json_num(iobuf, n, "\"cols\":");
                long rows = json_num(iobuf, n, "\"rows\":");
                if (cols > 0 && rows > 0 && cols < 500 && rows < 500 &&
                    ci_has_n(iobuf, (size_t)n, "\"type\":\"resize\"")) {
                    pty_resize(ptm, (unsigned short)cols, (unsigned short)rows);
                    idle = now + SESSION_IDLE_SEC;
                    continue;                 /* 确认 resize 控制帧才拦下 */
                }
                /* 否则是普通以 { 开头的输入，落入下方写 pty */
            }
            if ((op == 0x1 || op == 0x2) && n > 0) {
                (void)write_all(ptm, iobuf, (size_t)n);   /* EAGAIN 丢键不终止会话 */
                idle = now + SESSION_IDLE_SEC;
            }
        }
        if (fds[0].revents & (POLLHUP | POLLERR)) break;

        if (fds[1].revents & POLLIN) {
            n = read(ptm, iobuf, sizeof iobuf);
            if (n > 0) {
                ws_send(c, 0x82, iobuf, (size_t)n);
                idle = now + SESSION_IDLE_SEC;
            } else if (n == 0) {
                break;
            } else if (errno != EAGAIN) {
                break;
            }
        }
        if (fds[1].revents & (POLLHUP | POLLERR)) break;
    }

    kill(-shpid, SIGKILL);
    close(ptm);
    close(c);
    waitpid(shpid, NULL, 0);
}

int main(void)
{
    int s, c, one = 1, ws;
    struct sockaddr_in sa = {0};
    struct sockaddr_in peer;
    socklen_t plen;
    struct timeval tv;
    uint32_t ip;
    ssize_t hlen, total;
    time_t t0, deadline;
    pid_t pid;

    if (setup_signals() < 0) return 5;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 1;

    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) {
        close(s); return 6;
    }

    sa.sin_family = AF_INET;
    sa.sin_port   = htons(PORT);
    if (inet_pton(AF_INET, BIND_IP, &sa.sin_addr) != 1) {
        close(s); return 4;
    }

    if (bind(s, (struct sockaddr *)&sa, sizeof sa) < 0) { close(s); return 7; }
    if (listen(s, SOMAXCONN) < 0) { close(s); return 3; }

    for (;;) {
        int ka = 1;
        char up[32];

        plen = sizeof peer;
        c = accept(s, (struct sockaddr *)&peer, &plen);
        if (c < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            sleep(1);
            continue;
        }

        ip = ntohl(peer.sin_addr.s_addr);
        if ((ip & ALLOW_MASK) != ALLOW_NET) { close(c); continue; }

        reap_children();
        if (active_children >= MAX_CHILDREN) { close(c); sleep(1); continue; }

        setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof ka);

        tv.tv_sec = IO_TIMEOUT_SEC; tv.tv_usec = 0;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

        t0 = time(NULL);
        deadline = t0 + TOTAL_TIMEOUT_SEC;

        hlen = read_headers(c, &total, deadline);
        if (hlen == -2) { http_error(c, "431 Request Header Fields Too Large"); close(c); continue; }
        if (hlen < 0)   { close(c); continue; }

        ws = 0;
        if (get_hdr(hbuf, hlen, "Upgrade", up, sizeof up) >= 0 &&
            strcasecmp(up, "websocket") == 0)
            ws = 1;

        if (ws) {
            pid = fork();
            if (pid < 0) { http_error(c, "500 Internal Server Error"); close(c); sleep(1); continue; }
            if (pid == 0) {
                close(s);
                if (ws_handshake(c, hlen) < 0) { close(c); _exit(0); }
                ws_session(c);
                _exit(0);
            }
            active_children++;
            close(c);
            continue;
        }

        if (hlen >= 4 && memcmp(hbuf, "GET ", 4) == 0) {
            const char *path = hbuf + 4;
            const char *sp   = memchr(path, ' ', (size_t)(hlen - 4));
            const char *qm;
            size_t pathlen = sp ? (size_t)(sp - path) : 0;
            const sfile_t *sf;

            if (pathlen > 0) {
                qm = memchr(path, '?', pathlen);
                if (qm) pathlen = (size_t)(qm - path);
            }

            sf = static_lookup(path, pathlen);
            if (sf == NULL) { http_error(c, "404 Not Found"); close(c); continue; }

            pid = fork();
            if (pid == 0) { close(s); serve_file(c, sf); _exit(0); }
            if (pid > 0)  { active_children++; }
            else          { http_error(c, "500 Internal Server Error"); }
            close(c);
            continue;
        }

        http_error(c, "404 Not Found");
        close(c);
        continue;
    }
    return 0;
}
