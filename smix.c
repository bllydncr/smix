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
static void quit(const Arg *a);

#include "config.h"

static Node nodes[NSEC][MAXN];
static int cnt[NSEC], sel[NSEC], off[NSEC], tab = SINK;
static volatile sig_atomic_t running = 1;
static struct termios orig;
static const char *tabname[NSEC] = { "Sinks", "Sources", "Streams" };

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
	sel[tab] += a->i;
	if (sel[tab] >= cnt[tab])
		sel[tab] = cnt[tab] - 1;
	if (sel[tab] < 0)
		sel[tab] = 0;
}

static void
switchtab(const Arg *a)
{
	tab = (tab + a->i + NSEC) % NSEC;
}

static void
settab(const Arg *a)
{
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
	int W, H, rows, barw, namew, i, t, fill, cur;
	Node *nd;

	ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
	W = ws.ws_col ? ws.ws_col : 80;
	H = ws.ws_row ? ws.ws_row : 24;
	rows = H - 3 < 1 ? 1 : H - 3;
	barw = W > 60 ? barwidth : W / 4;
	namew = W - barw - 10;
	if (namew < 4)
		namew = 4;

	if (sel[tab] < off[tab])
		off[tab] = sel[tab];
	if (sel[tab] >= off[tab] + rows)
		off[tab] = sel[tab] - rows + 1;

	fputs("\033[H", stdout);
	for (t = 0; t < NSEC; t++)
		printf(t == tab ? "\033[7m %s \033[0m" : " %s ", tabname[t]);
	fputs("\033[K\r\n\033[K\r\n", stdout);

	if (!cnt[tab])
		fputs(" (none)\033[K\r\n", stdout);
	for (i = off[tab]; i < cnt[tab] && i < off[tab] + rows; i++) {
		nd = &nodes[tab][i];
		cur = i == sel[tab];
		if (cur)
			fputs("\033[7m", stdout);
		printf("%c ", nd->def ? '*' : ' ');
		putname(nd->name, namew);
		putchar(' ');
		if (nd->muted)
			fputs("\033[2m", stdout);
		fill = nd->vol * barw / maxvol;
		if (fill > barw)
			fill = barw;
		for (t = 0; t < barw; t++)
			putchar(t < fill ? '#' : '-');
		printf("\033[22m %3d%% %c", nd->vol, nd->muted ? 'M' : ' ');
		fputs("\033[0m\033[K\r\n", stdout);
	}
	fputs("\033[J", stdout);
	printf("\033[%d;1H\033[2m%.*s\033[0m", H, W, statusline);
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
			for (i = 0; i < LEN(keys); i++)
				if (keys[i].key == k && keys[i].fn)
					keys[i].fn(&keys[i].arg);
			refresh();
		} else if (r == 0)
			refresh();
	}
	return 0;
}
