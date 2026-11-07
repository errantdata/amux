#define FD_SET_MAX(fd, set, maxfd) do { \
		FD_SET(fd, set);        \
		if (fd > maxfd)         \
			maxfd = fd;     \
	} while (0)

static Client *client_malloc(int socket) {
	Client *c = calloc(1, sizeof(Client));
	if (!c)
		return NULL;
	c->socket = socket;
	return c;
}

static void client_free(Client *c) {
	if (c && c->socket > 0)
		close(c->socket);
	free(c);
}

/* Remove c from the client list; safe whatever its current position is. */
static void server_unlink_client(Client *c) {
	for (Client **p = &server.clients; *p; p = &(*p)->next) {
		if (*p == c) {
			*p = c->next;
			return;
		}
	}
}

static void server_sink_client(void) {
	if (!server.clients || !server.clients->next)
		return;
	Client *target = server.clients;
	server.clients = target->next;
	Client *dst = server.clients;
	while (dst->next)
		dst = dst->next;
	target->next = NULL;
	dst->next = target;
}

static void server_mark_socket_exec(bool exec, bool usr) {
	struct stat sb;
	if (stat(sockaddr.sun_path, &sb) == -1)
		return;
	mode_t mode = sb.st_mode;
	mode_t flag = usr ? S_IXUSR : S_IXGRP;
	if (exec)
		mode |= flag;
	else
		mode &= ~flag;
	chmod(sockaddr.sun_path, mode);
}

static int server_create_socket(const char *name) {
	if (!set_socket_name(&sockaddr, name))
		return -1;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1)
		return -1;
	socklen_t socklen = offsetof(struct sockaddr_un, sun_path) + strlen(sockaddr.sun_path) + 1;
	mode_t mask = umask(S_IXUSR|S_IRWXG|S_IRWXO);
	int r = bind(fd, (struct sockaddr*)&sockaddr, socklen);
	umask(mask);

	if (r == -1) {
		close(fd);
		return -1;
	}

	if (listen(fd, 5) == -1) {
		unlink(sockaddr.sun_path);
		close(fd);
		return -1;
	}

	return fd;
}

static int server_set_socket_non_blocking(int sock) {
	int flags;
	if ((flags = fcntl(sock, F_GETFL, 0)) == -1)
		flags = 0;
    	return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

static bool server_read_pty(Packet *pkt) {
	pkt->type = MSG_CONTENT;
	ssize_t len = read(server.pty, pkt->u.msg, sizeof(pkt->u.msg));
	if (len > 0)
		pkt->len = len;
	else if (len == 0)
		server.running = false;
	else if (len == -1 && errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK)
		server.running = false;
	print_packet("server-read-pty:", pkt);
	return len > 0;
}

/* Queue client input for the application. Never blocks: the pty master is
 * non-blocking and drained via select() write-readiness, so a large paste (or
 * a piped-in file in passthrough mode) into an application that has stopped
 * reading its stdin lands here instead of stalling the whole server loop.
 * The queue is bounded by PTYBUF_HIGHWATER, above which the server stops
 * reading client sockets and the back-pressure reaches the sender. */
static bool server_write_pty(Packet *pkt) {
	print_packet("server-write-pty:", pkt);
	if (!buffer_append(&server.pty_out, pkt->u.msg, pkt->len)) {
		debug("FAILED\n");
		server.running = false;
		return false;
	}
	return true;
}

/* Drain queued input into the pty as far as it accepts; never blocks. */
static void server_flush_pty(void) {
	if (buffer_pending(&server.pty_out) == 0)
		return;
	if (buffer_flush(&server.pty_out, server.pty) == -1) {
		debug("server-flush-pty: FAILED\n");
		buffer_free(&server.pty_out);   /* app is gone; drop the backlog */
		server.running = false;
	}
}

/* Queue a packet for delivery to a client; never blocks. For low-priority
 * observers the queue is bounded and whole frames are dropped past the cap,
 * so a slow observer can neither stall the session nor corrupt its framing
 * (we only ever drop entire packets, never a partial frame). */
static bool server_enqueue_packet(Client *c, Packet *pkt) {
	print_packet("server-queue:", pkt);
	if ((c->flags & CLIENT_LOWPRIORITY) &&
	    buffer_pending(&c->out) > OUTBUF_LOWPRIO_CAP)
		return true;
	if (!buffer_append(&c->out, (char *)pkt, packet_size(pkt))) {
		c->state = STATE_DISCONNECTED;
		return false;
	}
	return true;
}

/* Drain a client's pending output as far as the socket allows; never blocks. */
static void server_flush_client(Client *c) {
	if (buffer_pending(&c->out) > 0 && buffer_flush(&c->out, c->socket) == -1)
		c->state = STATE_DISCONNECTED;
}

/* ---- full-history ring -------------------------------------------------- *
 * All pty output is appended here (even while detached). Each client holds a
 * monotonic cursor (ring_pos) into the logical byte stream; the last RING_SIZE
 * bytes are physically retained, and a reattaching client starts as far back
 * as its replay window reaches. */

static void server_ring_init(void) {
	server.ring_total = 0;
	char tmpl[] = "/tmp/.mux-scrollback-XXXXXX";
	int fd = mkstemp(tmpl);
	if (fd != -1) {
		unlink(tmpl);                 /* anonymous: cleaned up on exit */
		if (ftruncate(fd, RING_SIZE) == 0) {
			void *p = mmap(NULL, RING_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
			if (p != MAP_FAILED) {
				server.ring = p;
				close(fd);
				return;
			}
		}
		close(fd);
	}
	server.ring = malloc(RING_SIZE);  /* fallback: anonymous RAM */
	if (!server.ring)
		die("server-ring-init");
}

static uint64_t server_ring_oldest(void) {
	return server.ring_total > RING_SIZE ? server.ring_total - RING_SIZE : 0;
}

/* Append live pty output to the ring (len <= one packet payload). */
static void server_ring_record(const char *data, size_t len) {
	size_t off = (size_t)(server.ring_total % RING_SIZE);
	size_t first = RING_SIZE - off;
	if (first > len)
		first = len;
	memcpy(server.ring + off, data, first);
	if (len > first)
		memcpy(server.ring, data + first, len - first);  /* wrap */
	server.ring_total += len;
	term_scan(&server.modes, data, len);
}

/* A cut made on the byte budget alone lands anywhere, including inside an
 * escape sequence or a UTF-8 character, whose tail would then be printed as
 * text at the top of the replay. Move it up to the next sequence or line. */
static uint64_t server_ring_resync(uint64_t pos, uint64_t end) {
	if (pos == 0)
		return pos;                   /* the start of the stream, not a cut */
	uint64_t max = end - pos > REPLAY_RESYNC_BYTES ? pos + REPLAY_RESYNC_BYTES : end;
	for (uint64_t p = pos; p < max; p++) {
		char ch = server.ring[p % RING_SIZE];
		if (ch == '\033' || ch == '\n' || ch == '\r')
			return p;
	}
	return pos;
}

/* Pick a client's starting cursor: walk back from `end` over `lines` newlines,
 * never further than `bytes` and never past the oldest retained byte. With
 * lines == 0 the byte budget alone decides (0 = no replay, UINT64_MAX =
 * everything still retained). Replaying only what the terminal can actually
 * retain is what makes reattach land on the live prompt at once instead of
 * rendering the whole ring first. */
static uint64_t server_ring_replay_start(uint64_t end, uint32_t lines, uint64_t bytes) {
	uint64_t limit = end - server_ring_oldest();   /* physically available */
	if (bytes < limit)
		limit = bytes;
	uint64_t seen = 0, back = 0;
	while (lines > 0 && back < limit) {
		if (server.ring[(end - back - 1) % RING_SIZE] == '\n' && ++seen > lines)
			return end - back;  /* stop *after* the newline closing the older line */
		back++;
	}
	return server_ring_resync(end - limit, end);
}

/* Back up from `end` to just past the previous newline, or to `floor` if there
 * is none. What an application writes on its way to the alternate screen is
 * setup, not output: modes, which are tracked anyway, and queries, which a
 * terminal would answer as if the user had typed the reply. It follows the
 * last line of real output, so ending a replay on a line boundary drops it. */
static uint64_t server_ring_line_end(uint64_t end, uint64_t floor) {
	while (end > floor && server.ring[(end - 1) % RING_SIZE] != '\n')
		end--;
	return end;
}

/* A replay must not begin inside an alternate-screen session that has since
 * ended: those bytes are repaints of a screen that no longer exists, and a
 * full-screen application writes so few newlines that the walk back from the
 * head goes straight through them. If the first alternate-screen sequence in
 * the window is an exit, the window started inside one: skip past it. */
static uint64_t server_ring_skip_alt(uint64_t start, uint64_t end) {
	if (!server.modes.alt_first)
		return start;                 /* never used it: nothing to find */
	TermModes window = { .pos = start };
	while (window.pos < end && !window.alt_first) {
		size_t off = (size_t)(window.pos % RING_SIZE);
		size_t chunk = 64 * 1024;
		if (chunk > end - window.pos)
			chunk = (size_t)(end - window.pos);
		if (chunk > RING_SIZE - off)
			chunk = RING_SIZE - off;      /* don't span the physical wrap */
		term_scan(&window, server.ring + off, chunk);
	}
	return window.alt_first == 'l' ? window.alt_first_end : start;
}

/* The write head this client is streaming towards. */
static uint64_t server_client_end(Client *c) {
	if (c->flags & CLIENT_HISTORY)
		return c->history_end;
	return c->replay_end ? c->replay_end : server.ring_total;
}

/* Does this client still have ring bytes owed to it? Used to ask select() for
 * write readiness even when the out buffer is momentarily empty: a flush that
 * drains it completely would otherwise leave nothing to wake the loop, and an
 * idle application never wakes it either, stalling the client mid-replay. */
static bool server_client_owes_data(Client *c) {
	return c->attach_seen && c->ring_pos < server_client_end(c);
}

/* Send a client bytes of our own. They set up its terminal and are not part of
 * the session's history, so they never enter the ring. */
static void server_send_text(Client *c, const char *text) {
	Packet pkt = { .type = MSG_CONTENT, .len = strlen(text) };
	memcpy(pkt.u.msg, text, pkt.len);
	if (pkt.len > 0)
		server_enqueue_packet(c, &pkt);
}

/* Send a client the terminal modes the application has set. */
static void server_send_modes(Client *c) {
	char seq[TERM_SEQ_MAX];
	term_restore(&server.modes, seq);
	server_send_text(c, seq);
}

/* The primary-screen history is out, so this terminal can join the application
 * on the alternate screen. Everything the application has drawn there is left
 * unsent: those bytes are diffs against a screen this terminal never had, and
 * a long session holds nothing else (no newlines at all, tens of megabytes of
 * them, and its startup queries, which a terminal would answer as if typed).
 * The application holds the real screen, so ask it for that instead. */
static void server_finish_replay(Client *c) {
	c->replay_end = 0;
	server_send_modes(c);
	if (!server.modes.alt || !server.running) {
		/* It left during the replay, so that screen is gone for good. */
		if (c->ring_pos < server.modes.alt_end)
			c->ring_pos = server.modes.alt_end;
		return;
	}
	c->ring_pos = term_join_pos(&server.modes);
	if (!(c->flags & CLIENT_PASSTHROUGH))
		server.redraw_wanted = true;
}

/* Feed a client from the ring (history then live, unified) until its output
 * queue reaches the high-water mark or it has caught up to the write head. */
static void server_pump_client(Client *c) {
	uint64_t oldest = server_ring_oldest();
	if (c->ring_pos < oldest)
		c->ring_pos = oldest;             /* fell >RING_SIZE behind: skip gap */
	/* A dump stops at the write head as it was when it attached; a live
	 * client follows the head wherever it goes. */
	uint64_t end = server_client_end(c);
	while (c->ring_pos < end &&
	       buffer_pending(&c->out) < OUTBUF_HIGHWATER) {
		size_t off = (size_t)(c->ring_pos % RING_SIZE);
		uint64_t avail = end - c->ring_pos;
		size_t chunk = sizeof(((Packet *)0)->u.msg);
		if (chunk > avail)
			chunk = (size_t)avail;
		if (chunk > RING_SIZE - off)
			chunk = RING_SIZE - off;      /* don't span the physical wrap */
		Packet pkt = { .type = MSG_CONTENT, .len = chunk };
		memcpy(pkt.u.msg, server.ring + off, chunk);
		if (!server_enqueue_packet(c, &pkt))
			break;
		c->ring_pos += chunk;
	}
	if (c->replay_end && c->ring_pos >= c->replay_end)
		server_finish_replay(c);
}

/* Decide what a newly attached client gets to see. */
static void server_attach_client(Client *c, Packet *pkt) {
	/* a short attach packet is what an older client means by a full replay */
	bool bounded = pkt->len >= sizeof(pkt->u.attach);
	uint32_t lines = bounded ? pkt->u.attach.lines : 0;
	uint64_t bytes = bounded ? pkt->u.attach.bytes : UINT64_MAX;

	c->flags = pkt->u.attach.flags;   /* overlays the legacy u.i */
	c->attach_seen = true;
	if (c->flags & (CLIENT_LOWPRIORITY|CLIENT_HISTORY))
		server_sink_client();

	if (c->flags & CLIENT_HISTORY) {
		c->ring_pos = server_ring_replay_start(server.ring_total, lines, bytes);
		/* Freeze the end point of a dump at attach time (see history_end). */
		c->history_end = server.ring_total;
		return;
	}

	/* Connected before the application had written a byte, so this client
	 * has missed nothing and there is nothing to reconstruct, however far
	 * the application got in the moment it took the attach to arrive: what
	 * is in the ring is only live output that got here first. */
	if (c->ring_pos == 0) {
		server_send_text(c, TERM_CLEAR);
		return;
	}

	/* A replay runs up to the live head, unless the application is on the
	 * alternate screen: then it stops where the application went there. */
	bool on_alt = server.modes.alt && server.running;
	uint64_t end = server.ring_total;
	if (on_alt) {
		uint64_t floor = server_ring_oldest();
		end = server.modes.alt_pos > floor ? server.modes.alt_pos : floor;
		if (end - floor > bytes)
			floor = end - bytes;
		end = server_ring_line_end(end, floor);
	}
	c->ring_pos = server_ring_skip_alt(server_ring_replay_start(end, lines, bytes), end);

	/* Leave the screen alone if nothing is going to be drawn on it: the user
	 * gets it back, untouched, when they detach from the alternate screen. */
	bool replaying = c->ring_pos < end;
	if (replaying || !on_alt)
		server_send_text(c, TERM_CLEAR);
	if (!on_alt)
		server_send_modes(c);
	else if (replaying)
		c->replay_end = end;
	else
		server_finish_replay(c);
}

static uint64_t server_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Make the application repaint its whole screen. SIGWINCH alone will not do:
 * anything that compares the new size with the old one, which includes every
 * Node application, ignores a signal that reports no change, and so does an
 * application with nothing it believes needs drawing. Only a real size change
 * is sure to work, so take a row away and, once it has reacted, give it back.
 * tmux does the same thing for the same reason. */
static void server_redraw_begin(void) {
	server.redraw_wanted = false;
	struct winsize ws = server.winsize;
	ws.ws_row += ws.ws_row > 1 ? -1 : 1;
	if (ioctl(server.pty, TIOCSWINSZ, &ws) == -1)
		return;
	server.redraw_limit = server_now_ms() + REDRAW_TIMEOUT_MS;
	server.redraw_deadline = server.redraw_limit;
}

/* Output pushes the end of a repaint back, within limits: see REDRAW_SETTLE_MS. */
static void server_redraw_output(void) {
	if (server.redraw_deadline) {
		uint64_t quiet = server_now_ms() + REDRAW_SETTLE_MS;
		server.redraw_deadline = quiet < server.redraw_limit ? quiet : server.redraw_limit;
	}
}

/* Start or finish a repaint. A client waiting for one may have joined too late
 * for the round under way, so it gets a round of its own. */
static void server_redraw_step(void) {
	if (server.redraw_deadline && server_now_ms() >= server.redraw_deadline) {
		server.redraw_deadline = 0;
		ioctl(server.pty, TIOCSWINSZ, &server.winsize);
	}
	if (server.redraw_wanted && !server.redraw_deadline)
		server_redraw_begin();
}

/* How long select() may sleep. Only a repaint in progress, and a client waiting
 * for an exit status yet to arrive, need waking up without an event. */
static struct timeval *server_select_timeout(struct timeval *tv) {
	uint64_t ms = !server.running && server.clients ? EXIT_POLL_MS : UINT64_MAX;
	if (server.redraw_deadline) {
		uint64_t now = server_now_ms();
		uint64_t left = server.redraw_deadline > now ? server.redraw_deadline - now : 0;
		if (left < ms)
			ms = left;
	}
	if (ms == UINT64_MAX)
		return NULL;
	tv->tv_sec = ms / 1000;
	tv->tv_usec = (ms % 1000) * 1000;
	return tv;
}

static void server_pty_died_handler(int sig) {
	int errsv = errno;
	pid_t pid;

	while ((pid = waitpid(-1, &server.exit_status, WNOHANG)) != 0) {
		if (pid == -1)
			break;
		server.exit_status = WEXITSTATUS(server.exit_status);
		server_mark_socket_exec(true, false);
	}

	debug("server pty died: %d\n", server.exit_status);
	errno = errsv;
}

static void server_sigterm_handler(int sig) {
	exit(EXIT_FAILURE); /* invoke atexit handler */
}

static Client *server_accept_client(void) {
	int newfd = accept(server.socket, NULL, NULL);
	if (newfd == -1 || server_set_socket_non_blocking(newfd) == -1)
		goto error;
	Client *c = client_malloc(newfd);
	if (!c)
		goto error;
	if (!server.clients)
		server_mark_socket_exec(true, true);
	c->socket = newfd;
	c->state = STATE_CONNECTED;
	/* Start at the live head and only rewind once MSG_ATTACH says how much
	 * history this client wants; a bare probe (session_exists, amux -l)
	 * therefore never triggers a replay at all. */
	c->ring_pos = server.ring_total;
	c->next = server.clients;
	server.clients = c;
	server.read_pty = true;

	Packet pkt = {
		.type = MSG_PID,
		.len = sizeof pkt.u.pid,
		.u.pid = { .pid = getpid(), .caps = SERVER_SETS_UP_SCREEN },
	};
	server_enqueue_packet(c, &pkt);
	server_flush_client(c);

	return c;
error:
	if (newfd != -1)
		close(newfd);
	return NULL;
}

static void server_sigusr1_handler(int sig) {
	int socket = server_create_socket(server.session_name);
	if (socket != -1) {
		if (server.socket)
			close(server.socket);
		server.socket = socket;
	}
}

static void server_atexit_handler(void) {
	unlink(sockaddr.sun_path);
}

/* Pull new output from the pty unless a real (non-observer) client has fallen
 * far enough behind in the ring that running ahead risks overwriting bytes it
 * has not yet consumed. This back-pressures a flooding application to the
 * speed of the slowest real terminal (and pauses it during a long replay)
 * without ever busy-spinning. With no clients we keep draining into the ring
 * so a detached app does not block and history keeps accumulating. */
static bool server_should_read_pty(void) {
	if (!server.running || !server.read_pty)
		return false;
	for (Client *c = server.clients; c; c = c->next) {
		/* Observers and history dumps must never throttle the application:
		 * `amux -H foo | less` would otherwise freeze the session for as
		 * long as the pager sits unread. They skip gaps instead. */
		if (c->flags & (CLIENT_LOWPRIORITY|CLIENT_HISTORY))
			continue;
		if (server.ring_total - c->ring_pos > RING_LAG_HIGHWATER)
			return false;
	}
	return true;
}

static void server_handle_packet(Client *c, Packet *pkt, bool *exit_delivered) {
	switch (pkt->type) {
	case MSG_CONTENT:
		server_write_pty(pkt);
		break;
	case MSG_ATTACH:
		server_attach_client(c, pkt);
		break;
	case MSG_RESIZE:
		c->state = STATE_ATTACHED;
		if (!(c->flags & CLIENT_READONLY) && c == server.clients) {
			debug("server-ioct: TIOCSWINSZ\n");
			struct winsize ws = { 0 };
			ws.ws_row = pkt->u.ws.rows;
			ws.ws_col = pkt->u.ws.cols;
			/* A real change repaints by itself, nothing to force. Going by
			 * the pty, which may be a row short just now. */
			struct winsize now;
			if (ioctl(server.pty, TIOCGWINSZ, &now) == 0 &&
			    (ws.ws_row != now.ws_row || ws.ws_col != now.ws_col))
				server.redraw_wanted = false;
			server.winsize = ws;
			/* while one row short, server_redraw_step() applies this */
			if (!server.redraw_deadline)
				ioctl(server.pty, TIOCSWINSZ, &ws);
		}
		kill(-server.pid, SIGWINCH);
		break;
	case MSG_EXIT:
		*exit_delivered = true;
		/* fall through */
	case MSG_DETACH:
		c->state = STATE_DISCONNECTED;
		break;
	default: /* ignore unknown packet */
		break;
	}
}

static void server_mainloop(void) {
	atexit(server_atexit_handler);
	server_ring_init();
	server_set_socket_non_blocking(server.pty);
	bool exit_packet_delivered = false;

	while (server.clients || !exit_packet_delivered) {
		fd_set readfds, writefds;
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);
		int fdmax = server.socket;
		FD_SET(server.socket, &readfds);

		bool read_pty = server_should_read_pty();
		if (read_pty)
			FD_SET_MAX(server.pty, &readfds, fdmax);
		if (buffer_pending(&server.pty_out) > 0)
			FD_SET_MAX(server.pty, &writefds, fdmax);

		/* Stop soliciting client input while the application is behind on
		 * reading it; the detach key is handled client side, so detaching
		 * stays instant even here. */
		bool pty_backed_up = buffer_pending(&server.pty_out) >= PTYBUF_HIGHWATER;

		for (Client *c = server.clients; c; c = c->next) {
			if (!pty_backed_up)
				FD_SET_MAX(c->socket, &readfds, fdmax);
			if (buffer_pending(&c->out) > 0 || server_client_owes_data(c))
				FD_SET_MAX(c->socket, &writefds, fdmax);
		}

		struct timeval timeout;
		if (select(fdmax+1, &readfds, &writefds, NULL, server_select_timeout(&timeout)) == -1) {
			if (errno == EINTR)
				continue;
			die("server-mainloop");
		}

		if (FD_ISSET(server.socket, &readfds))
			server_accept_client();

		/* live pty output goes into the ring; clients stream from the ring */
		if (read_pty && FD_ISSET(server.pty, &readfds)) {
			Packet server_packet;
			if (server_read_pty(&server_packet)) {
				server_ring_record(server_packet.u.msg, server_packet.len);
				server_redraw_output();
			}
		}

		for (Client *c = server.clients, *next; c; c = next) {
			next = c->next;

			/* client input -> us, resilient to partial frames */
			if (FD_ISSET(c->socket, &readfds)) {
				if (reader_fill(&c->in, c->socket) < 0) {
					c->state = STATE_DISCONNECTED;
				} else {
					Packet in;
					int got;
					while ((got = reader_next(&c->in, &in)) == 1)
						server_handle_packet(c, &in, &exit_packet_delivered);
					if (got < 0)
						c->state = STATE_DISCONNECTED;
				}
			}

			/* Stream ring -> this client (replay history, then live), but
			 * not before MSG_ATTACH has fixed the replay window: pumping at
			 * the head first and rewinding afterwards would re-send those
			 * bytes. A bare probe never attaches and so gets nothing. */
			if (c->state != STATE_DISCONNECTED && c->attach_seen)
				server_pump_client(c);

			/* Terminators, once this client has been sent everything it
			 * asked for. Gated on attach_seen: a client that has not yet
			 * told us its replay window must never be finished off early. */
			if (c->state != STATE_DISCONNECTED && c->attach_seen &&
			    !c->exit_queued &&
			    c->ring_pos >= server_client_end(c)) {
				if (c->flags & CLIENT_HISTORY) {
					Packet pkt = { .type = MSG_HISTORY_END, .len = 0 };
					server_enqueue_packet(c, &pkt);
					c->exit_queued = true;
				} else if (!server.running && server.exit_status != -1) {
					Packet pkt = {
						.type = MSG_EXIT,
						.u.i = server.exit_status,
						.len = sizeof(pkt.u.i),
					};
					server_enqueue_packet(c, &pkt);
					c->exit_queued = true;
				}
			}

			/* opportunistic non-blocking drain */
			if (c->state != STATE_DISCONNECTED)
				server_flush_client(c);

			if (c->state == STATE_DISCONNECTED) {
				/* Unlink by searching from the head rather than from a
				 * cached predecessor: handling MSG_ATTACH above may have
				 * re-ordered the list (server_sink_client), so this client
				 * is not necessarily where the iteration left it. */
				bool first = (c == server.clients);
				next = c->next;
				server_unlink_client(c);
				buffer_free(&c->out);
				buffer_free(&c->in);
				client_free(c);
				if (first && server.clients) {
					/* promote the next client: make it resize/redraw */
					Packet pkt = { .type = MSG_RESIZE, .len = 0 };
					server_enqueue_packet(server.clients, &pkt);
					server_flush_client(server.clients);
				} else if (!server.clients) {
					server_mark_socket_exec(false, true);
				}
			}
		}

		/* Left until every client has been handled: the resize that usually
		 * follows an attach may be a real change, and need no forced repaint. */
		server_redraw_step();

		/* hand queued client input to the application, non-blocking */
		server_flush_pty();
	}

	exit(EXIT_SUCCESS);
}
