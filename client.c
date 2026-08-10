static Buffer client_out;   /* server  -> terminal (stdout)            */
static Buffer client_srv;   /* terminal -> server   (input)            */
static Buffer client_in;    /* server  -> client    (partial frames)   */
static int client_stdin_flags  = -1;
static int client_stdout_flags = -1;
static bool alt_active = false;  /* the app has switched the real terminal
                                  * to its alternate screen (?1049/?47/?1047) */

static void client_sigwinch_handler(int sig) {
	client.need_resize = true;
}

static void client_queue_to_server(Packet *pkt) {
	print_packet("client-queue:", pkt);
	buffer_append(&client_srv, (char *)pkt, packet_size(pkt));
}

/* Track alternate-screen state by sniffing the OUTPUT stream for
 * ESC [ ? <n> (h|l) with n in {47,1047,1049}. A tiny state machine so the
 * sequence may be split across packets. We never rewrite the stream; this is
 * only so detach can return the user's terminal to a sane (primary) screen. */
static void scan_altscreen(const char *p, size_t n) {
	static int st = 0;   /* 0 ground, 1 ESC, 2 CSI, 3 CSI? */
	static int num = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char ch = (unsigned char)p[i];
		switch (st) {
		case 0: if (ch == 0x1b) st = 1; break;
		case 1: st = (ch == '[') ? 2 : (ch == 0x1b ? 1 : 0); break;
		case 2:
			if (ch == '?') { st = 3; num = 0; }
			else st = (ch == 0x1b) ? 1 : 0;
			break;
		case 3:
			if (ch >= '0' && ch <= '9') { num = num * 10 + (ch - '0'); }
			else {
				if (num == 1049 || num == 47 || num == 1047) {
					if (ch == 'h') alt_active = true;
					else if (ch == 'l') alt_active = false;
				}
				st = (ch == 0x1b) ? 1 : 0;
			}
			break;
		}
	}
}

/* Blocking control-path read, used by session_exists() on the freshly
 * connected (still blocking) socket to collect MSG_PID. */
static bool client_recv_packet(Packet *pkt) {
	if (recv_packet(server.socket, pkt)) {
		print_packet("client-recv:", pkt);
		return true;
	}
	debug("client-recv: FAILED\n");
	server.running = false;
	return false;
}

static void client_restore_terminal(void) {
	if (!has_term)
		return;
	if (client_stdin_flags != -1) {
		fcntl(STDIN_FILENO, F_SETFL, client_stdin_flags);
		client_stdin_flags = -1;
	}
	if (client_stdout_flags != -1) {
		fcntl(STDOUT_FILENO, F_SETFL, client_stdout_flags);
		client_stdout_flags = -1;
	}
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term);
	/* leave the user's terminal sane: exit any app alternate screen, show the
	 * cursor, reset attributes and scroll region, pop our window title. */
	if (alt_active) {
		printf("\033[?1049l");
		alt_active = false;
	}
	printf("\033[?25h\033[0m\033[r");
	if (server.session_name)
		printf("\033[23;2t");
	fflush(stdout);
}

static void client_setup_terminal(void) {
	if (!has_term)
		return;
	atexit(client_restore_terminal);

	cur_term = orig_term;
	cur_term.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON|IXOFF);
	cur_term.c_oflag &= ~(OPOST);
	cur_term.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	cur_term.c_cflag &= ~(CSIZE|PARENB);
	cur_term.c_cflag |= CS8;
	cur_term.c_cc[VLNEXT] = _POSIX_VDISABLE;
	cur_term.c_cc[VMIN] = 1;
	cur_term.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &cur_term);

	/* No alternate screen: we stay on the primary screen so the host
	 * terminal owns scrolling/scrollback. Reset scroll region, clear, and
	 * push a window-title status. The server then replays the full history
	 * into this (now native-scrollback) screen. */
	printf("\033[r\033[H\033[2J");
	if (server.session_name)
		printf("\033[22;2t\033]2;amux: %s\007", server.session_name);
	fflush(stdout);

	/* From here the hot path uses non-blocking write()s drained through
	 * client_out / client_srv, so a slow terminal can no longer block us.
	 * The original fd flags are restored on exit by client_restore_terminal. */
	client_stdin_flags  = fcntl(STDIN_FILENO,  F_GETFL, 0);
	client_stdout_flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
	if (client_stdin_flags != -1)
		fcntl(STDIN_FILENO,  F_SETFL, client_stdin_flags  | O_NONBLOCK);
	if (client_stdout_flags != -1)
		fcntl(STDOUT_FILENO, F_SETFL, client_stdout_flags | O_NONBLOCK);
}

/* amux -H: write the session's full retained history to stdout and exit.
 * Attaching normally only replays a bounded tail; this is how the rest of the
 * ring is reached (`amux -H work | less`, or piped to a file). The socket
 * stays blocking: the server bounds what it queues for us and never waits on
 * us, so a slow consumer here can neither spin nor stall the session. */
static int client_dump_history(const char *name) {
	if ((server.socket = session_connect(name)) == -1)
		return -1;
	Packet apkt = {
		.type = MSG_ATTACH,
		.u.attach = {
			.flags = CLIENT_READONLY | CLIENT_HISTORY,
			.lines = 0,
			.bytes = UINT64_MAX,   /* everything still retained */
		},
		.len = sizeof(apkt.u.attach),
	};
	if (write_all(server.socket, (char *)&apkt, packet_size(&apkt)) == -1) {
		close(server.socket);
		return -1;
	}

	int ret = -1;
	Packet pkt;
	while (client_recv_packet(&pkt)) {
		if (pkt.type == MSG_CONTENT) {
			if (write_all(STDOUT_FILENO, pkt.u.msg, pkt.len) != (ssize_t)pkt.len)
				break;
		} else if (pkt.type == MSG_HISTORY_END) {
			ret = 0;
			break;
		}
		/* MSG_PID and anything else: ignore */
	}
	close(server.socket);
	return ret;
}

static int client_mainloop(void) {
	sigset_t emptyset, blockset;
	sigemptyset(&emptyset);
	sigemptyset(&blockset);
	sigaddset(&blockset, SIGWINCH);
	sigprocmask(SIG_BLOCK, &blockset, NULL);

	/* a process may attach more than once (action 'A' retry); start clean */
	client_in.len  = client_in.off  = 0;
	client_out.len = client_out.off = 0;
	client_srv.len = client_srv.off = 0;

	client.need_resize = true;
	/* Ask for a bounded slice of history (see -s). The server holds the full
	 * ring either way; replaying more than the terminal can retain would only
	 * scroll off the top, at the cost of rendering every byte of it first. */
	Packet apkt = {
		.type = MSG_ATTACH,
		.u.attach = {
			.flags = client.flags,
			.lines = replay_lines,
			.bytes = replay_bytes,
		},
		.len = sizeof(apkt.u.attach),
	};
	client_queue_to_server(&apkt);

	while (server.running) {
		fd_set readfds, writefds;
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);

		/* Solicit more output only while the terminal is not backed up;
		 * otherwise stop reading and let the server back-pressure the app. */
		if (buffer_pending(&client_out) < OUTBUF_HIGHWATER)
			FD_SET(server.socket, &readfds);
		/* Likewise stop reading stdin once the server has stopped draining
		 * us, so a firehose (cat big-file | amux -p) back-pressures its
		 * writer instead of piling up in our memory. The cap is far above
		 * any interactive paste, so the detach key never waits on it. */
		if (buffer_pending(&client_srv) < SRCBUF_HIGHWATER)
			FD_SET(STDIN_FILENO, &readfds);
		if (buffer_pending(&client_srv) > 0)
			FD_SET(server.socket, &writefds);
		if (buffer_pending(&client_out) > 0)
			FD_SET(STDOUT_FILENO, &writefds);

		if (client.need_resize) {
			struct winsize ws;
			if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != -1) {
				Packet rpkt = {
					.type = MSG_RESIZE,
					.u = { .ws = { .rows = ws.ws_row, .cols = ws.ws_col } },
					.len = sizeof(rpkt.u.ws),
				};
				client_queue_to_server(&rpkt);
				client.need_resize = false;
			}
		}

		if (pselect(server.socket+1, &readfds, &writefds, NULL, NULL, &emptyset) == -1) {
			if (errno == EINTR)
				continue;
			die("client-mainloop");
		}

		/* terminal input -> server */
		if (FD_ISSET(STDIN_FILENO, &readfds)) {
			char buf[4096];
			ssize_t len = read(STDIN_FILENO, buf, sizeof buf);
			if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				die("client-stdin");
			if (len == 0) {               /* stdin EOF (e.g. passthrough pipe) */
				buffer_flush(&client_srv, server.socket);
				close(server.socket);
				return -1;
			} else if (len > 0) {
				if (KEY_REDRAW && buf[0] == KEY_REDRAW) {
					client.need_resize = true;
				} else if (buf[0] == KEY_DETACH) {
					Packet dp = { .type = MSG_DETACH, .len = 0 };
					client_queue_to_server(&dp);
					buffer_flush(&client_srv, server.socket);
					close(server.socket);
					return -1;
				} else if (!(client.flags & CLIENT_READONLY)) {
					/* chunk into <=4080-byte content frames */
					size_t off = 0;
					while (off < (size_t)len) {
						Packet ip = { .type = MSG_CONTENT };
						size_t chunk = (size_t)len - off;
						if (chunk > sizeof(ip.u.msg))
							chunk = sizeof(ip.u.msg);
						ip.len = chunk;
						memcpy(ip.u.msg, buf + off, chunk);
						client_queue_to_server(&ip);
						off += chunk;
					}
				}
			}
		}

		/* server -> terminal, resilient to partial frames */
		if (FD_ISSET(server.socket, &readfds)) {
			if (reader_fill(&client_in, server.socket) < 0) {
				server.running = false;
			} else {
				Packet pkt;
				int got;
				while ((got = reader_next(&client_in, &pkt)) == 1) {
					if (pkt.type == MSG_CONTENT) {
						if (!passthrough) {
							scan_altscreen(pkt.u.msg, pkt.len);
							buffer_append(&client_out, pkt.u.msg, pkt.len);
						}
					} else if (pkt.type == MSG_RESIZE) {
						client.need_resize = true;
					} else if (pkt.type == MSG_EXIT) {
						client_queue_to_server(&pkt);   /* echo to server */
						buffer_flush(&client_srv, server.socket);
						/* drain remaining output with a blocking write since
						 * we are leaving anyway */
						if (has_term && client_stdout_flags != -1)
							fcntl(STDOUT_FILENO, F_SETFL, client_stdout_flags);
						if (buffer_pending(&client_out) > 0)
							write_all(STDOUT_FILENO,
							          client_out.data + client_out.off,
							          buffer_pending(&client_out));
						close(server.socket);
						return pkt.u.i;
					}
					/* MSG_PID and anything else: ignore */
				}
				if (got < 0)
					server.running = false;   /* framing error */
			}
		}

		/* drain queues without blocking */
		if (buffer_pending(&client_out) > 0 &&
		    buffer_flush(&client_out, STDOUT_FILENO) == -1)
			server.running = false;
		if (buffer_pending(&client_srv) > 0 &&
		    buffer_flush(&client_srv, server.socket) == -1)
			server.running = false;
	}

	return -EIO;
}
