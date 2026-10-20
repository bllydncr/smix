/* smix config: edit, then `make` (config.h is copied from this file once) */

/* channels start locked together (like pulsemixer); 0 = unlocked */
#define CHLOCK 1

static const int barwidth  = 30;    /* bar width in columns                  */
static const int maxvol    = 150;   /* volume (%) shown as a full bar        */
static const int refreshms = 1000;  /* re-poll wpctl every N ms              */

/* run (without waiting) after every `run` action; "" to disable.
 * handy for status bars, e.g. "pkill -RTMIN+10 dwmblocks"                  */
static const char *postcmd = "";

static const char *statusline =
	" j/k select  h/l vol  c channels  space lock  m mute  d default  1-3/tab section  r refresh  q quit";

/*
 * Actions:
 *   run    {.s = "cmd"}  run cmd via sh, wait, refresh afterwards
 *   spawn  {.s = "cmd"}  run cmd detached (GUI programs, long scripts)
 *   movesel{.i = n}      move selection (also steps through opened channels)
 *   volume {.i = pct}    change volume by pct; on a channel row only that channel
 *   togglech             show/hide channels of the selected node
 *   togglelock           lock channels together / adjust one channel alone
 *   switchtab{.i = n}    next/prev section   settab{.i = n}  absolute section
 *   quit / refresh
 *
 * In every command: %d  -> id of selected node
 * Environment for scripts: $SMIX_ID  $SMIX_NAME  $SMIX_TAB
 *
 * Special keys: K_UP K_DOWN K_LEFT K_RIGHT K_ESC, '\t', '\n' (enter)
 */
#define VOL(arg) "wpctl set-volume -l 1.5 %d " arg

static const Key keys[] = {
	/* key        function    argument */
	{ 'q',        quit,       {0} },
	{ K_ESC,      quit,       {0} },
	{ 'r',        refresh_k,  {0} },

	{ 'j',        movesel,    {.i = +1} },
	{ K_DOWN,     movesel,    {.i = +1} },
	{ 'k',        movesel,    {.i = -1} },
	{ K_UP,       movesel,    {.i = -1} },

	{ '\t',       switchtab,  {.i = +1} },
	{ '1',        settab,     {.i = 0} },
	{ '2',        settab,     {.i = 1} },
	{ '3',        settab,     {.i = 2} },

	/* volume: node row -> wpctl, channel row -> that channel only (pactl) */
	{ 'l',        volume,     {.i = +5} },
	{ K_RIGHT,    volume,     {.i = +5} },
	{ 'L',        volume,     {.i = +1} },
	{ 'h',        volume,     {.i = -5} },
	{ K_LEFT,     volume,     {.i = -5} },
	{ 'H',        volume,     {.i = -1} },
	{ 'c',        togglech,   {0} },
	{ ' ',        togglelock, {0} },
	{ '\n',       togglech,   {0} },
	{ '0',        run,        {.s = VOL("1.0")} },
	{ 'm',        run,        {.s = "wpctl set-mute %d toggle"} },
	{ 'd',        run,        {.s = "wpctl set-default %d"} },

	{ 'p',        spawn,      {.s = "pavucontrol"} },

	/* examples:
	{ 'x',        run,        {.s = "notify-send \"$SMIX_NAME\" \"id $SMIX_ID\""} },
	{ 'n',        run,        {.s = "~/.local/bin/mylauncher.sh %d"} },
	*/
};
