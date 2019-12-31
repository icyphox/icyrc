#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include <curses.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <locale.h>
#include <wchar.h>
#include <openssl/ssl.h>

#undef CTRL
#define CTRL(x)  (x & 037)

#include "config.h"

enum {
    ChanLen = 64,
    LineLen = 512,
    MaxChans = 16,
    BufSz = 2048,
    LogSz = 4096,
    MaxRecons = 10, /* -1 for infinitely many */
    UtfSz = 4,
    RuneInvalid = 0xFFFD,
};

typedef wchar_t Rune;

static struct {
    int x;
    int y;
    WINDOW *sw, *mw, *iw;
} scr;

static struct Chan {
    char name[ChanLen];
    char *buf, *eol;
    int n;     /* Scroll offset. */
    size_t sz; /* Size of buf. */
    char high; /* Nick highlight. */
    char new;  /* New message. */
    char join; /* Channel was 'j'-oined. */
} chl[MaxChans];

static int ssl;
static struct {
    int fd;
    SSL *ssl;
    SSL_CTX *ctx;
} srv;
static char nick[64];
static int quit, winchg;
static int nch, ch; /* Current number of channels, and current channel. */
static char outb[BufSz], *outp = outb; /* Output buffer. */
static FILE *logfp;

static unsigned char utfbyte[UtfSz + 1] = {0x80,    0, 0xC0, 0xE0, 0xF0};
static unsigned char utfmask[UtfSz + 1] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
static Rune utfmin[UtfSz + 1] = {       0,    0,  0x80,  0x800,  0x10000};
static Rune utfmax[UtfSz + 1] = {0x10FFFF, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};

static void scmd(char *, char *, char *, char *);
static void tdrawbar(void);
static void tredraw(void);
static void treset(void);

static void
panic(const char *m)
{
    treset();
    fprintf(stderr, "Panic: %s\n", m);
    exit(1);
}

static size_t
utf8validate(Rune *u, size_t i)
{
    if (*u < utfmin[i] || *u > utfmax[i] || (0xD800 <= *u && *u <= 0xDFFF))
        *u = RuneInvalid;
    for (i = 1; *u > utfmax[i]; ++i)
        ;
    return i;
}

static Rune
utf8decodebyte(unsigned char c, size_t *i)
{
    for (*i = 0; *i < UtfSz + 1; ++(*i))
        if ((c & utfmask[*i]) == utfbyte[*i])
            return c & ~utfmask[*i];
    return 0;
}

static size_t
utf8decode(char *c, Rune *u, size_t clen)
{
    size_t i, j, len, type;
    Rune udecoded;

    *u = RuneInvalid;
    if (!clen)
        return 0;
    udecoded = utf8decodebyte(c[0], &len);
    if (len < 1 || len > UtfSz)
        return 1;
    for (i = 1, j = 1; i < clen && j < len; ++i, ++j) {
        udecoded = (udecoded << 6) | utf8decodebyte(c[i], &type);
        if (type != 0)
            return j;
    }
    if (j < len)
        return 0;
    *u = udecoded;
    utf8validate(u, len);
    return len;
}

static char
utf8encodebyte(Rune u, size_t i)
{
    return utfbyte[i] | (u & ~utfmask[i]);
}

static size_t
utf8encode(Rune u, char *c)
{
    size_t len, i;

    len = utf8validate(&u, 0);
    if (len > UtfSz)
        return 0;
    for (i = len - 1; i != 0; --i) {
        c[i] = utf8encodebyte(u, 0);
        u >>= 6;
    }
    c[0] = utf8encodebyte(u, len);
    return len;
}

static void
sndf(const char *fmt, ...)
{
    va_list vl;
    size_t n, l = BufSz - (outp - outb);

    if (l < 2)
        return;
    va_start(vl, fmt);
    n = vsnprintf(outp, l - 2, fmt, vl);
    va_end(vl);
    outp += n > l - 2 ? l - 2 : n;
    *outp++ = '\r';
    *outp++ = '\n';
}

static int
srd(void)
{
    static char l[BufSz], *p = l;
    char *s, *usr, *cmd, *par, *data;
    int rd;
    if (p - l >= BufSz)
        p = l; /* Input buffer overflow, there should something better to do. */
    if (ssl)
        rd = SSL_read(srv.ssl, p, BufSz - (p - l));
    else
        rd = read(srv.fd, p, BufSz - (p - l));
    if (rd <= 0)
        return 0;
    p += rd;
    for (;;) { /* Cycle on all received lines. */
        if (!(s = memchr(l, '\n', p - l)))
            return 1;
        if (s > l && s[-1] == '\r')
            s[-1] = 0;
        *s++ = 0;
        if (*l == ':') {
            if (!(cmd = strchr(l, ' ')))
                goto lskip;
            *cmd++ = 0;
            usr = l + 1;
        } else {
            usr = 0;
            cmd = l;
        }
        if (!(par = strchr(cmd, ' ')))
            goto lskip;
        *par++ = 0;
        if ((data = strchr(par, ':')))
            *data++ = 0;
        scmd(usr, cmd, par, data);
    lskip:
        memmove(l, s, p - s);
        p -= s - l;
    }
}

static void
sinit(const char *key, const char *nick, const char *user)
{
    if (key)
        sndf("PASS %s", key);
    sndf("NICK %s", nick);
    sndf("USER %s 8 * :%s", user, user);
    sndf("MODE %s +i", nick);
}

static char *
dial(const char *host, const char *service)
{
    struct addrinfo hints, *res = NULL, *rp;
    int fd = -1, e;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     /* allow IPv4 or IPv6 */
    hints.ai_flags = AI_NUMERICSERV; /* avoid name lookup for port */
    hints.ai_socktype = SOCK_STREAM;
    if ((e = getaddrinfo(host, service, &hints, &res)))
        return "Getaddrinfo failed.";
    for (rp = res; rp; rp = rp->ai_next) {
        if ((fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) == -1)
            continue;
        if (connect(fd, res->ai_addr, res->ai_addrlen) == -1) {
            close(fd);
            continue;
        }
        break;
    }
    if (fd == -1)
        return "Cannot connect to host.";
    srv.fd = fd;
    if (ssl) {
        SSL_load_error_strings();
        SSL_library_init();
        srv.ctx = SSL_CTX_new(SSLv23_client_method());
        if (!srv.ctx)
            return "Could not initialize ssl context.";
        srv.ssl = SSL_new(srv.ctx);
        if (SSL_set_fd(srv.ssl, srv.fd) == 0
        || SSL_connect(srv.ssl) != 1)
            return "Could not connect with ssl.";
    }
    freeaddrinfo(res);
    return 0;
}

static void
hangup(void)
{
    if (srv.ssl) {
        SSL_shutdown(srv.ssl);
        SSL_free(srv.ssl);
        srv.ssl = 0;
    }
    if (srv.fd) {
        close(srv.fd);
        srv.fd = 0;
    }
    if (srv.ctx) {
        SSL_CTX_free(srv.ctx);
        srv.ctx = 0;
    }
}

static inline int
chfind(const char *name)
{
    int i;

    assert(name);
    for (i = nch - 1; i > 0; i--)
        if (!strcmp(chl[i].name, name))
            break;
    return i;
}

static int
chadd(const char *name, int joined)
{
    int n;

    if (nch >= MaxChans || strlen(name) >= ChanLen)
        return -1;
    if ((n = chfind(name)) > 0)
        return n;
    strcpy(chl[nch].name, name);
    chl[nch].sz = LogSz;
    chl[nch].buf = malloc(LogSz);
    if (!chl[nch].buf)
        panic("Out of memory.");
    chl[nch].eol = chl[nch].buf;
    chl[nch].n = 0;
    chl[nch].join = joined;
    if (joined)
        ch = nch;
    nch++;
    tdrawbar();
    return nch;
}

static int
chdel(char *name)
{
    int n;

    if (!(n = chfind(name)))
        return 0;
    nch--;
    free(chl[n].buf);
    memmove(&chl[n], &chl[n + 1], (nch - n) * sizeof(struct Chan));
    ch = nch - 1;
    tdrawbar();
    return 1;
}

static char *
pushl(char *p, char *e)
{
    int x, cl;
    char *w;
    Rune u[2];
    cchar_t cc;

    u[1] = 0;
    if ((w = memchr(p, '\n', e - p)))
        e = w + 1;
    w = p;
    x = 0;
    for (;;) {
        if (x >= scr.x) {
            waddch(scr.mw, '\n');
            for (x = 0; x < INDENT; x++)
                waddch(scr.mw, ' ');
            if (*w == ' ')
                w++;
            x += p - w;
        }
        if (p >= e || *p == ' ' || p - w + INDENT >= scr.x - 1) {
            while (w < p) {
                w += utf8decode(w, u, UtfSz);
                if (wcwidth(*u) > 0 || *u == '\n') {
                    setcchar(&cc, u, 0, 0, 0);
                    wadd_wch(scr.mw, &cc);
                }
            }
            if (p >= e)
                return e;
        }
        p += utf8decode(p, u, UtfSz);
        if ((cl = wcwidth(*u)) >= 0)
            x += cl;
    }
}

static void
pushf(int cn, const char *fmt, ...)
{
    struct Chan *const c = &chl[cn];
    size_t n, blen = c->eol - c->buf;
    va_list vl;
    time_t t;
    char *s;
    struct tm *tm, *gmtm;

    if (blen + LineLen >= c->sz) {
        c->sz *= 2;
        c->buf = realloc(c->buf, c->sz);
        if (!c->buf)
            panic("Out of memory.");
        c->eol = c->buf + blen;
    }
    t = time(0);
    if (!(tm = localtime(&t)))
        panic("Localtime failed.");
#ifdef DATEFMT
    n = strftime(c->eol, LineLen, DATEFMT, tm);
#endif
    if (!(gmtm = gmtime(&t)))
        panic("Gmtime failed.");
    c->eol[n++] = ' ';
    va_start(vl, fmt);
    s = c->eol + n;
    n += vsnprintf(s, LineLen - n - 1, fmt, vl);
    va_end(vl);

    if (logfp) {
        fprintf(logfp, "%-12.12s\t%04d-%02d-%02dT%02d:%02d:%02dZ\t%s\n",
            c->name,
            gmtm->tm_year + 1900, gmtm->tm_mon + 1, gmtm->tm_mday,
            gmtm->tm_hour, gmtm->tm_min, gmtm->tm_sec, s);
        fflush(logfp);
    }

    strcat(c->eol, "\n");
    if (n >= LineLen - 1)
        c->eol += LineLen - 1;
    else
        c->eol += n + 1;
    if (cn == ch && c->n == 0) {
        char *p = c->eol - n - 1;

        if (p != c->buf)
            waddch(scr.mw, '\n');
        pushl(p, c->eol - 1);
        wrefresh(scr.mw);
    }
}

char
*strremove(char *str, const char *sub) {
    size_t len = strlen(sub);
    if (len > 0) {
        char *p = str;
        size_t size = 0;
        while ((p = strstr(p, sub)) != NULL) {
            size = (size == 0) ? (p - str) + strlen(p + len) + 1 : size - len;
            memmove(p, p + len, size - (p - str));
        }
    }
    return str;
}

static void
scmd(char *usr, char *cmd, char *par, char *data)
{
    int s, c;
    char *pm = strtok(par, " "), *chan;
    if (!usr)
        usr = "?";
    else {
        char *bang = strchr(usr, '!');
        if (bang)
            *bang = 0;
    }
    if (!strcmp(cmd, "PRIVMSG")) {
        if (!strcmp(data, "\001VERSION\001"))
            sndf("NOTICE %s :\001VERSION %s\001", usr, VERSION);
        if (strstr(data, "\001PING") != NULL)
            sndf("NOTICE %s :%s", usr, data);
        if (!pm || !data)
            return;
        if (strchr("&#!+.~", pm[0]))
            chan = pm;
        else
            chan = usr;
        if (!(c = chfind(chan))) {
            if (chadd(chan, 0) < 0)
                return;
            tredraw();
        }
        c = chfind(chan);
        if (strstr(data, "\001ACTION") != NULL) {
            char *s = strremove(data, "\001ACTION ");
            pushf(c, AFMT, usr, s);
        }
        if (strcasestr(data, nick)) {
            pushf(c, PFMTHIGH, usr, data);
            /*
            TODO: figure out notification cmd (shell out or fork/exec?)
            char *cmd;
            snprintf(cmd, sizeof(cmd), "notify-send '%s @ %s' %s", usr, chan, data);
            */
            chl[c].high |= ch != c;
        } else
            pushf(c, PFMT, usr, data);
        if (ch != c) {
            chl[c].new = 1;
            tdrawbar();
        }
    } else if (!strcmp(cmd, "PING")) {
        sndf("PONG :%s", data ? data : "(null)");
    } else if (!strcmp(cmd, "PART")) {
        if (!pm)
            return;
        pushf(chfind(pm), "! %-12s has left %s", usr, pm);
    } else if (!strcmp(cmd, "JOIN")) {
        if (!pm)
            return;
        pushf(chfind(pm), "! %-12s has joined %s", usr, pm);
    } else if (!strcmp(cmd, "470")) { /* Channel forwarding. */
        char *ch = strtok(0, " "), *fch = strtok(0, " ");

        if (!ch || !fch || !(s = chfind(ch)))
            return;
        chl[s].name[0] = 0;
        strncat(chl[s].name, fch, ChanLen - 1);
        tdrawbar();
    } else if (!strcmp(cmd, "471") || !strcmp(cmd, "473")
           || !strcmp(cmd, "474") || !strcmp(cmd, "475")) { /* Join error. */
        if ((pm = strtok(0, " "))) {
            chdel(pm);
            pushf(0, "-!- Cannot join channel %s (%s)", pm, cmd);
            tredraw();
        }
    } else if (!strcmp(cmd, "QUIT")) { /* Commands we don't care about. */
        return;
    } else if (!strcmp(cmd, "NOTICE") || !strcmp(cmd, "375")
           || !strcmp(cmd, "372") || !strcmp(cmd, "376")) {
        pushf(0, "%s", data ? data : "");
    } else
        pushf(0, "%s - %s %s", cmd, par, data ? data : "(null)");
}

static void
uparse(char *m)
{
    char *p = m;
    //if (!p[0]|| (p[1] != ' ' && p[1] != 0)) {
    if (!strncmp("/j", p, 2)) { /* Join channels. */
        p += 1 + (p[2] == ' ');
        p = strtok(p, " ");
        while (p) {
            if (chadd(p, 1) < 0)
                break;
            sndf("JOIN %s", p);
            p = strtok(0, " ");
        }
        tredraw();
        return;
    }
    if (!strncmp("/l", p, 2)) {/* Leave channels. */
        p += 1 + (p[2] == ' ');
        if (!*p) {
            if (ch == 0)
                return; /* Cannot leave server window. */
            strcat(p, chl[ch].name);
        }
        p = strtok(p, " ");
        while (p) {
            if (chdel(p))
                sndf("PART %s", p);
            p = strtok(0, " ");
        }
        tredraw();
        return;
    }
    if (!strncmp("/q", p, 2)) { /* Private message. */
        m = p + 1 + (p[2] == ' ');
        if (!(p = strchr(m, ' ')))
            return;
        *p++ = 0;
        sndf("PRIVMSG %s :%s", m, p);
        return;
    }
    if (!strncmp("/r", p, 2)) { /* Send raw. */
        if (p[1])
            sndf("%s", &p[3]);
        return;
    }
    if (!strncmp("/x", p, 2)) {/* Quit. */
        quit = 1;
        return;
    }
    if (!strncmp("/me", p, 3)) {
        char *s = strremove(p, "/me");
        pushf(ch, AFMT, nick, s);
        sndf("PRIVMSG %s :\001ACTION %s\001", chl[ch].name, s);
    }
    else {
        if (ch == 0)
            return;
        m += strspn(m, " ");
        if (!*m)
            return;
        pushf(ch, PFMT, nick, m);
        sndf("PRIVMSG %s :%s", chl[ch].name, m);
        return;
    }/* Send on current channel. */
}

static void
sigwinch(int sig)
{
    if (sig)
        winchg = 1;
}

static void
tinit(void)
{
    setlocale(LC_ALL, "");
    signal(SIGWINCH, sigwinch);
    initscr();
    raw();
    noecho();
    getmaxyx(stdscr, scr.y, scr.x);
    if (scr.y < 4)
        panic("Screen too small.");
    if ((scr.sw = newwin(1, scr.x, 0, 0)) == 0
    || (scr.mw = newwin(scr.y - 2, scr.x, 1, 0)) == 0
    || (scr.iw = newwin(1, scr.x, scr.y - 1, 0)) == 0)
        panic("Cannot create windows.");
    keypad(scr.iw, 1);
    scrollok(scr.mw, 1);
    if (has_colors() == TRUE) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_BLACK, COLOR_WHITE);
        init_pair(2, COLOR_RED, COLOR_WHITE);
        init_pair(3, COLOR_GREEN, COLOR_WHITE);
        wbkgd(scr.sw, COLOR_PAIR(1));
    }
}

static void
tresize(void)
{
    struct winsize ws;

    winchg = 0;
    if (ioctl(0, TIOCGWINSZ, &ws) < 0)
        panic("Ioctl (TIOCGWINSZ) failed.");
    if (ws.ws_row <= 2)
        return;
    resizeterm(scr.y = ws.ws_row, scr.x = ws.ws_col);
    wresize(scr.mw, scr.y - 2, scr.x);
    wresize(scr.iw, 1, scr.x);
    wresize(scr.sw, 1, scr.x);
    mvwin(scr.iw, scr.y - 1, 0);
    tredraw();
    tdrawbar();
}

static void
tredraw(void)
{
    struct Chan *const c = &chl[ch];
    char *q, *p;
    int nl = -1;

    if (c->eol == c->buf) {
        wclear(scr.mw);
        wrefresh(scr.mw);
        return;
    }
    p = c->eol - 1;
    if (c->n) {
        int i = c->n;
        for (; p > c->buf; p--)
            if (*p == '\n' && !i--)
                break;
        if (p == c->buf)
            c->n -= i;
    }
    q = p;
    while (nl < scr.y - 2) {
        while (*q != '\n' && q > c->buf)
            q--;
        nl++;
        if (q == c->buf)
            break;
        q--;
    }
    if (q != c->buf)
        q += 2;
    wclear(scr.mw);
    wmove(scr.mw, 0, 0);
    while (q < p)
        q = pushl(q, p);
    wrefresh(scr.mw);
}

static void
tdrawbar(void)
{
    size_t l;
    int fst = ch;

    for (l = 0; fst > 0 && l < scr.x / 2; fst--)
        l += strlen(chl[fst].name) + 3;

    werase(scr.sw);
    for (l = 0; fst < nch && l < scr.x; fst++) {
        char *p = chl[fst].name;
        if (fst == ch)
            wattron(scr.sw, A_REVERSE);
        waddstr(scr.sw, "  "), l++;
        if (chl[fst].high) {
            wattron(scr.sw, COLOR_PAIR(2)), l++;
        }
        else if (chl[fst].new)
            wattron(scr.sw, COLOR_PAIR(3)), l++;
        for (; *p && l < scr.x; p++, l++)
            waddch(scr.sw, *p);
        if (l < scr.x - 1)
            waddstr(scr.sw, "  "), l += 2;
        if (fst == ch)
            wattroff(scr.sw, A_REVERSE);
            wattroff(scr.sw, COLOR_PAIR(2));
            wattroff(scr.sw, COLOR_PAIR(3));
    }
    wrefresh(scr.sw);
}

static void
tgetch(void)
{
    static char l[BufSz];
    static size_t shft, cu, len;
    size_t dirty = len + 1, i;
    int c;

    c = wgetch(scr.iw);
    switch (c) {
    case CTRL('n'):
        ch = (ch + 1) % nch;
        chl[ch].high = chl[ch].new = 0;
        tdrawbar();
        tredraw();
        return;
    case CTRL('p'):
        ch = (ch + nch - 1) % nch;
        chl[ch].high = chl[ch].new = 0;
        tdrawbar();
        tredraw();
        return;
    case KEY_PPAGE:
        chl[ch].n += SCROLL;
        tredraw();
        return;
    case KEY_NPAGE:
        chl[ch].n -= SCROLL;
        if (chl[ch].n < 0)
            chl[ch].n = 0;
        tredraw();
        return;
    case CTRL('a'):
    case KEY_HOME:
        cu = 0;
        break;
    case CTRL('e'):
    case KEY_END:
        cu = len;
        break;
    case CTRL('b'):
    case KEY_LEFT:
        if (cu)
            cu--;
        break;
    case KEY_RIGHT:
        if (cu < len)
            cu++;
        break;
    case CTRL('k'):
        dirty = len = cu;
        break;
    case CTRL('u'):
        if (cu == 0)
            return;
        len -= cu;
        memmove(l, &l[cu], len);
        dirty = cu = 0;
        break;
    case CTRL('d'):
        if (cu >= len)
            return;
        memmove(&l[cu], &l[cu + 1], len - cu - 1);
        dirty = cu;
        len--;
        break;
    case CTRL('h'):
    case KEY_BACKSPACE:
        if (cu == 0)
            return;
        memmove(&l[cu - 1], &l[cu], len - cu);
        dirty = --cu;
        len--;
        break;
    case CTRL('w'):
        if (cu == 0)
            break;
        i = 1;
        while (l[cu - i] == ' ' && cu - i != 0) i++;
        while (l[cu - i] != ' ' && cu - i != 0) i++;
        if (cu - i != 0) i--;
        memmove(&l[cu - i], &l[cu], len - cu);
        cu -= i;
        dirty = cu;
        len -= i;
        break;
    case '\n':
        l[len] = 0;
        uparse(l);
        dirty = cu = len = 0;
        break;
    default:
        if (c > CHAR_MAX || len >= BufSz - 1)
            return; /* Skip other curses codes. */
        memmove(&l[cu + 1], &l[cu], len - cu);
        dirty = cu;
        len++;
        l[cu++] = c;
        break;
    }
    while (cu < shft)
        dirty = 0, shft -= shft >= scr.x / 2 ? scr.x / 2 : shft;
    while (cu >= scr.x + shft)
        dirty = 0, shft += scr.x / 2;
    if (dirty <= shft)
        i = shft;
    else if (dirty > scr.x + shft || dirty > len)
        goto mvcur;
    else
        i = dirty;
    wmove(scr.iw, 0, i - shft);
    wclrtoeol(scr.iw);
    for (; i - shft < scr.x && i < len; i++)
        waddch(scr.iw, l[i]);
mvcur:  wmove(scr.iw, 0, cu - shft);
}

static void
treset(void)
{
    if (scr.mw)
        delwin(scr.mw);
    if (scr.sw)
        delwin(scr.sw);
    if (scr.iw)
        delwin(scr.iw);
    endwin();
}

int
main(int argc, char *argv[])
{
    const char *user = getenv("USER");
    const char *ircnick = getenv("IRCNICK");
    const char *key = getenv("IRCPASS");
    const char *server = SRV;
    const char *port = PORT;
    char *err;
    int o, reconn;

    signal(SIGPIPE, SIG_IGN);
    while ((o = getopt(argc, argv, "thk:n:u:s:p:l:")) >= 0)
        switch (o) {
        case 'h':
        case '?':
        usage:
            fputs("usage: irc [-n NICK] [-u USER] [-s SERVER] [-p PORT] [-l LOGFILE ] [-t] [-h]\n", stderr);
            exit(0);
        case 'l':
            if (!(logfp = fopen(optarg, "a")))
                panic("fopen: logfile");
            break;
        case 'n':
            if (strlen(optarg) >= sizeof nick)
                goto usage;
            strcpy(nick, optarg);
            break;
        case 't':
            ssl = 1;
            break;
        case 'u':
            user = optarg;
            break;
        case 's':
            server = optarg;
            break;
        case 'p':
            port = optarg;
            break;
        }
    if (!user)
        user = "anonymous";
    if (!nick[0] && ircnick && strlen(ircnick) < sizeof nick)
        strcpy(nick, ircnick);
    if (!nick[0] && strlen(user) < sizeof nick)
        strcpy(nick, user);
    if (!nick[0])
        goto usage;
    tinit();
    err = dial(server, port);
    if (err)
        panic(err);
    chadd(server, 0);
    sinit(key, nick, user);
    reconn = 0;
    while (!quit) {
        struct timeval t = {.tv_sec = 5};
        struct Chan *c;
        fd_set rfs, wfs;
        int ret;

        if (winchg)
            tresize();
        FD_ZERO(&wfs);
        FD_ZERO(&rfs);
        FD_SET(0, &rfs);
        if (!reconn) {
            FD_SET(srv.fd, &rfs);
            if (outp != outb)
                FD_SET(srv.fd, &wfs);
        }
        ret = select(srv.fd + 1, &rfs, &wfs, 0, &t);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            panic("Select failed.");
        }
        if (reconn) {
            hangup();
            if (reconn++ == MaxRecons + 1)
                panic("Link lost.");
            pushf(0, "-!- Link lost, attempting reconnection...");
            if (dial(server, port) != 0)
                continue;
            sinit(key, nick, user);
            for (c = chl; c < &chl[nch]; ++c)
                if (c->join)
                    sndf("JOIN %s", c->name);
            reconn = 0;
        }
        if (FD_ISSET(srv.fd, &rfs)) {
            if (!srd()) {
                reconn = 1;
                continue;
            }
        }
        if (FD_ISSET(srv.fd, &wfs)) {
            int wr;

            if (ssl)
                wr = SSL_write(srv.ssl, outb, outp - outb);
            else
                wr = write(srv.fd, outb, outp - outb);
            if (wr <= 0) {
                reconn = wr < 0;
                continue;
            }
            outp -= wr;
            memmove(outb, outb + wr, outp - outb);
        }
        if (FD_ISSET(0, &rfs)) {
            tgetch();
            wrefresh(scr.iw);
        }
    }
    hangup();
    while (nch--)
        free(chl[nch].buf);
    treset();
    exit(0);
}
