/* ------------------------------------------------------------------ *
 *  What the application has asked of the terminal                     *
 *                                                                     *
 *  Everything an application configures with an escape sequence --    *
 *  alternate screen, mouse reporting, bracketed paste -- lives in the *
 *  terminal, and applications send it once, at startup. A session     *
 *  outlives its terminals, so one that attaches later has to be told  *
 *  all of it again: a bounded replay starts long after those bytes    *
 *  went by, and the ring may have overwritten them. Without them the  *
 *  mouse wheel never reaches a full-screen application; it scrolls    *
 *  the host terminal's own scrollback.                                *
 *                                                                     *
 *  This only ever reads the stream. Live output still reaches the     *
 *  terminal byte for byte (DESIGN.md, invariant 1); what is learned   *
 *  here is sent as a separate preamble when a terminal attaches, and  *
 *  undone when it detaches.                                           *
 * ------------------------------------------------------------------ */

#define TERM_SEQ_MAX    512  /* room for term_restore()/term_release() */
#define TERM_MAX_PARAMS 16   /* extra parameters of one sequence are ignored   */
#define TERM_CLEAR      "\033[r\033[H\033[2J"

#define TERM_MODE_SYNC  2026 /* synchronized output: see term_restore() */

/* DEC private modes followed from terminal to terminal, ascending: the mouse
 * protocols override one another and the higher number is the more capable.
 * `detach` is the value to leave behind in the user's terminal, which is also
 * what an unconfigured terminal starts with; 0 where that default belongs to
 * the user and cannot be known. (xterm alone starts with 1007 off, and turning
 * it on is harmless: left off, the wheel dies in every later pager.) */
static const struct {
	uint16_t mode;
	char detach;
} term_modes[] = {
	{ 1,    'l' },  /* application cursor keys   */
	{ 7,    'h' },  /* autowrap                  */
	{ 9,    'l' },  /* mouse: X10                */
	{ 12,    0  },  /* cursor blink              */
	{ 25,   'h' },  /* cursor visible            */
	{ 66,   'l' },  /* application keypad        */
	{ 1000, 'l' },  /* mouse: press and release  */
	{ 1002, 'l' },  /* mouse: drag               */
	{ 1003, 'l' },  /* mouse: all motion         */
	{ 1004, 'l' },  /* focus events              */
	{ 1005, 'l' },  /* mouse encoding: UTF-8     */
	{ 1006, 'l' },  /* mouse encoding: SGR       */
	{ 1007, 'h' },  /* wheel as arrow keys       */
	{ 1015, 'l' },  /* mouse encoding: urxvt     */
	{ 1016, 'l' },  /* mouse encoding: SGR pixel */
	{ 2004, 'l' },  /* bracketed paste           */
	{ TERM_MODE_SYNC, 'l' },
	{ 2027, 'l' },  /* grapheme clusters         */
	{ 2031, 'l' },  /* colour scheme reports     */
	{ 2048, 'l' },  /* in-band resize reports    */
};

typedef struct {
	/* parser, kept here so a sequence may span reads */
	enum {
		TERM_GROUND,
		TERM_ESC,
		TERM_ESC_INTER,
		TERM_CSI,
		TERM_STRING,     /* OSC, DCS, SOS, PM, APC: skipped to its end */
		TERM_STRING_ESC, /* just saw ESC inside one: maybe half of ST  */
	} st;
	char prefix;                 /* CSI private marker: < = > ?            */
	char inter;                  /* CSI intermediate byte                  */
	uint8_t nparams;             /* index of the parameter being read      */
	uint16_t params[TERM_MAX_PARAMS + 1]; /* the extra slot swallows excess */
	uint64_t pos;                /* stream offset of the next byte         */
	uint64_t seq_pos;            /* stream offset of the sequence's ESC    */

	/* what the application has asked for */
	char modes[countof(term_modes)]; /* 0 never set, else 'h' or 'l'       */
	bool keypad;                 /* application keypad (DECKPAM)           */
	uint16_t cursor_style;       /* DECSCUSR, 0 = the terminal's default   */
	uint16_t modify_keys;        /* xterm modifyOtherKeys level            */
	uint8_t kitty_depth[2];      /* kitty keyboard stack, primary and
	                              * alternate: each screen has its own     */

	bool alt;                    /* on the alternate screen                */
	uint64_t alt_pos;            /* where it was last entered              */
	uint64_t alt_end;            /* just past where it was last left       */
	char alt_first;              /* first alt sequence seen: 'h' or 'l'    */
	uint64_t alt_first_end;      /* offset just past that sequence         */
} TermModes;

static void term_set_alt(TermModes *t, bool on) {
	if (!t->alt_first) {
		t->alt_first = on ? 'h' : 'l';
		t->alt_first_end = t->pos;
	}
	if (on && !t->alt)
		t->alt_pos = t->seq_pos;
	if (!on && t->alt)
		t->alt_end = t->pos;
	t->alt = on;
}

static void term_set_mode(TermModes *t, uint16_t mode, char value) {
	if (mode == 47 || mode == 1047 || mode == 1049) {
		term_set_alt(t, value == 'h');
		return;
	}
	for (size_t i = 0; i < countof(term_modes); i++) {
		if (term_modes[i].mode == mode) {
			t->modes[i] = value;
			return;
		}
	}
}

/* RIS: the terminal is back to its defaults, so there is nothing left to
 * carry over or to undo. */
static void term_reset(TermModes *t) {
	if (t->alt)
		term_set_alt(t, false);
	memset(t->modes, 0, sizeof t->modes);
	memset(t->kitty_depth, 0, sizeof t->kitty_depth);
	t->keypad = false;
	t->cursor_style = 0;
	t->modify_keys = 0;
}

static void term_csi(TermModes *t, unsigned char final) {
	size_t count = t->nparams + 1u;
	if (count > TERM_MAX_PARAMS)
		count = TERM_MAX_PARAMS;
	uint8_t screen = t->alt;

	switch (final) {
	case 'h':
	case 'l':
		if (t->prefix == '?') {
			for (size_t i = 0; i < count; i++)
				term_set_mode(t, t->params[i], final);
		}
		break;
	case 'm':
		if (t->prefix == '>' && t->params[0] == 4)
			t->modify_keys = count > 1 ? t->params[1] : 0;
		break;
	case 'q':
		if (t->inter == ' ' && !t->prefix)
			t->cursor_style = t->params[0];
		break;
	case 'u':
		if (t->prefix == '>') {
			if (t->kitty_depth[screen] < UINT8_MAX)
				t->kitty_depth[screen]++;
		} else if (t->prefix == '<') {
			unsigned int pop = t->params[0] ? t->params[0] : 1;
			t->kitty_depth[screen] = pop < t->kitty_depth[screen] ?
			                         t->kitty_depth[screen] - pop : 0;
		}
		break;
	}
}

/* The byte after an ESC. */
static void term_esc(TermModes *t, unsigned char ch) {
	if (ch < 0x20) {             /* C0 controls act without ending a sequence */
		t->st = TERM_ESC;
		return;
	}
	t->st = TERM_GROUND;
	switch (ch) {
	case '[':
		t->st = TERM_CSI;
		t->prefix = t->inter = 0;
		t->nparams = 0;
		t->params[0] = 0;
		break;
	case ']': case 'P': case 'X': case '^': case '_':
		t->st = TERM_STRING;
		break;
	case '=':
		t->keypad = true;
		break;
	case '>':
		t->keypad = false;
		break;
	case 'c':
		term_reset(t);
		break;
	default:
		if (ch <= 0x2f)
			t->st = TERM_ESC_INTER;
		break;
	}
}

static void term_csi_byte(TermModes *t, unsigned char ch) {
	if (ch >= '0' && ch <= '9') {
		uint16_t *param = &t->params[t->nparams];
		*param = *param > (UINT16_MAX - 9) / 10 ? UINT16_MAX : *param * 10 + (ch - '0');
	} else if (ch == ';' || ch == ':') {
		if (t->nparams < TERM_MAX_PARAMS)
			t->nparams++;
		t->params[t->nparams] = 0;
	} else if (ch >= '<' && ch <= '?') {
		t->prefix = ch;
	} else if (ch >= 0x20 && ch <= 0x2f) {
		t->inter = ch;
	} else if (ch >= 0x40 && ch <= 0x7e) {
		t->st = TERM_GROUND;
		term_csi(t, ch);
	}
}

static void term_byte(TermModes *t, unsigned char ch) {
	if (ch == 0x18 || ch == 0x1a) {  /* CAN, SUB: abort whatever this was */
		t->st = TERM_GROUND;
		return;
	}
	if (ch == 0x1b) {
		t->seq_pos = t->pos - 1;
		t->st = t->st == TERM_STRING ? TERM_STRING_ESC : TERM_ESC;
		return;
	}

	switch (t->st) {
	case TERM_GROUND:
		break;
	case TERM_ESC:
		term_esc(t, ch);
		break;
	case TERM_ESC_INTER:
		if (ch >= 0x30)
			t->st = TERM_GROUND;
		break;
	case TERM_CSI:
		term_csi_byte(t, ch);
		break;
	case TERM_STRING:
		if (ch == 0x07)
			t->st = TERM_GROUND;
		break;
	case TERM_STRING_ESC:
		if (ch == '\\')
			t->st = TERM_GROUND;
		else
			term_esc(t, ch);     /* that ESC ended the string and began this */
		break;
	}
}

/* Follow a stretch of output. Text is skipped wholesale, so this costs next
 * to nothing except on bytes that are actually escape sequences. */
static void term_scan(TermModes *t, const char *data, size_t len) {
	const char *p = data, *end = data + len;
	while (p < end) {
		if (t->st == TERM_GROUND) {
			const char *esc = memchr(p, 0x1b, end - p);
			if (!esc) {
				t->pos += end - p;
				return;
			}
			t->pos += esc - p;
			p = esc;
		}
		t->pos++;
		term_byte(t, (unsigned char)*p++);
	}
}

/* Where a terminal can start following the stream without seeing half of a
 * sequence: the end of it, or the start of the sequence it stops in. */
static uint64_t term_join_pos(const TermModes *t) {
	return t->st == TERM_GROUND ? t->pos : t->seq_pos;
}

/* Append to buf, a TERM_SEQ_MAX array, of which n bytes are in use. */
#define TERM_PUT(...) (n += snprintf(buf + n, TERM_SEQ_MAX - n, __VA_ARGS__))

/* Bring a terminal up to what the application expects. A mode the application
 * turned off is sent as well, because this terminal is not necessarily in its
 * default state: a replay can stop between turning something on and off.
 *
 * Two things stay out. Synchronized output brackets a single frame, and that
 * frame is gone, so re-entering it would only freeze the terminal. Keyboard
 * encoding (the kitty protocol, modifyOtherKeys) is only ever undone: it is a
 * stack whose levels are not followed here, and plain keys are something every
 * application understands, so that is what it gets after a reattach. */
static void term_restore(const TermModes *t, char *buf) {
	size_t n = 0;
	buf[0] = '\0';
	if (t->alt)
		TERM_PUT("\033[?1049h");
	for (size_t i = 0; i < countof(term_modes); i++) {
		if (t->modes[i] && term_modes[i].mode != TERM_MODE_SYNC)
			TERM_PUT("\033[?%d%c", term_modes[i].mode, t->modes[i]);
	}
	if (t->keypad)
		TERM_PUT("\033=");
	if (t->cursor_style)
		TERM_PUT("\033[%d q", t->cursor_style);
}

/* Undo what a terminal was put through, for when it leaves the session and
 * goes back to being the user's: anything left on would be typed at their
 * shell as escape sequences on every wheel notch and mouse move. */
static void term_release(const TermModes *t, char *buf) {
	size_t n = 0;
	buf[0] = '\0';
	for (size_t i = 0; i < countof(term_modes); i++) {
		char value = t->modes[i], detach = term_modes[i].detach;
		if (value && detach && value != detach)
			TERM_PUT("\033[?%d%c", term_modes[i].mode, detach);
	}
	if (t->cursor_style)
		TERM_PUT("\033[0 q");
	if (t->modify_keys)
		TERM_PUT("\033[>4m");
	if (t->keypad)
		TERM_PUT("\033>");
	if (t->alt && t->kitty_depth[1])
		TERM_PUT("\033[<%du", t->kitty_depth[1]);
	if (t->alt)
		TERM_PUT("\033[?1049l");
	if (t->kitty_depth[0])
		TERM_PUT("\033[<%du", t->kitty_depth[0]);
}
