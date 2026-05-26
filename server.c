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

static void server_sink_client() {
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

static bool server_write_pty(Packet *pkt) {
	print_packet("server-write-pty:", pkt);
	size_t size = pkt->len;
	if (write_all(server.pty, pkt->u.msg, size) == size)
		return true;
	debug("FAILED\n");
	server.running = false;
	return false;
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
 * bytes are physically retained. A reattaching client starts at the oldest
 * retained byte, so it replays the entire kept history before live output. */

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
}

/* Feed a client from the ring (history then live, unified) until its output
 * queue reaches the high-water mark or it has caught up to the write head. */
static void server_pump_client(Client *c) {
	uint64_t oldest = server_ring_oldest();
	if (c->ring_pos < oldest)
		c->ring_pos = oldest;             /* fell >RING_SIZE behind: skip gap */
	while (c->ring_pos < server.ring_total &&
	       buffer_pending(&c->out) < OUTBUF_HIGHWATER) {
		size_t off = (size_t)(c->ring_pos % RING_SIZE);
		uint64_t avail = server.ring_total - c->ring_pos;
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
	c->ring_pos = server_ring_oldest();   /* replay the full kept history */
	c->next = server.clients;
	server.clients = c;
	server.read_pty = true;

	Packet pkt = {
		.type = MSG_PID,
		.len = sizeof pkt.u.l,
		.u.l = getpid(),
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
		if (c->flags & CLIENT_LOWPRIORITY)
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
		c->flags = pkt->u.i;
		if (c->flags & CLIENT_LOWPRIORITY)
			server_sink_client();
		break;
	case MSG_RESIZE:
		c->state = STATE_ATTACHED;
		if (!(c->flags & CLIENT_READONLY) && c == server.clients) {
			debug("server-ioct: TIOCSWINSZ\n");
			struct winsize ws = { 0 };
			ws.ws_row = pkt->u.ws.rows;
			ws.ws_col = pkt->u.ws.cols;
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

		for (Client *c = server.clients; c; c = c->next) {
			FD_SET_MAX(c->socket, &readfds, fdmax);
			if (buffer_pending(&c->out) > 0)
				FD_SET_MAX(c->socket, &writefds, fdmax);
		}

		if (select(fdmax+1, &readfds, &writefds, NULL, NULL) == -1) {
			if (errno == EINTR)
				continue;
			die("server-mainloop");
		}

		if (FD_ISSET(server.socket, &readfds))
			server_accept_client();

		/* live pty output goes into the ring; clients stream from the ring */
		if (read_pty && FD_ISSET(server.pty, &readfds)) {
			Packet server_packet;
			if (server_read_pty(&server_packet))
				server_ring_record(server_packet.u.msg, server_packet.len);
		}

		for (Client **prev_next = &server.clients, *c = server.clients; c;) {
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

			/* stream ring -> this client (replay history, then live) */
			if (c->state != STATE_DISCONNECTED)
				server_pump_client(c);

			/* once the app has exited, the status is known, and this client
			 * has been sent everything, queue a single MSG_EXIT */
			if (!server.running && !c->exit_queued && server.exit_status != -1 &&
			    c->ring_pos >= server.ring_total && c->state != STATE_DISCONNECTED) {
				Packet pkt = {
					.type = MSG_EXIT,
					.u.i = server.exit_status,
					.len = sizeof(pkt.u.i),
				};
				server_enqueue_packet(c, &pkt);
				c->exit_queued = true;
			}

			/* opportunistic non-blocking drain */
			if (c->state != STATE_DISCONNECTED)
				server_flush_client(c);

			if (c->state == STATE_DISCONNECTED) {
				bool first = (c == server.clients);
				Client *t = c->next;
				buffer_free(&c->out);
				buffer_free(&c->in);
				client_free(c);
				*prev_next = c = t;
				if (first && server.clients) {
					/* promote the next client: make it resize/redraw */
					Packet pkt = { .type = MSG_RESIZE, .len = 0 };
					server_enqueue_packet(server.clients, &pkt);
					server_flush_client(server.clients);
				} else if (!server.clients) {
					server_mark_socket_exec(false, true);
				}
				continue;
			}

			prev_next = &c->next;
			c = c->next;
		}
	}

	exit(EXIT_SUCCESS);
}
