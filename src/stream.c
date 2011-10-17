#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

#include <osmocom/core/timer.h>
#include <osmocom/core/select.h>
#include <osmocom/gsm/tlv.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/socket.h>

#include <osmocom/netif/stream.h>

/*
 * Client side.
 */

enum osmo_stream_client_conn_state {
        STREAM_CLIENT_LINK_STATE_NONE         = 0,
        STREAM_CLIENT_LINK_STATE_CONNECTING   = 1,
        STREAM_CLIENT_LINK_STATE_CONNECTED    = 2,
        STREAM_CLIENT_LINK_STATE_MAX
};

#define OSMO_STREAM_CLIENT_F_RECONFIG	(1 << 0)

struct osmo_stream_client_conn {
	struct osmo_fd			ofd;
	struct llist_head		tx_queue;
	struct osmo_timer_list		timer;
	enum osmo_stream_client_conn_state	state;
	const char			*addr;
	uint16_t			port;
	int (*connect_cb)(struct osmo_stream_client_conn *link);
	int (*read_cb)(struct osmo_stream_client_conn *link);
	int (*write_cb)(struct osmo_stream_client_conn *link);
	void				*data;
	int				flags;
};

void osmo_stream_client_conn_close(struct osmo_stream_client_conn *link);

static void osmo_stream_client_retry(struct osmo_stream_client_conn *link)
{
	LOGP(DLINP, LOGL_DEBUG, "connection closed\n");
	osmo_stream_client_conn_close(link);
	LOGP(DLINP, LOGL_DEBUG, "retrying in 5 seconds...\n");
	osmo_timer_schedule(&link->timer, 5, 0);
	link->state = STREAM_CLIENT_LINK_STATE_CONNECTING;
}

void osmo_stream_client_conn_close(struct osmo_stream_client_conn *link)
{
	osmo_fd_unregister(&link->ofd);
	close(link->ofd.fd);
}

static void osmo_stream_client_read(struct osmo_stream_client_conn *link)
{
	LOGP(DLINP, LOGL_DEBUG, "message received\n");

	if (link->read_cb)
		link->read_cb(link);
}

static int osmo_stream_client_write(struct osmo_stream_client_conn *link)
{
	struct msgb *msg;
	struct llist_head *lh;
	int ret;

	LOGP(DLINP, LOGL_DEBUG, "sending data\n");

	if (llist_empty(&link->tx_queue)) {
		link->ofd.when &= ~BSC_FD_WRITE;
		return 0;
	}
	lh = link->tx_queue.next;
	llist_del(lh);
	msg = llist_entry(lh, struct msgb, list);

	if (link->state == STREAM_CLIENT_LINK_STATE_CONNECTING) {
		LOGP(DLINP, LOGL_ERROR, "not connected, dropping data!\n");
		return 0;
	}

	ret = send(link->ofd.fd, msg->data, msg->len, 0);
	if (ret < 0) {
		if (errno == EPIPE || errno == ENOTCONN) {
			osmo_stream_client_retry(link);
		}
		LOGP(DLINP, LOGL_ERROR, "error to send\n");
	}
	msgb_free(msg);
	return 0;
}

static int osmo_stream_client_fd_cb(struct osmo_fd *ofd, unsigned int what)
{
	struct osmo_stream_client_conn *link = ofd->data;
	int error, ret;
	socklen_t len = sizeof(error);

	switch(link->state) {
	case STREAM_CLIENT_LINK_STATE_CONNECTING:
		ret = getsockopt(ofd->fd, SOL_SOCKET, SO_ERROR, &error, &len);
		if (ret >= 0 && error > 0) {
			osmo_stream_client_retry(link);
			return 0;
		}
		ofd->when &= ~BSC_FD_WRITE;
		LOGP(DLINP, LOGL_DEBUG, "connection done.\n");
		link->state = STREAM_CLIENT_LINK_STATE_CONNECTED;
		if (link->connect_cb)
			link->connect_cb(link);
		break;
	case STREAM_CLIENT_LINK_STATE_CONNECTED:
		if (what & BSC_FD_READ) {
			LOGP(DLINP, LOGL_DEBUG, "connected read\n");
			osmo_stream_client_read(link);
		}
		if (what & BSC_FD_WRITE) {
			LOGP(DLINP, LOGL_DEBUG, "connected write\n");
			osmo_stream_client_write(link);
		}
		break;
	default:
		break;
	}
        return 0;
}

static void link_timer_cb(void *data);

struct osmo_stream_client_conn *osmo_stream_client_conn_create(void *ctx)
{
	struct osmo_stream_client_conn *link;

	link = talloc_zero(ctx, struct osmo_stream_client_conn);
	if (!link)
		return NULL;

	link->ofd.fd = -1;
	link->ofd.when |= BSC_FD_READ | BSC_FD_WRITE;
	link->ofd.priv_nr = 0;	/* XXX */
	link->ofd.cb = osmo_stream_client_fd_cb;
	link->ofd.data = link;
	link->state = STREAM_CLIENT_LINK_STATE_CONNECTING;
	link->timer.cb = link_timer_cb;
	link->timer.data = link;
	INIT_LLIST_HEAD(&link->tx_queue);

	return link;
}

void
osmo_stream_client_conn_set_addr(struct osmo_stream_client_conn *link,
				 const char *addr)
{
	link->addr = talloc_strdup(link, addr);
	link->flags |= OSMO_STREAM_CLIENT_F_RECONFIG;
}

void
osmo_stream_client_conn_set_port(struct osmo_stream_client_conn *link,
				 uint16_t port)
{
	link->port = port;
	link->flags |= OSMO_STREAM_CLIENT_F_RECONFIG;
}

void
osmo_stream_client_conn_set_data(struct osmo_stream_client_conn *link,
				 void *data)
{
	link->data = data;
}

void *osmo_stream_client_conn_get_data(struct osmo_stream_client_conn *link)
{
	return link->data;
}

struct osmo_fd *
osmo_stream_client_conn_get_ofd(struct osmo_stream_client_conn *link)
{
	return &link->ofd;
}

void
osmo_stream_client_conn_set_connect_cb(struct osmo_stream_client_conn *link,
	int (*connect_cb)(struct osmo_stream_client_conn *link))
{
	link->connect_cb = connect_cb;
}

void
osmo_stream_client_conn_set_read_cb(struct osmo_stream_client_conn *link,
	int (*read_cb)(struct osmo_stream_client_conn *link))
{
	link->read_cb = read_cb;
}

void osmo_stream_client_conn_destroy(struct osmo_stream_client_conn *link)
{
	talloc_free(link);
}

int osmo_stream_client_conn_open(struct osmo_stream_client_conn *link)
{
	int ret;

	/* we are reconfiguring this socket, close existing first. */
	if ((link->flags & OSMO_STREAM_CLIENT_F_RECONFIG) && link->ofd.fd >= 0)
		osmo_stream_client_conn_close(link);

	link->flags &= ~OSMO_STREAM_CLIENT_F_RECONFIG;

	ret = osmo_sock_init(AF_INET, SOCK_STREAM, IPPROTO_TCP,
			     link->addr, link->port,
			     OSMO_SOCK_F_CONNECT|OSMO_SOCK_F_NONBLOCK);
	if (ret < 0) {
		if (errno != EINPROGRESS)
			return ret;
	}
	link->ofd.fd = ret;
	if (osmo_fd_register(&link->ofd) < 0) {
		close(ret);
		return -EIO;
	}
	return 0;
}

static void link_timer_cb(void *data)
{
	struct osmo_stream_client_conn *link = data;

	LOGP(DLINP, LOGL_DEBUG, "reconnecting.\n");

	switch(link->state) {
	case STREAM_CLIENT_LINK_STATE_CONNECTING:
		osmo_stream_client_conn_open(link);
	        break;
	default:
		break;
	}
}

void osmo_stream_client_conn_send(struct osmo_stream_client_conn *link,
				  struct msgb *msg)
{
	msgb_enqueue(&link->tx_queue, msg);
	link->ofd.when |= BSC_FD_WRITE;
}

int osmo_stream_client_conn_recv(struct osmo_stream_client_conn *link,
				 struct msgb *msg)
{
	int ret;

	ret = recv(link->ofd.fd, msg->data, msg->data_len, 0);
	if (ret < 0) {
		if (errno == EPIPE || errno == ECONNRESET) {
			LOGP(DLINP, LOGL_ERROR,
				"lost connection with server\n");
		}
		osmo_stream_client_retry(link);
		return ret;
	} else if (ret == 0) {
		LOGP(DLINP, LOGL_ERROR, "connection closed with server\n");
		osmo_stream_client_retry(link);
		return ret;
	}
	msgb_put(msg, ret);
	LOGP(DLINP, LOGL_DEBUG, "received %d bytes from server\n", ret);
	return ret;
}

/*
 * Server side.
 */

#define OSMO_STREAM_SERVER_F_RECONFIG	(1 << 0)

struct osmo_stream_server_link {
        struct osmo_fd                  ofd;
        const char                      *addr;
        uint16_t                        port;
        int (*accept_cb)(struct osmo_stream_server_link *link, int fd);
        void                            *data;
	int				flags;
};

static int osmo_stream_server_fd_cb(struct osmo_fd *ofd, unsigned int what)
{
	int ret;
	struct sockaddr_in sa;
	socklen_t sa_len = sizeof(sa);
	struct osmo_stream_server_link *link = ofd->data;

	ret = accept(ofd->fd, (struct sockaddr *)&sa, &sa_len);
	if (ret < 0) {
		LOGP(DLINP, LOGL_ERROR, "failed to accept from origin "
			"peer, reason=`%s'\n", strerror(errno));
		return ret;
	}
	LOGP(DLINP, LOGL_DEBUG, "accept()ed new link from %s to port %u\n",
		inet_ntoa(sa.sin_addr), link->port);

	if (link->accept_cb)
		link->accept_cb(link, ret);

	return 0;
}

struct osmo_stream_server_link *osmo_stream_server_link_create(void *ctx)
{
	struct osmo_stream_server_link *link;

	link = talloc_zero(ctx, struct osmo_stream_server_link);
	if (!link)
		return NULL;

	link->ofd.fd = -1;
	link->ofd.when |= BSC_FD_READ | BSC_FD_WRITE;
	link->ofd.cb = osmo_stream_server_fd_cb;
	link->ofd.data = link;

	return link;
}

void osmo_stream_server_link_set_addr(struct osmo_stream_server_link *link,
				      const char *addr)
{
	link->addr = talloc_strdup(link, addr);
	link->flags |= OSMO_STREAM_SERVER_F_RECONFIG;
}

void osmo_stream_server_link_set_port(struct osmo_stream_server_link *link,
				      uint16_t port)
{
	link->port = port;
	link->flags |= OSMO_STREAM_SERVER_F_RECONFIG;
}

void
osmo_stream_server_link_set_data(struct osmo_stream_server_link *link,
				 void *data)
{
	link->data = data;
}

void *osmo_stream_server_link_get_data(struct osmo_stream_server_link *link)
{
	return link->data;
}

struct osmo_fd *
osmo_stream_server_link_get_ofd(struct osmo_stream_server_link *link)
{
	return &link->ofd;
}

void osmo_stream_server_link_set_accept_cb(struct osmo_stream_server_link *link,
	int (*accept_cb)(struct osmo_stream_server_link *link, int fd))

{
	link->accept_cb = accept_cb;
}

void osmo_stream_server_link_destroy(struct osmo_stream_server_link *link)
{
	talloc_free(link);
}

int osmo_stream_server_link_open(struct osmo_stream_server_link *link)
{
	int ret;

	/* we are reconfiguring this socket, close existing first. */
	if ((link->flags & OSMO_STREAM_SERVER_F_RECONFIG) && link->ofd.fd >= 0)
		osmo_stream_server_link_close(link);

	link->flags &= ~OSMO_STREAM_SERVER_F_RECONFIG;

	ret = osmo_sock_init(AF_INET, SOCK_STREAM, IPPROTO_TCP,
			     link->addr, link->port, OSMO_SOCK_F_BIND);
	if (ret < 0)
		return ret;

	link->ofd.fd = ret;
	if (osmo_fd_register(&link->ofd) < 0) {
		close(ret);
		return -EIO;
	}
	return 0;
}

void osmo_stream_server_link_close(struct osmo_stream_server_link *link)
{
	osmo_fd_unregister(&link->ofd);
	close(link->ofd.fd);
}

struct osmo_stream_server_conn {
	struct osmo_stream_server_link	*server;
        struct osmo_fd                  ofd;
        struct llist_head               tx_queue;
        int (*closed_cb)(struct osmo_stream_server_conn *peer);
        int (*cb)(struct osmo_stream_server_conn *peer);
        void                            *data;
};

static void osmo_stream_server_conn_read(struct osmo_stream_server_conn *conn)
{
	LOGP(DLINP, LOGL_DEBUG, "message received\n");

	if (conn->cb)
		conn->cb(conn);

	return;
}

static void osmo_stream_server_conn_write(struct osmo_stream_server_conn *conn)
{
	struct msgb *msg;
	struct llist_head *lh;
	int ret;

	LOGP(DLINP, LOGL_DEBUG, "sending data\n");

	if (llist_empty(&conn->tx_queue)) {
		conn->ofd.when &= ~BSC_FD_WRITE;
		return;
	}
	lh = conn->tx_queue.next;
	llist_del(lh);
	msg = llist_entry(lh, struct msgb, list);

	ret = send(conn->ofd.fd, msg->data, msg->len, 0);
	if (ret < 0) {
		LOGP(DLINP, LOGL_ERROR, "error to send\n");
	}
	msgb_free(msg);
}

static int osmo_stream_server_conn_cb(struct osmo_fd *ofd, unsigned int what)
{
	struct osmo_stream_server_conn *conn = ofd->data;

	LOGP(DLINP, LOGL_DEBUG, "connected read/write\n");
	if (what & BSC_FD_READ)
		osmo_stream_server_conn_read(conn);
	if (what & BSC_FD_WRITE)
		osmo_stream_server_conn_write(conn);

	return 0;
}

struct osmo_stream_server_conn *
osmo_stream_server_conn_create(void *ctx, struct osmo_stream_server_link *link,
	int fd,
	int (*cb)(struct osmo_stream_server_conn *conn),
	int (*closed_cb)(struct osmo_stream_server_conn *conn), void *data)
{
	struct osmo_stream_server_conn *conn;

	conn = talloc_zero(ctx, struct osmo_stream_server_conn);
	if (conn == NULL) {
		LOGP(DLINP, LOGL_ERROR, "cannot allocate new peer in server, "
			"reason=`%s'\n", strerror(errno));
		return NULL;
	}
	conn->server = link;
	conn->ofd.fd = fd;
	conn->ofd.data = conn;
	conn->ofd.cb = osmo_stream_server_conn_cb;
	conn->ofd.when = BSC_FD_READ;
	conn->cb = cb;
	conn->closed_cb = closed_cb;
	conn->data = data;
	INIT_LLIST_HEAD(&conn->tx_queue);

	if (osmo_fd_register(&conn->ofd) < 0) {
		LOGP(DLINP, LOGL_ERROR, "could not register FD\n");
		talloc_free(conn);
		return NULL;
	}
	return conn;
}

void *osmo_stream_server_conn_get_data(struct osmo_stream_server_conn *link)
{
	return link->data;
}

struct osmo_fd *
osmo_stream_server_conn_get_ofd(struct osmo_stream_server_conn *link)
{
	return &link->ofd;
}

void osmo_stream_server_conn_destroy(struct osmo_stream_server_conn *conn)
{
	close(conn->ofd.fd);
	osmo_fd_unregister(&conn->ofd);
	if (conn->closed_cb)
		conn->closed_cb(conn);
	talloc_free(conn);
}

void osmo_stream_server_conn_send(struct osmo_stream_server_conn *conn,
				  struct msgb *msg)
{
	msgb_enqueue(&conn->tx_queue, msg);
	conn->ofd.when |= BSC_FD_WRITE;
}

int osmo_stream_server_conn_recv(struct osmo_stream_server_conn *conn,
				 struct msgb *msg)
{
	int ret;

	ret = recv(conn->ofd.fd, msg->data, msg->data_len, 0);
	if (ret < 0) {
		if (errno == EPIPE || errno == ECONNRESET) {
			LOGP(DLINP, LOGL_ERROR,
				"lost connection with server\n");
		}
		osmo_stream_server_conn_destroy(conn);
		return ret;
	} else if (ret == 0) {
		LOGP(DLINP, LOGL_ERROR, "connection closed with server\n");
		osmo_stream_server_conn_destroy(conn);
		return ret;
	}
	msgb_put(msg, ret);
	LOGP(DLINP, LOGL_DEBUG, "received %d bytes from client\n", ret);
	return ret;
}
