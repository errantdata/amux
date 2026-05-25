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

/* Pull new output from the pty only while every interactive (non-observer)
 * client can still accept it. This back-pressures a flooding application to
 * the speed of the slowest real terminal without ever busy-spinning. With no
 * clients attached we still drain (and discard) so a detached app does not
 * block on a full pty. */
static bool server_should_read_pty(void) {
	if (!server.running || !server.read_pty)
		return false;
	for (Client *c = server.clients; c; c = c->next) {
		if (c->flags & CLIENT_LOWPRIORITY)
			continue;
		if (buffer_pending(&c->out) > OUTBUF_HIGHWATER)
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

		bool pty_data = false;
		Packet server_packet;
		if (read_pty && FD_ISSET(server.pty, &readfds))
			pty_data = server_read_pty(&server_packet);

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

			/* fan fresh pty output out to this client's queue */
			if (pty_data && c->state != STATE_DISCONNECTED)
				server_enqueue_packet(c, &server_packet);

			/* once the app has exited and the status is known, queue a
			 * single MSG_EXIT after the client's remaining output */
			if (!server.running && !c->exit_queued &&
			    server.exit_status != -1 && c->state != STATE_DISCONNECTED) {
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
					/* promote the next client: make it redraw */
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
