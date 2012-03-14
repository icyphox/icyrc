/*% cc -g -Wall -lncurses -o # %
 */
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

#define SCROLL 15
#define DATEFMT "%H:%M"
#define PFMT "%-12s < %s"

enum { ChanLen = 64, LineLen = 512, MaxChans = 16, BufSz = 2048, LogSz = 4096 };

char nick[64];
char prefix[64];
int quit, winchg;
int sfd; /* Server file descriptor. */
struct {
	int x;
	int y;
	WINDOW *sw, *mw, *iw;
} scr; /* Screen relative data. */
int eof; /* EOF reached on server side. */
struct Chan {
	char name[ChanLen];
	char *buf, *eol;
	int n; /* Scroll offset. */
	size_t sz; /* size of buf. */
} chl[MaxChans];
int nch, ch; /* Current number of channels, and current channel. */
char outb[BufSz], *outp=outb; /* Output buffer. */

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

static void
sndf(const char *fmt, ...)
{
	va_list vl;
	size_t n, l=BufSz-(outp-outb);

	if (l<2) return;
	va_start(vl, fmt);
	n=vsnprintf(outp, l-2, fmt, vl);
	va_end(vl);
	outp += n>l-2 ? l-2 : n;
	*outp++ = '\r';
	*outp++ = '\n';
}

static int
srd(void)
{
	static char l[BufSz], *p=l;
	char *s, *usr, *cmd, *par, *data;
	int rd;

	if (p-l>=BufSz) p=l; /* Input buffer overflow, there should something better to do. */
	rd=read(sfd, p, BufSz-(p-l));
	if (rd<0) {
		if (errno==EINTR) return 1;
		panic("IO error while reading.");
	}
	if (rd==0) return 0;
	p+=rd;
	for (;;) { /* Cycle on all received lines. */
		if (!(s=memchr(l, '\n', p-l)))
			return 1;
		if (s>l && s[-1]=='\r')
			s[-1]=0;
		*s++ = 0;
		if (*l==':') {
			if (!(cmd=strchr(l, ' '))) goto lskip;
				*cmd++ = 0;
			usr = l+1;
		} else {
			usr = 0;
			cmd = l;
		}
		if (!(par=strchr(cmd, ' '))) goto lskip;
		*par++ = 0;
		if ((data=strchr(par, ':')))
			*data++ = 0;
		scmd(usr, cmd, par, data);
	lskip:
		memmove(l, s, p-s);
		p-=s-l;
	}
}

static int
dial(const char *host, short port)
{
	int f;
	struct sockaddr_in sin;
	struct addrinfo *ai, hai;

	hai.ai_family = AF_INET;
	hai.ai_socktype = SOCK_STREAM;
	hai.ai_flags = hai.ai_protocol = 0;
	if (getaddrinfo(host, 0, &hai, &ai))
		panic("Cannot resolve host.");
	memcpy(&sin, ai->ai_addr, sizeof sin);
	sin.sin_port = htons(port);
	freeaddrinfo(ai);
	f = socket(AF_INET, SOCK_STREAM, 0);
	if (f<0)
		panic("Cannot create socket.");
	if (connect(f, (struct sockaddr *)&sin, sizeof sin)<0)
		panic("Cannot connect to host.");
	return f;
}

static int
chadd(char *name)
{
	if (nch>=MaxChans || strlen(name)>=ChanLen)
		return -1;
	strcpy(chl[nch].name, name);
	chl[nch].sz=LogSz;
	chl[nch].buf=malloc(LogSz);
	if (!chl[nch].buf)
		panic("Out of memory.");
	chl[nch].eol=chl[nch].buf;
	chl[nch].n=0;
	ch=nch++;
	tdrawbar();
	return nch;
}

static inline int
chfind(char *name)
{
	int i;

	assert(name);
	for (i=nch-1; i>0; i--)
		if (!strcmp(chl[i].name, name))
			break;
	return i;
}

static int
chdel(char *name)
{
	int n;

	if (!(n=chfind(name))) return 0;
	nch--;
	free(chl[n].buf);
	memmove(&chl[n], &chl[n+1], (nch-n)*sizeof(struct Chan));
	ch=nch-1;
	tdrawbar();
	return 1;
}

static void
pushf(int cn, const char *fmt, ...)
{
	struct Chan *const c=&chl[cn];
	size_t n, blen=c->eol-c->buf;
	va_list vl;
	time_t t;
	struct tm *tm;

	if (blen+LineLen>=c->sz) {
		c->sz *= 2;
		c->buf=realloc(c->buf, c->sz);
		if (!c->buf) panic("Out of memory.");
		c->eol=c->buf+blen;
	}
	t=time(0);
	if (!(tm=localtime(&t))) panic("Localtime failed.");
	n=strftime(c->eol, LineLen, DATEFMT, tm);
	c->eol[n++] = ' ';
	va_start(vl, fmt);
	n+=vsnprintf(c->eol+n, LineLen-n-1, fmt, vl);
	va_end(vl);
	strcat(c->eol, "\n");
	if (n>=LineLen-1)
		c->eol+=LineLen-1;
	else
		c->eol+=n+1;
	if (cn==ch && c->n==0) {
		char *p=c->eol-n-1;
		if (p!=c->buf) waddch(scr.mw, '\n');
		for (; p<c->eol-1; p++)
			waddch(scr.mw, *p);
		wrefresh(scr.mw);
	}
}

static void
scmd(char *usr, char *cmd, char *par, char *data)
{
	int s;
	char *pm=strtok(par, " ");

	if (!usr) usr="?";
	else {
		char *bang=strchr(usr, '!');
		if (bang)
			*bang=0;
	}
	if (!strcmp(cmd, "PRIVMSG")) {
		if (!pm || !data) return;
		pushf(chfind(pm), PFMT, usr, data);
	} else if (!strcmp(cmd, "PING")) {
		sndf("PONG :%s", data?data:"(null)");
	} else if (!strcmp(cmd, "PART")) {
		if (!pm) return;
		pushf(chfind(pm), "-!- %s has left %s", usr, pm);
	} else if (!strcmp(cmd, "JOIN")) {
		if (!pm) return;
		pushf(chfind(pm), "-!- %s has joined %s", usr, pm);
	} else if (!strcmp(cmd, "470")) { /* Channel forwarding. */
		char *ch=strtok(0, " "), *fch=strtok(0, " ");
		if (!ch || !fch || !(s=chfind(ch))) return;
		chl[s].name[0] = 0;
		strncat(chl[s].name, fch, ChanLen-1);
		tdrawbar();
	} else if (!strcmp(cmd, "471") || !strcmp(cmd, "473")
	       || !strcmp(cmd, "474") || !strcmp(cmd, "475")) { /* Join error. */
		if ((pm=strtok(0, " "))) {
			chdel(pm);
			pushf(0, "-!- Cannot join channel %s (%s)", pm, cmd);
			tredraw();
		}
	} else if (!strcmp(cmd, "QUIT")) { /* Commands we don't care about. */
		return;
	} else if (!strcmp(cmd, "NOTICE") || !strcmp(cmd, "375")
	       || !strcmp(cmd, "372") || !strcmp(cmd, "376")) {
		pushf(0, "%s", data?data:"");
	} else
		pushf(0, "%s - %s %s", cmd, par, data?data:"(null)");
}

static void
uparse(char *m)
{
	char *p=m;

	if (p[1]!=' ' && p[1]!=0) {
	pmsg:
		if (ch==0) return;
		m+=strspn(m, " ");
		if (!*m) return;
		pushf(ch, PFMT, nick, m);
		sndf("PRIVMSG %s :%s", chl[ch].name, m);
		return;
	}
	switch (*p) {
	case 'j': /* Join channels. */
		p+=1+(p[1]==' ');
		p=strtok(p, " ");
		while (p) {
			if (chadd(p)<0) break;
			sndf("JOIN %s", p);
			p=strtok(0, " ");
		}
		tredraw();
		return;
	case 'l': /* Leave channels. */
		p+=1+(p[1]==' ');
		if (!*p) {
			if (ch==0) return; /* Cannot leave server window. */
			strcat(p, chl[ch].name);
		}
		p=strtok(p, " ");
		while (p) {
			if (chdel(p))
				sndf("PART %s", p);
			p=strtok(0, " ");
		}
		tredraw();
		return;
	case 'm': /* Private message. */
		m=p+1+(p[1]==' ');
		if (!(p=strchr(m, ' '))) return;
		*p++ = 0;
		sndf("PRIVMSG %s :%s", m, p);
		return;
	case 'r': /* Send raw. */
		if (p[1])
			sndf("%s", &p[2]);
		return;
	case 'q': /* Quit. */
		quit=1;
		return;
	default: /* Send on current channel. */
		goto pmsg;
	}
}

static void
sigwinch(int sig)
{
	if (sig) winchg=1;
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
	if (scr.y<4) panic("Screen too small.");
	if ((scr.sw=newwin(1, scr.x, 0, 0))==0
	|| (scr.mw=newwin(scr.y-2, scr.x, 1, 0))==0
	|| (scr.iw=newwin(1, scr.x, scr.y-1, 0))==0)
		panic("Cannot create windows.");
	keypad(scr.iw, 1);
	scrollok(scr.mw, 1);
	if (has_colors()==TRUE) {
		start_color();
		init_pair(1, COLOR_WHITE, COLOR_BLUE);
		wbkgd(scr.sw, COLOR_PAIR(1));
	}
}

static void
tresize(void)
{
	struct winsize ws;

	winchg=0;
	if (ioctl(0, TIOCGWINSZ, &ws)<0)
		panic("Ioctl (TIOCGWINSZ) failed.");
	resizeterm(scr.y=ws.ws_row, scr.x=ws.ws_col);
	if (scr.y<3 || scr.x<10)
		panic("Screen too small.");
	wresize(scr.mw, scr.y-2, scr.x);
	wresize(scr.iw, 1, scr.x);
	wresize(scr.sw, 1, scr.x);
	mvwin(scr.iw, scr.y-1, 0);
	tredraw();
	tdrawbar();
}

static void
tredraw(void)
{
	struct Chan *const c=&chl[ch];
	char *q, *p;
	int llen=0, nl=-1;

	if (c->eol==c->buf) {
		wclear(scr.mw);
		wrefresh(scr.mw);
		return;
	}
	p=c->eol-1;
	if (c->n) {
		int i=c->n;
		for (; p>c->buf; p--)
			if (*p=='\n' && !i--) break;
		if (p==c->buf) c->n-=i;
	}
	q=p;
	while (nl<scr.y-2) {
		llen=0;
		while (*q!='\n' && q>c->buf)
			q--, llen++;
		nl += 1+llen/scr.x;
		if (q==c->buf) break;
		q--;
	}
	if (q!=c->buf) q+=2;
	for (llen=0; nl>scr.y-2; ) { /* Maybe we must split the top line. */
		if (q[llen]=='\n' || llen>=scr.x) {
			q+=llen+(q[llen]=='\n');
			llen=0;
			nl--;
		} else llen++;
	}
	wclear(scr.mw);
	wmove(scr.mw, 0, 0);
	for (; q<p; q++)
		waddch(scr.mw, *q);
	wrefresh(scr.mw);
}

static void
tdrawbar(void)
{
	size_t l;
	int fst=ch;

	for (l=0; fst>0 && l<scr.x/2; fst--)
		l+=strlen(chl[fst].name)+3;

	werase(scr.sw);
	for (l=0; fst<nch && l<scr.x; fst++) {
		char *p=chl[fst].name;

		if (fst==ch) wattron(scr.sw, A_BOLD);
		waddch(scr.sw, '['), l++;
		for (; *p && l<scr.x; p++, l++)
			waddch(scr.sw, *p);
		if (l<scr.x-1)
			waddstr(scr.sw, "] "), l+=2;
		if (fst==ch) wattroff(scr.sw, A_BOLD);
	}
	wrefresh(scr.sw);
}

static void
tgetch(void)
{
	static char l[BufSz];
	static size_t shft, cu, len;
	size_t dirty=len+1, i;
	int c;

	c=wgetch(scr.iw);
	switch (c) {
	case CTRL('n'):
		ch=(ch+1)%nch;
		tdrawbar();
		tredraw();
		return;
	case CTRL('p'):
		ch=(ch+nch-1)%nch;
		tdrawbar();
		tredraw();
		return;
	case KEY_PPAGE:
		chl[ch].n+=SCROLL;
		tredraw();
		return;
	case KEY_NPAGE:
		chl[ch].n-=SCROLL;
		if (chl[ch].n<0) chl[ch].n=0;
		tredraw();
		return;
	case CTRL('a'):
		cu=0;
		break;
	case CTRL('e'):
		cu=len;
		break;
	case CTRL('b'):
	case KEY_LEFT:
		if (cu) cu--;
		break;
	case CTRL('f'):
	case KEY_RIGHT:
		if (cu<len) cu++;
		break;
	case CTRL('k'):
		dirty=len=cu;
		break;
	case CTRL('u'):
		if (cu==0) return;
		len-=cu;
		memmove(l, &l[cu], len);
		dirty=cu=0;
		break;
	case CTRL('d'):
		if (cu>=len) return;
		memmove(&l[cu], &l[cu+1], len-cu-1);
		dirty=cu;
		len--;
		break;
	case KEY_BACKSPACE:
		if (cu==0) return;
		memmove(&l[cu-1], &l[cu], len-cu);
		dirty=--cu;
		len--;
		break;
	case '\n':
		l[len]=0;
		uparse(l);
		dirty=cu=len=0;
		break;
	default:
		if (c>CHAR_MAX || len>=BufSz-1) return; /* Skip other curses codes. */
		memmove(&l[cu+1], &l[cu], len-cu);
		dirty=cu;
		len++;
		l[cu++]=c;
		break;
	}
	while (cu<shft)
		dirty=0, shft -= shft>=scr.x/2 ? scr.x/2 : shft;
	while (cu>=scr.x+shft)
		dirty=0, shft += scr.x/2;
	if (dirty<=shft)
		i=shft;
	else if (dirty>scr.x+shft || dirty>len)
		goto mvcur;
	else
		i=dirty;
	wmove(scr.iw, 0, i-shft);
	wclrtoeol(scr.iw);
	for (; i-shft<scr.x && i<len; i++)
		waddch(scr.iw, l[i]);
mvcur:	wmove(scr.iw, 0, cu-shft);
}

static void
treset(void)
{
	if (scr.mw) delwin(scr.mw);
	if (scr.sw) delwin(scr.sw);
	if (scr.iw) delwin(scr.iw);
	endwin();
}

int
main(void)
{
	const char *user = getenv("USER");

	if (!user) user="Unknown";
	tinit();
	chadd("*server*");
	strcpy(nick, "_mpu");
	sfd = dial("chat.freenode.org", 6667);
	sndf("NICK %s", nick);
	sndf("USER brebi 8 * :%s", user);
	sndf("MODE %s +i", nick);
	while (!quit) {
		fd_set rfs, wfs;
		int ret;

		if (winchg)
			tresize();
		FD_ZERO(&wfs);
		FD_ZERO(&rfs);
		FD_SET(0, &rfs);
		FD_SET(sfd, &rfs);
		if (outp!=outb)
			FD_SET(sfd, &wfs);
		ret=select(sfd+1, &rfs, &wfs, 0, 0);
		if (ret<0) {
			if (errno==EINTR) continue;
			panic("Select failed.");
		}
		if (FD_ISSET(sfd, &rfs)) {
			if (!srd())
				quit=1;
		}
		if (FD_ISSET(sfd, &wfs)) {
			int wr;

			wr=write(sfd, outb, outp-outb);
			if (wr<0) {
				if (errno==EINTR) continue;
				panic("Write error.");
			}
			if (wr==0) continue;
			outp-=wr;
			memmove(outb, outb+wr, outp-outb);
		}
		if (FD_ISSET(0, &rfs)) {
			tgetch();
			wrefresh(scr.iw);
		}
	}
	close(sfd);
	while (nch--)
		free(chl[nch].buf);
	treset();
	exit(0);
}
