/* default command to execute if none is given and $AMUX_CMD is unset */
#define AMUX_CMD "dvtm"
/* default detach key, can be overriden at run time using -e option */
static char KEY_DETACH = CTRL('\\');
/* redraw key to send a SIGWINCH signal to underlying process
 * (set to 0 to disable the redraw key) */
static char KEY_REDRAW = 0;
/* Where to place the "amux" directory storing all session socket files.
 * The first directory to succeed is used. $ABDUCO_SOCKET_DIR is honoured
 * after $AMUX_SOCKET_DIR so a pre-fork configuration keeps working. */
static struct Dir {
	char *path;    /* fixed (absolute) path to a directory */
	char *env;     /* environment variable to use if (set) */
	bool personal; /* if false a user owned sub directory will be created */
} socket_dirs[] = {
	{ .env  = "AMUX_SOCKET_DIR",   false },
	{ .env  = "ABDUCO_SOCKET_DIR", false },
	{ .env  = "HOME",              true  },
	{ .env  = "TMPDIR",            false },
	{ .path = "/tmp",              false },
};
