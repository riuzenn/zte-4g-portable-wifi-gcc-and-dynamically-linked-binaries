/*
 * gal.c -- GoAhead Launcher（精简版，无日志）
 * 绑定 192.168.0.1:80，只认 GET / POST；命中后：
 *   1) 给触发它的这次请求回一个 503（正常 FIN 关闭，不 RST 硬砍）；
 *   2) 子进程等 80 端口空出后 execlp("goahead")（从 PATH 查找）。
 */
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define LISTEN_IP   0xC0A80001UL   /* 192.168.0.1 */
#define LISTEN_PORT 80

int main(void)
{
    int lfd, cfd, one = 1, i, t, fd;
    struct sockaddr_in sa;
    struct pollfd pfd;
    struct linger lg;
    char b[8];
    ssize_t n;
    pid_t pid;

    // signal(SIGCHLD, SIG_IGN);   /* 可删：父进程 fork 后立即自杀，无僵尸可收 */

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    fcntl(lfd, F_SETFD, FD_CLOEXEC);   /* exec 时绝不把 80 端口带进 goahead */

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(LISTEN_PORT);
    sa.sin_addr.s_addr = htonl(LISTEN_IP);

    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) return 1;  /* 静默退出 */
    listen(lfd, 8);

    for (;;) {
        cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) { usleep(50000); continue; }

        /* 非触发连接关掉时不留 TIME_WAIT；触发那条例外，命中后会恢复成正常关闭 */
        lg.l_onoff  = 1;
        lg.l_linger = 0;
        setsockopt(cfd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        fcntl(cfd, F_SETFD, FD_CLOEXEC);

        pfd.fd      = cfd;
        pfd.events  = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 400) <= 0) { close(cfd); continue; }

        n = recv(cfd, b, 8, 0);
        if (n < 4) { close(cfd); continue; }

        /* 只认 GET 和 POST */
        if (memcmp(b, "GET ", 4) != 0 &&
            (n < 5 || memcmp(b, "POST ", 5) != 0)) {
            close(cfd);
            continue;
        }

        /* 命中：fork 一个"拉手"，gal 本体自杀 */
        pid = fork();
        if (pid < 0) { close(cfd); continue; }

        if (pid == 0) {
            /* 子进程：等 80 空出后拉起 goahead（相当于 goahead &） */
            setsid();   /* 脱离控制终端，防止 SIGHUP 带走 goahead */

            /* 本程序最多 5 个 fd，关 3..63 足够覆盖 */
            for (i = 3; i < 64; i++) close(i);

            fd = open("/dev/null", O_RDWR);
            if (fd >= 0) {
                dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
                if (fd > 2) close(fd);
            }

            /* 试绑 192.168.0.1:80，能绑上 = gal 已死、端口已空，再拉起。
             * SO_REUSEADDR 用来容下 503 正常关闭留下的 TIME_WAIT。 */
            for (i = 0; i < 60; i++) {           /* 最多 ~3s */
                t = socket(AF_INET, SOCK_STREAM, 0);
                if (t >= 0) {
                    setsockopt(t, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                    if (bind(t, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
                        close(t);
                        break;
                    }
                    close(t);
                }
                usleep(50000);
            }

            execlp("goahead", "goahead", (char *)0);
            _exit(127);
        }

        /* ---- 父进程 = gal 本体：体面地回完 503，再自杀 ---- */
        {
            static const char resp[] =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Connection: close\r\n"
                "Content-Type: text/html\r\n"
                "\r\n"
                "<html><head><meta http-equiv=\"refresh\" content=\"3;url=/\">"
                "</head><body>Starting web service...</body></html>";

            /* accept 后设过 SO_LINGER(1,0)，必须先关掉，否则 close 会把 503 吞掉再 RST */
            lg.l_onoff  = 0;
            lg.l_linger = 0;
            setsockopt(cfd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));

            write(cfd, resp, sizeof(resp) - 1);
            shutdown(cfd, SHUT_WR);   /* 数据发完，先 FIN */
            close(cfd);
            close(lfd);               /* 显式放掉 80，端口交接更确定 */
        }

        kill(getpid(), SIGKILL);
        _exit(1);   /* 极端安全网；SIGKILL 下通常走不到 */
    }
}
