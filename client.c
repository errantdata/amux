static Buffer client_out;   /* server  -> terminal (stdout)            */
static Buffer client_srv;   /* terminal -> server   (input)            */
static Buffer client_in;    /* server  -> client    (partial frames)   */
static int client_stdin_flags  = -1;
static int client_stdout_flags = -1;

static void client_sigwinch_handler(int sig) {
	client.need_resize = true;
}

static void client_queue_to_server(Packet *pkt) {
	print_packet("client-queue:", pkt);
	buffer_append(&client_srv, (char *)pkt, packet_size(pkt));
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
	if (alternate_buffer) {
		printf("\033[?25h\033[?1049l");
		fflush(stdout);
		alternate_buffer = false;
	}
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

	if (!alternate_buffer) {
		printf("\033[?1049h\033[H");
		fflush(stdout);
		alternate_buffer = true;
	}

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
	Packet apkt = {
		.type = MSG_ATTACH,
		.u.i = client.flags,
		.len = sizeof(apkt.u.i),
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
						if (!passthrough)
							buffer_append(&client_out, pkt.u.msg, pkt.len);
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
