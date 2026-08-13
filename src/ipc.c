#include "ipc.h"

#include "dispatch.h"
#include "log.h"
#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static void ipc_reply(int fd, const char *text);
static int ipc_write_all(int fd, const char *buf, size_t len);
static void ipc_command(struct inis_server *server, int fd, char *line);
static void ipc_clients(struct inis_server *server, int fd);
static void ipc_monitors(struct inis_server *server, int fd);
static void ipc_workspaces(struct inis_server *server, int fd);
static void ipc_activewindow(struct inis_server *server, int fd);
static void ipc_damage(struct inis_server *server, int fd);
static void ipc_backend(struct inis_server *server, int fd);

void
inis_ipc_init(struct inis_ipc *ipc, const char *socket_name)
{
	const char *runtime;

	memset(ipc, 0, sizeof(*ipc));
	snprintf(ipc->socket_name, sizeof(ipc->socket_name), "%s", socket_name);
	runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime == NULL || runtime[0] == '\0')
		runtime = "/tmp";
	snprintf(ipc->socket_path, sizeof(ipc->socket_path),
	    "%s/%s.sock", runtime, socket_name);
	ipc->fd = -1;
	ipc->enabled = true;
}

int
inis_ipc_start(struct inis_server *server)
{
	struct sockaddr_un addr;
	struct inis_ipc *ipc = &server->ipc;
	int fd;

	if (!ipc->enabled)
		return 0;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		inis_warn("socket failed for IPC: %s", strerror(errno));
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(ipc->socket_path) >= sizeof(addr.sun_path)) {
		inis_warn("IPC socket path too long: %s", ipc->socket_path);
		close(fd);
		return -1;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc->socket_path);
	unlink(ipc->socket_path);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		inis_warn("bind failed for IPC %s: %s", ipc->socket_path, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 16) != 0) {
		inis_warn("listen failed for IPC: %s", strerror(errno));
		close(fd);
		unlink(ipc->socket_path);
		return -1;
	}

	ipc->fd = fd;
	inis_info("IPC socket: %s", ipc->socket_path);
	return 0;
}

int
inis_ipc_get_fd(const struct inis_ipc *ipc)
{
	return ipc->fd;
}

void
inis_ipc_accept(struct inis_server *server)
{
	int client;
	char buf[INIS_MAX_LINE];
	ssize_t n;

	struct timeval tv = { 0, 200000 }; /* 200ms — prevents blocking compositor */

	client = accept(server->ipc.fd, NULL, NULL);
	if (client < 0)
		return;

	/*
	 * Both directions need the timeout: without SO_SNDTIMEO a client that
	 * connects but never reads its reply blocks write() forever and freezes
	 * the whole compositor (IPC is served on the event-loop thread).
	 */
	setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	n = recv(client, buf, sizeof(buf) - 1, 0);
	if (n <= 0) {
		close(client);
		return;
	}
	buf[n] = '\0';
	buf[strcspn(buf, "\r\n")] = '\0';
	ipc_command(server, client, buf);
	close(client);
}

void
inis_ipc_finish(struct inis_ipc *ipc)
{
	if (ipc->fd >= 0)
		close(ipc->fd);
	ipc->fd = -1;
	if (ipc->socket_path[0] != '\0')
		unlink(ipc->socket_path);
}

static void
ipc_reply(int fd, const char *text)
{
	(void)ipc_write_all(fd, text, strlen(text));
}

static int
ipc_write_all(int fd, const char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

static void
ipc_command(struct inis_server *server, int fd, char *line)
{
	char *args;

	if (strncmp(line, "dispatch ", 9) == 0) {
		char *name = line + 9;
		args = strchr(name, ' ');
		if (args != NULL)
			*args++ = '\0';
		else
			args = "";
		if (inis_dispatch(server, name, args) == 0)
			ipc_reply(fd, "ok\n");
		else
			ipc_reply(fd, "error unknown-dispatcher\n");
		return;
	}
	if (strcmp(line, "clients") == 0) {
		ipc_clients(server, fd);
		return;
	}
	if (strcmp(line, "monitors") == 0) {
		ipc_monitors(server, fd);
		return;
	}
	if (strcmp(line, "workspaces") == 0) {
		ipc_workspaces(server, fd);
		return;
	}
	if (strcmp(line, "activewindow") == 0) {
		ipc_activewindow(server, fd);
		return;
	}
	if (strcmp(line, "damage") == 0) {
		ipc_damage(server, fd);
		return;
	}
	if (strcmp(line, "backend") == 0) {
		ipc_backend(server, fd);
		return;
	}
	if (strcmp(line, "reload") == 0) {
		inis_server_reload_config(server);
		ipc_reply(fd, "ok\n");
		return;
	}

	ipc_reply(fd, "error unknown-command\n");
}

static void
ipc_clients(struct inis_server *server, int fd)
{
	char line[512];
	size_t i;

	for (i = 0; i < server->window_count; i++) {
		if (!server->windows[i].mapped)
			continue;
		snprintf(line, sizeof(line), "%zu workspace:%s app_id:%s title:%s\n",
		    i,
		    server->windows[i].workspace_index < server->workspace_count ?
		    server->workspaces[server->windows[i].workspace_index].name : "?",
		    server->windows[i].app_id, server->windows[i].title);
		ipc_reply(fd, line);
	}
}

static void
ipc_monitors(struct inis_server *server, int fd)
{
	char line[256];
	size_t i;

	for (i = 0; i < server->monitor_count; i++) {
		const struct inis_monitor *mon = &server->monitors[i];

		snprintf(line, sizeof(line),
		    "%s %dx%d+%d+%d usable:%dx%d+%d+%d reserved:%d,%d,%d,%d workspace:%s\n",
		    mon->name,
		    mon->geometry.w, mon->geometry.h,
		    mon->geometry.x, mon->geometry.y,
		    mon->usable.w, mon->usable.h,
		    mon->usable.x, mon->usable.y,
		    mon->reserved.top, mon->reserved.bottom,
		    mon->reserved.left, mon->reserved.right,
		    mon->active_workspace < server->workspace_count ?
		    server->workspaces[mon->active_workspace].name : "?");
		ipc_reply(fd, line);
	}
}

static void
ipc_workspaces(struct inis_server *server, int fd)
{
	char line[256];
	size_t i;

	for (i = 0; i < server->workspace_count; i++) {
		const char *state = server->workspaces[i].active ? "active" : "inactive";
		const char *visibility = "";

		if (server->workspaces[i].special)
			visibility = server->workspaces[i].visible ? " visible" : " hidden";

		snprintf(line, sizeof(line), "%s %s%s windows:%zu\n",
		    server->workspaces[i].name,
		    state, visibility,
		    server->workspaces[i].window_count);
		ipc_reply(fd, line);
	}
}

static void
ipc_activewindow(struct inis_server *server, int fd)
{
	char line[512];

	if (server->focused_window == NULL) {
		ipc_reply(fd, "none\n");
		return;
	}
	snprintf(line, sizeof(line), "app_id:%s title:%s workspace:%s\n",
	    server->focused_window->app_id, server->focused_window->title,
	    server->focused_window->workspace_index < server->workspace_count ?
	    server->workspaces[server->focused_window->workspace_index].name : "?");
	ipc_reply(fd, line);
}

static void
ipc_damage(struct inis_server *server, int fd)
{
	char line[512];
	size_t i;

	snprintf(line, sizeof(line),
	    "global pending:%s bounds:%dx%d+%d+%d events:%lu flushes:%lu skipped:%lu reason:%s\n",
	    server->damage.pending ? "yes" : "no",
	    server->damage.bounds.w, server->damage.bounds.h,
	    server->damage.bounds.x, server->damage.bounds.y,
	    server->damage.events, server->damage.flushes,
	    server->damage.skipped_flushes,
	    server->damage.reason[0] != '\0' ? server->damage.reason : "-");
	ipc_reply(fd, line);

	if (server->monitor_count == 0) {
		ipc_reply(fd, "no outputs\n");
		return;
	}

	for (i = 0; i < server->monitor_count; i++) {
		const struct inis_monitor *mon = &server->monitors[i];
		const struct inis_damage *d = &mon->damage;

		snprintf(line, sizeof(line),
		    "output:%s pending:%s bounds:%dx%d+%d+%d events:%lu flushes:%lu skipped:%lu reason:%s\n",
		    mon->name,
		    d->pending ? "yes" : "no",
		    d->bounds.w, d->bounds.h,
		    d->bounds.x, d->bounds.y,
		    d->events, d->flushes, d->skipped_flushes,
		    d->reason[0] != '\0' ? d->reason : "-");
		ipc_reply(fd, line);
	}
}

static void
ipc_backend(struct inis_server *server, int fd)
{
	const char *name;
	const char *running;

	name = "neuswc";
	running = server->backend.started ? "yes" : "no";

	char line[256];

	snprintf(line, sizeof(line),
	    "backend:%s running:%s wlroots:no\n",
	    name, running);
	ipc_reply(fd, line);
}
