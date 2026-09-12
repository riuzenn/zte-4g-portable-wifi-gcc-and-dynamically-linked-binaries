/*
 * websh.c
 * HTTP 命令执行服务：POST body = 一段 shell 脚本，stdout 原样回传。
 *
 * POST 流程：accept → 读头(≤16KB/10s) → fork → 子进程流式收 body 到
 * mkstemp 临时文件 → dup2 到 stdin → 资源限额 → execve sh。
 * 父进程 fork 后立即回 accept，绝不被 body 传输阻塞。
 *
 * 墙钟兜底（双保险）：
 *   - alarm(120s) 杀 sh 本体（内建/死循环）；
 *   - 看门狗孙进程用 pipe 监测会话存活：
 *       写端 wp[1] 不设 CLOEXEC → sh 及所有后代继承；
 *       最后一个后代退出 → 管道 EOF → 看门狗 poll 返回 HUP → 补刀 kill(-gpid)
 *       （清掉“关了监控 fd 仍活着的组内残留”，空组等价无操作）→ _exit；
 *       122 秒仍有后代活着 → poll 超时 → kill(-gpid) 端掉整组。
 *
 * 已知边界：命令里显式 setsid 的进程会离开会话，组杀不可达——属所有
 * “按进程组清理”方案的固有边界。
 *
 * 状态码：405 非 POST / 400 坏请求 / 411 缺 Content-Length /
 *         413 body 超限 / 431 请求头超限 / 500 内部错误 / 200 成功
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/stat.h>
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
/* uClibc 0.9.33.2 未定义 _GNU_SOURCE 时不暴露 O_NOFOLLOW（GNU 扩展）。
 * 按 linux asm-generic/fcntl.h 的取值兜底：00400000（八进制）= 0x20000。 */
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x20000
#endif

#define WWW_PAGE        "/etc_ro/web/websh.html"
#define TMP_TEMPLATE    "/tmp/webshXXXXXX"

#define PORT            2333
#define BIND_IP         "192.168.0.1"
#define ALLOW_NET       0xC0A80000u    /* 192.168.0.0/24 */
#define ALLOW_MASK      0xFFFFFF00u
#define MAX_CHILDREN    3
#define MAX_FD_CAP      65536
#define MAX_BODY        (64 * 1024)
#define HEAD_MAX        (16 * 1024)
#define GET_MAX_SIZE    (1024 * 1024)
#define IO_TIMEOUT_SEC  5
#define TOTAL_TIMEOUT_SEC 10
#define CHILD_CPU_LIMIT  30
#define CHILD_FSIZE_LIM  (4 << 20)
#define CHILD_WALL_LIMIT 120

static const char OK_HDR[] =
    "HTTP/1.1 200 OK\r\n"
    "Server: websh\r\n"
    "Cache-Control: no-store\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "Connection: close\r\n"
    "\r\n";

static int active_children = 0;
static char hbuf[HEAD_MAX];

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

static int parse_head(const char *p, ssize_t n, long *clen)
{
    ssize_t st, e, a, b, k, m;
    int seen_cl = 0;
    char tmp[32];
    char *endp;
    long v;

    *clen = -1;
    if (n < 5 || strncmp(p, "POST ", 5) != 0)
        return -1;

    st = 0;
    while (st < n) {
        for (e = st; e < n && p[e] != '\n'; e++)
            ;
        a = st; b = e;
        if (b > a && p[b - 1] == '\r') b--;
        if (b - a >= 15 && strncasecmp(p + a, "content-length:", 15) == 0) {
            if (seen_cl) return -2;
            seen_cl = 1;
            k = a + 15;
            while (k < b && (p[k] == ' ' || p[k] == '\t')) k++;
            m = 0;
            while (k < b && m < (ssize_t)sizeof(tmp) - 1) tmp[m++] = p[k++];
            tmp[m] = '\0';
            if (m == 0) return -2;
            errno = 0;
            v = strtol(tmp, &endp, 10);
            if (errno == ERANGE || *endp != '\0' || v < 0)
                return -2;
            *clen = v;
        }
        if (e >= n) break;
        st = e + 1;
    }
    return 0;
}

static void http_error(int c, const char *status)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %s\r\n"
                     "Server: websh\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "Connection: close\r\n"
                     "Content-Length: 0\r\n"
                     "\r\n", status);
    if (n < 0) return;
    if ((size_t)n >= sizeof hdr)
        n = (int)sizeof hdr - 1;
    write_all(c, hdr, (size_t)n);
}

static void handle_get(int c)
{
    int f;
    struct stat st;
    char hdr[512];
    char iobuf[4096];
    ssize_t r;
    int n;

    f = open(WWW_PAGE, O_RDONLY | O_NOFOLLOW);
    if (f < 0) {
        http_error(c, "404 Not Found");
        return;
    }
    if (fstat(f, &st) < 0) {
        close(f);
        http_error(c, "500 Internal Server Error");
        return;
    }
    if (st.st_size > GET_MAX_SIZE) {
        close(f);
        http_error(c, "500 Internal Server Error");
        return;
    }

    n = snprintf(hdr, sizeof hdr,
                 "HTTP/1.1 200 OK\r\n"
                 "Server: websh\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "X-Content-Type-Options: nosniff\r\n"
                 "Cache-Control: no-store\r\n"
                 "Content-Length: %ld\r\n"
                 "Connection: close\r\n"
                 "\r\n", (long)st.st_size);
    if (n < 0) {
        close(f);
        return;
    }
    if ((size_t)n >= sizeof hdr)
        n = (int)sizeof hdr - 1;
    if (write_all(c, hdr, (size_t)n) < 0) {
        close(f);
        return;
    }

    while ((r = read(f, iobuf, sizeof iobuf)) > 0) {
        if (write_all(c, iobuf, (size_t)r) < 0)
            break;
    }
    close(f);
}

int main(void)
{
    int s, c, one = 1, fd, pr, ok, tf;
    struct rlimit rl;
    rlim_t maxfd;
    struct sockaddr_in sa = {0};
    struct sockaddr_in peer;
    socklen_t plen;
    struct timeval tv;
    uint32_t ip;
    long clen;
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

        /* —— GET：服务 websh.html，交给子进程 —— */
        if (hlen >= 4 && memcmp(hbuf, "GET ", 4) == 0) {
            const char *path = hbuf + 4;
            const char *sp   = memchr(path, ' ', (size_t)(hlen - 4));
            size_t pathlen   = sp ? (size_t)(sp - path) : 0;

            ok = (pathlen == 11 && memcmp(path, "/websh.html", 11) == 0) ||
                 (pathlen == 1  && path[0] == '/');
            if (!ok) { http_error(c, "404 Not Found"); close(c); continue; }

            pid = fork();
            if (pid == 0) {
                close(s);
                handle_get(c);
                _exit(0);
            }
            if (pid > 0) {
                active_children++;
            } else {
                http_error(c, "500 Internal Server Error");
                close(c);
                sleep(1);
                continue;
            }
            close(c);
            continue;
        }

        /* —— POST：fork 前置，body 收取与喂 sh 全在子进程 —— */
        pr = parse_head(hbuf, hlen, &clen);
        if (pr == -1) { http_error(c, "405 Method Not Allowed"); close(c); continue; }
        if (pr == -2) { http_error(c, "400 Bad Request");        close(c); continue; }
        if (clen < 0)        { http_error(c, "411 Length Required"); close(c); continue; }
        if (clen == 0)       { http_error(c, "400 Bad Request");      close(c); continue; }
        if (clen > MAX_BODY) { http_error(c, "413 Request Entity Too Large"); close(c); continue; }

        pid = fork();
        if (pid < 0) { http_error(c, "500 Internal Server Error"); close(c); sleep(1); continue; }

        if (pid == 0) {
            char tmpl[] = TMP_TEMPLATE;
            char *sh_argv[] = { "sh", (char *)0 };
            char *sh_envp[] = { (char *)0 };
            char iobuf[4096];
            long have;
            ssize_t prefix, n;

            close(s);

            /* 1) 流式暂存 body */
            tf = mkstemp(tmpl);
            if (tf < 0) _exit(124);

            prefix = total - hlen;
            if (prefix > clen) prefix = (ssize_t)clen;
            if (prefix > 0) {
                if (write_all(tf, hbuf + hlen, (size_t)prefix) < 0) {
                    unlink(tmpl); _exit(124);
                }
            }

            have = (long)prefix;
            while (have < clen) {
                long want = clen - have;
                if (time(NULL) > deadline) { unlink(tmpl); _exit(0); }
                n = recv(c, iobuf,
                         (size_t)(want < (long)sizeof iobuf ? want : (long)sizeof iobuf), 0);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    unlink(tmpl); _exit(0);
                }
                if (n == 0) { unlink(tmpl); _exit(0); }
                if (write_all(tf, iobuf, (size_t)n) < 0) { unlink(tmpl); _exit(124); }
                have += (long)n;
            }
            if (lseek(tf, 0, SEEK_SET) < 0) { unlink(tmpl); _exit(124); }

            /* 2) 三流接管 */
            if (dup2(c, 1) < 0 || dup2(c, 2) < 0 || dup2(tf, 0) < 0) {
                unlink(tmpl); _exit(126);
            }
            close(tf);
            unlink(tmpl);

            /* 3) 资源限额 */
            rl.rlim_cur = rl.rlim_max = CHILD_CPU_LIMIT;
            setrlimit(RLIMIT_CPU, &rl);
            rl.rlim_cur = rl.rlim_max = CHILD_FSIZE_LIM;
            setrlimit(RLIMIT_FSIZE, &rl);

            /* 4) 关多余 fd（pipe 在之后创建，不受影响） */
            if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
                maxfd = rl.rlim_cur;
            else
                maxfd = 1024;
            if (maxfd > MAX_FD_CAP) maxfd = MAX_FD_CAP;
            for (fd = 3; (rlim_t)fd < maxfd; fd++)
                close(fd);

            if (setsid() < 0) _exit(125);
            if (chdir("/") < 0) _exit(128);

            /* 5) 看门狗孙进程 + 会话存活管道 */
            {
                int wp[2];
                pid_t gpid, wdog;

                gpid = getpid();
                if (pipe(wp) == 0) {
                    wdog = fork();
                    if (wdog == 0) {
                        struct pollfd pfd;
                        int pret;
                        char cbuf[1];
                        ssize_t rn;

                        close(0); close(1); close(2);
                        close(wp[1]);                    /* 不持写端 */
                        pfd.fd = wp[0];
                        pfd.events = POLLIN;
                        for (;;) {
                            pret = poll(&pfd, 1,
                                        (int)((CHILD_WALL_LIMIT + 2) * 1000));
                            if (pret == 0) {
                                kill(-gpid, SIGKILL);
                                _exit(0);
                            }
                            if (pret < 0) {
                                if (errno == EINTR) continue;
                                _exit(0);
                            }
                            if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
                                rn = read(wp[0], cbuf, 1);
                                if (rn == 0) {
                                    /* EOF 不等于组一定空：daemonize 类程序会
                                     * 关掉继承 fd，提前放掉写端。补一记组杀，
                                     * 空组=只杀自己（等价无操作），有残留正好兜住 */
                                    kill(-gpid, SIGKILL);
                                    _exit(0);
                                }
                                if (rn < 0) {
                                    if (errno == EINTR) continue;
                                    _exit(0);
                                }
                            }
                        }
                    }
                    close(wp[0]);
                    if (wdog < 0) close(wp[1]);   /* fork 失败别把死写端带进 sh */
                }
            }

            /* 6) 一切就绪才发 200 */
            if (write_all(1, OK_HDR, sizeof(OK_HDR) - 1) < 0) _exit(0);

            /* 7) 墙钟双保险：alarm 杀 sh 本体，看门狗杀全会话 */
            alarm(CHILD_WALL_LIMIT);
            execve("/bin/sh", sh_argv, sh_envp);
            _exit(127);
        }

        active_children++;
        close(c);
        continue;
    }
    return 0;
}
