/* smix - minimal PipeWire mixer for the terminal (talks to wpctl) */
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAXN 64
#define LEN(a) (sizeof(a) / sizeof((a)[0]))

enum { SINK, SOURCE, STREAM, NSEC };
enum { K_UP = 0x1000, K_DOWN, K_RIGHT, K_LEFT, K_ESC = 27 };

typedef union { int i; const char *s; } Arg;
typedef struct { int key; void (*fn)(const Arg *); Arg arg; } Key;
typedef struct { int id, def, muted, vol; char name[128]; } Node;

static void movesel(const Arg *a);
static void switchtab(const Arg *a);
static void settab(const Arg *a);
static void run(const Arg *a);
static void spawn(const Arg *a);
static void refresh(void);
static void refresh_k(const Arg *a);
static void volume(const Arg *a);
static void togglech(const Arg *a);
static void togglelock(const Arg *a);
static void quit(const Arg *a);

#include "config.h"

#ifndef CHLOCK
#define CHLOCK 1
#endif

static Node nodes[NSEC][MAXN];
static int cnt[NSEC], sel[NSEC], off[NSEC], tab = SINK;
static volatile sig_atomic_t running = 1;
static struct termios orig;
static const char *tabname[NSEC] = { "Sinks", "Sources", "Streams" };

#define MAXCH 8
static int openid = -1, chsel = -1, chn, chv[MAXCH];
static char chname[MAXCH][16];
static const char *msg;
static int chlock = CHLOCK;

static int
getvol(int id, int *vol, int *muted)
{
	char cmd[64], buf[128];
	FILE *f;
	int ok = 0;

	snprintf(cmd, sizeof cmd, "wpctl get-volume %d 2>/dev/null", id);
	if (!(f = popen(cmd, "r")))
		return -1;
	if (fgets(buf, sizeof buf, f) && !strncmp(buf, "Volume:", 7)) {
		*vol = (int)(atof(buf + 7) * 100 + 0.5);
		*muted = strstr(buf, "MUTED") != NULL;
		ok = 1;
	}
	pclose(f);
	return ok ? 0 : -1;
}

/*
 * Per-channel volumes go through the pulse layer (pactl + pipewire-pulse),
 * the same path pulsemixer uses, so hardware (ALSA route) volumes work too.
 * Pulse indexes equal wpctl ids for sinks/sources but not for streams, so
 * blocks are matched on their object.id property.
 */
typedef struct { int idx, objid, n, v[MAXCH]; char nm[MAXCH][16]; } Blk;

static const char *pk[]   = { "sinks", "sources", "sink-inputs", "source-outputs" };
static const char *pset[] = { "sink", "source", "sink-input", "source-output" };
static int chkind, chidx;
static const struct { const char *l, *s; } chshort[] = {
	{ "front-left", "FL" }, { "front-right", "FR" }, { "front-center", "FC" },
	{ "rear-left", "RL" }, { "rear-right", "RR" }, { "rear-center", "RC" },
	{ "side-left", "SL" }, { "side-right", "SR" }, { "lfe", "LFE" },
};

static int
take(const Blk *b, int id, int kind)
{
	int i;

	if (!b->n || !(b->objid == id || (b->objid < 0 && b->idx == id)))
		return 0;
	for (i = 0; i < b->n; i++) {
		chv[i] = b->v[i];
		snprintf(chname[i], sizeof chname[i], "%s", b->nm[i]);
	}
	chn = b->n;
	chidx = b->idx;
	chkind = kind;
	return 1;
}

static int
pulsech(int kind, int id)
{
	char line[512], cmd[64], *p, *q, *e;
	const char *s;
	size_t k, len;
	Blk b;
	FILE *f;
	int found = 0;

	snprintf(cmd, sizeof cmd, "pactl list %s 2>/dev/null", pk[kind]);
	if (!(f = popen(cmd, "r")))
		return 0;
	memset(&b, 0, sizeof b);
	b.objid = -1;
	while (fgets(line, sizeof line, f)) {
		if (line[0] != '\t' && line[0] != ' ' && (p = strchr(line, '#'))) {
			if ((found = take(&b, id, kind)))
				break;
			memset(&b, 0, sizeof b);
			b.idx = atoi(p + 1);
			b.objid = -1;
		} else if (!strncmp(line, "\tVolume:", 8)) {
			p = line + 8;
			while (b.n < MAXCH && (q = strchr(p, ':'))) {
				for (s = p; *s == ' ' || *s == ','; s++)
					;
				len = q - s;
				if (len >= sizeof b.nm[0])
					len = sizeof b.nm[0] - 1;
				memcpy(b.nm[b.n], s, len);
				b.nm[b.n][len] = 0;
				for (k = 0; k < LEN(chshort); k++)
					if (!strcmp(b.nm[b.n], chshort[k].l))
						snprintf(b.nm[b.n], sizeof b.nm[0], "%s", chshort[k].s);
				strtol(q + 1, &e, 10);
				if (!(e = strchr(e, '/')))
					break;
				b.v[b.n++] = atoi(e + 1);
				if (!(p = strstr(e, "dB")))
					break;
				p += 2;
			}
		} else if ((p = strstr(line, "object.id = \"")))
			b.objid = atoi(p + 13);
	}
	if (!found)
		found = take(&b, id, kind);
	pclose(f);
	return found;
}

static void
loadch(void)
{
	int i, found = 0;

	chn = 0;
	if (openid < 0)
		return;
	for (i = 0; i < cnt[tab]; i++)
		if (nodes[tab][i].id == openid)
			found = 1;
	if (!found) {
		openid = chsel = -1;
		return;
	}
	found = tab == STREAM ? pulsech(2, openid) || pulsech(3, openid)
	                      : pulsech(tab, openid);
	if (!found || chn < 2) {
		chn = 0;
		openid = chsel = -1;
	} else if (chsel >= chn)
		chsel = chn - 1;
}

static void
refresh(void)
{
	int selid[NSEC], t, i, audio = 0, sec = -1, streamcol = -1;
	char line[512], *p, *e, *v;
	FILE *f;
	size_t l;
	Node *nd;

	for (t = 0; t < NSEC; t++)
		selid[t] = sel[t] < cnt[t] ? nodes[t][sel[t]].id : -1;
	if (!(f = popen("wpctl status 2>/dev/null", "r")))
		return;
	memset(cnt, 0, sizeof cnt);

	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		l = strlen(line);
		if (!l)
			continue;
		/* top level header (no tree prefix) */
		if (!isspace((unsigned char)line[0]) && (unsigned char)line[0] < 0x80) {
			audio = !strncmp(line, "Audio", 5);
			sec = -1;
			continue;
		}
		if (!audio)
			continue;
		/* section header, e.g. " ├─ Sinks:" */
		if (line[l - 1] == ':') {
			sec = strstr(line, "Sinks:") ? SINK :
			      strstr(line, "Sources:") ? SOURCE :
			      strstr(line, "Streams:") ? STREAM : -1;
			streamcol = -1;
			continue;
		}
		if (sec < 0 || cnt[sec] >= MAXN)
			continue;
		/* find "<id>." */
		for (p = line; *p; p++)
			if (isdigit((unsigned char)*p) && (p == line || p[-1] == ' ')) {
				strtol(p, &e, 10);
				if (*e == '.')
					break;
			}
		if (!*p)
			continue;
		v = strstr(line, "[vol:");
		if (!v && sec == STREAM) {
			/* stream node lines have no vol; ports are indented deeper */
			if (streamcol < 0)
				streamcol = p - line;
			if (p - line != streamcol)
				continue;
		} else if (!v)
			continue;

		nd = &nodes[sec][cnt[sec]];
		memset(nd, 0, sizeof *nd);
		nd->id = atoi(p);
		for (i = 0; &line[i] < p; i++)
			if (line[i] == '*')
				nd->def = 1;
		{
			const char *s = e + 1, *end;
			while (*s == ' ')
				s++;
			end = v ? v : s + strlen(s);
			while (end > s && end[-1] == ' ')
				end--;
			l = end - s;
			if (l >= sizeof nd->name)
				l = sizeof nd->name - 1;
			memcpy(nd->name, s, l);
			nd->name[l] = 0;
		}
		if (v) {
			nd->vol = (int)(atof(v + 5) * 100 + 0.5);
			nd->muted = strstr(v, "MUTED") != NULL;
		} else if (getvol(nd->id, &nd->vol, &nd->muted) < 0)
			nd->vol = 0;
		cnt[sec]++;
	}
	pclose(f);

	for (t = 0; t < NSEC; t++) {
		for (i = 0; i < cnt[t]; i++)
			if (nodes[t][i].id == selid[t]) {
				sel[t] = i;
				break;
			}
		if (sel[t] >= cnt[t])
			sel[t] = cnt[t] ? cnt[t] - 1 : 0;
	}
	if (chsel >= 0 && (!cnt[tab] || nodes[tab][sel[tab]].id != openid))
		chsel = -1;
	loadch();
}

/* run cmd through sh; %d is replaced with the selected node id */
static void
exec_cmd(const char *tpl, int wait)
{
	char cmd[1024], idstr[16] = "", name[160] = "";
	const char *s;
	size_t n = 0;
	pid_t pid;
	int id = cnt[tab] ? nodes[tab][sel[tab]].id : -1;

	if (!tpl || !*tpl)
		return;
	if (id < 0 && strstr(tpl, "%d"))
		return;
	if (id >= 0) {
		snprintf(idstr, sizeof idstr, "%d", id);
		snprintf(name, sizeof name, "%s", nodes[tab][sel[tab]].name);
	}
	for (s = tpl; *s && n < sizeof cmd - 16; s++) {
		if (s[0] == '%' && s[1] == 'd') {
			n += snprintf(cmd + n, sizeof cmd - n, "%s", idstr);
			s++;
		} else
			cmd[n++] = *s;
	}
	cmd[n] = 0;

	if ((pid = fork()) < 0)
		return;
	if (pid == 0) {
		int fd = open("/dev/null", O_RDWR);
		if (!wait && fork())
			_exit(0);
		if (!wait)
			setsid();
		if (fd >= 0) {
			dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
		}
		setenv("SMIX_ID", idstr, 1);
		setenv("SMIX_NAME", name, 1);
		setenv("SMIX_TAB", tabname[tab], 1);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	waitpid(pid, NULL, 0);
}

static void
run(const Arg *a)
{
	exec_cmd(a->s, 1);
	exec_cmd(postcmd, 0);
}

static void
spawn(const Arg *a)
{
	exec_cmd(a->s, 0);
}

static void
movesel(const Arg *a)
{
	int d = a->i, id = cnt[tab] ? nodes[tab][sel[tab]].id : -1;

	for (; d > 0; d--) {
		if (id == openid && chsel + 1 < chn)
			chsel++;
		else if (sel[tab] + 1 < cnt[tab]) {
			sel[tab]++;
			chsel = -1;
			id = nodes[tab][sel[tab]].id;
		}
	}
	for (; d < 0; d++) {
		if (chsel >= 0)
			chsel--;
		else if (sel[tab] > 0) {
			sel[tab]--;
			id = nodes[tab][sel[tab]].id;
			chsel = id == openid ? chn - 1 : -1;
		}
	}
}

/* c: show/hide the channels of the selected node */
static void
togglech(const Arg *a)
{
	int id = cnt[tab] ? nodes[tab][sel[tab]].id : -1;

	(void)a;
	if (id < 0)
		return;
	if (id == openid) {
		openid = chsel = -1;
		chn = 0;
		return;
	}
	openid = id;
	chsel = -1;
	loadch();
	if (openid < 0)
		msg = " no channel info (needs pactl from pulseaudio-utils + pipewire-pulse, >= 2 channels)";
}

/* space: lock channels together (all move by the same amount) or not */
static void
togglelock(const Arg *a)
{
	(void)a;
	chlock = !chlock;
	msg = chlock ? " channels locked: h/l change all channels together"
	             : " channels unlocked: h/l change only the selected channel";
}

/* volume change in percent: on a channel row only that channel, else the node */
static void
volume(const Arg *a)
{
	char cmd[512];
	size_t n;
	int i, v, d, hi, lo;

	if (chsel >= 0 && chn > 0 && openid >= 0) {
		d = a->i;
		if (chlock) { /* keep balance: limit the step so no channel leaves 0..maxvol */
			for (i = 0, hi = 0, lo = maxvol; i < chn; i++) {
				if (chv[i] > hi)
					hi = chv[i];
				if (chv[i] < lo)
					lo = chv[i];
			}
			if (d > maxvol - hi)
				d = maxvol - hi;
			if (d < -lo)
				d = -lo;
		}
		n = snprintf(cmd, sizeof cmd, "pactl set-%s-volume %d",
		    pset[chkind], chidx);
		for (i = 0; i < chn; i++) {
			v = chv[i] + (chlock || i == chsel ? d : 0);
			if (v < 0)
				v = 0;
			if (v > maxvol)
				v = maxvol;
			n += snprintf(cmd + n, sizeof cmd - n, " %d%%", v);
		}
	} else
		snprintf(cmd, sizeof cmd, "wpctl set-volume -l %d.%02d %%d %d%%%c",
		    maxvol / 100, maxvol % 100, a->i < 0 ? -a->i : a->i,
		    a->i < 0 ? '-' : '+');
	exec_cmd(cmd, 1);
	exec_cmd(postcmd, 0);
}

static void
switchtab(const Arg *a)
{
	openid = chsel = -1;
	chn = 0;
	tab = (tab + a->i + NSEC) % NSEC;
}

static void
settab(const Arg *a)
{
	openid = chsel = -1;
	chn = 0;
	tab = a->i % NSEC;
}

static void
refresh_k(const Arg *a)
{
	(void)a;
	refresh();
}

static void
quit(const Arg *a)
{
	(void)a;
	running = 0;
}

/* print name padded/truncated to w columns (utf-8 aware) */
static void
putname(const char *s, int w)
{
	int col = 0;

	while (*s && col < w) {
		putchar(*s++);
		while (((unsigned char)*s & 0xC0) == 0x80)
			putchar(*s++);
		col++;
	}
	for (; col < w; col++)
		putchar(' ');
}

static void
draw(void)
{
	struct winsize ws = { 0 };
	int W, H, rows, barw, namew, i, t, fill, n = 0, cur, curpos = 0, vol, muted, ch;
	int rn[MAXN + MAXCH], rc[MAXN + MAXCH];
	char buf[160];
	const char *name;
	Node *nd;

	ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
	W = ws.ws_col ? ws.ws_col : 80;
	H = ws.ws_row ? ws.ws_row : 24;
	rows = H - 3 < 1 ? 1 : H - 3;
	barw = W > 60 ? barwidth : W / 4;
	namew = W - barw - 10;
	if (namew < 4)
		namew = 4;

	/* flat row list: nodes, with channel rows under the opened node */
	for (i = 0; i < cnt[tab]; i++) {
		if (i == sel[tab])
			curpos = n + chsel + 1;
		rn[n] = i;
		rc[n++] = -1;
		if (nodes[tab][i].id == openid)
			for (ch = 0; ch < chn && n < (int)LEN(rn); ch++) {
				rn[n] = i;
				rc[n++] = ch;
			}
	}
	if (curpos < off[tab])
		off[tab] = curpos;
	if (curpos >= off[tab] + rows)
		off[tab] = curpos - rows + 1;

	fputs("\033[H", stdout);
	for (t = 0; t < NSEC; t++)
		printf(t == tab ? "\033[7m %s \033[0m" : " %s ", tabname[t]);
	fputs("\033[K\r\n\033[K\r\n", stdout);

	if (!cnt[tab])
		fputs(" (none)\033[K\r\n", stdout);
	for (i = off[tab]; i < n && i < off[tab] + rows; i++) {
		nd = &nodes[tab][rn[i]];
		cur = i == curpos;
		if (rc[i] < 0) {
			name = nd->name;
			vol = nd->vol;
		} else {
			snprintf(buf, sizeof buf, "   \xe2\x94\x94 %s", chname[rc[i]]);
			name = buf;
			vol = chv[rc[i]];
		}
		muted = nd->muted;
		if (cur)
			fputs("\033[7m", stdout);
		printf("%c ", rc[i] < 0 && nd->def ? '*' : ' ');
		putname(name, namew);
		putchar(' ');
		if (muted)
			fputs("\033[2m", stdout);
		fill = vol * barw / maxvol;
		if (fill > barw)
			fill = barw;
		for (t = 0; t < barw; t++)
			putchar(t < fill ? '#' : '-');
		printf("\033[22m %3d%% %c", vol, rc[i] < 0 ? (muted ? 'M' : ' ') : (chlock ? 'L' : ' '));
		fputs("\033[0m\033[K\r\n", stdout);
	}
	fputs("\033[J", stdout);
	printf("\033[%d;1H\033[2m%.*s\033[0m", H, W, msg ? msg : statusline);
	fflush(stdout);
}

static int
readkey(void)
{
	unsigned char c, d;
	struct pollfd p = { STDIN_FILENO, POLLIN, 0 };

	if (read(STDIN_FILENO, &c, 1) <= 0)
		return -1;
	if (c != 27)
		return c;
	if (poll(&p, 1, 30) <= 0 || read(STDIN_FILENO, &c, 1) <= 0)
		return K_ESC;
	if (c != '[' && c != 'O')
		return K_ESC;
	d = 0;
	while (poll(&p, 1, 30) > 0 && read(STDIN_FILENO, &c, 1) > 0) {
		d = c;
		if (c >= 0x40 && c <= 0x7e)
			break;
	}
	switch (d) {
	case 'A': return K_UP;
	case 'B': return K_DOWN;
	case 'C': return K_RIGHT;
	case 'D': return K_LEFT;
	}
	return 0;
}

static void
cleanup(void)
{
	fputs("\033[?25h\033[?1049l", stdout);
	fflush(stdout);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
}

static void
onsig(int sig)
{
	(void)sig;
	running = 0;
}

int
main(void)
{
	struct termios raw;
	struct sigaction sa = { 0 };
	struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
	size_t i;
	int k, r;

	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		fputs("smix: not a terminal\n", stderr);
		return 1;
	}
	tcgetattr(STDIN_FILENO, &orig);
	raw = orig;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
	atexit(cleanup);
	fputs("\033[?1049h\033[?25l", stdout);

	sa.sa_handler = onsig; /* no SA_RESTART: poll() must wake up */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	refresh();
	while (running) {
		draw();
		r = poll(&p, 1, refreshms);
		if (r > 0 && (p.revents & POLLIN)) {
			if ((k = readkey()) < 0)
				break;
			msg = NULL;
			for (i = 0; i < LEN(keys); i++)
				if (keys[i].key == k && keys[i].fn)
					keys[i].fn(&keys[i].arg);
			refresh();
		} else if (r == 0)
			refresh();
	}
	return 0;
}
