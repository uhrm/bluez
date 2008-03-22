/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2008  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#include <glib.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <bluetooth/rfcomm.h>

#include "dbus.h"
#include "dbus-helper.h"
#include "logging.h"
#include "textfile.h"

#include "error.h"
#include "port.h"
#include "storage.h"
#include "manager.h"

#define BASE_UUID			"00000000-0000-1000-8000-00805F9B34FB"
#define SERIAL_PROXY_INTERFACE		"org.bluez.serial.Proxy"
#define BUF_SIZE			1024

/* Waiting for udev to create the device node */
#define MAX_OPEN_TRIES		5
#define OPEN_WAIT		300	/* ms */

struct pending_connect {
	DBusConnection	*conn;
	DBusMessage	*msg;
	char		*bda;		/* Destination address  */
	char		*adapter_path;	/* Adapter D-Bus path   */
	char		*pattern;	/* Connection request pattern */
	bdaddr_t	src;
	uint8_t		channel;
	guint		io_id;		/* GIOChannel watch id */
	GIOChannel	*io;		/* GIOChannel for RFCOMM connect */
	char		*dev;		/* tty device name */
	int		id;		/* RFCOMM device id */
	int		ntries;		/* Open attempts */
	int		canceled;	/* Operation canceled */
};

/* FIXME: Common file required */
static struct {
	const char	*name;
	uint16_t	class;
} serial_services[] = {
	{ "vcp",	VIDEO_CONF_SVCLASS_ID		},
	{ "pbap",	PBAP_SVCLASS_ID			},
	{ "sap",	SAP_SVCLASS_ID			},
	{ "ftp",	OBEX_FILETRANS_SVCLASS_ID	},
	{ "bpp",	BASIC_PRINTING_SVCLASS_ID	},
	{ "bip",	IMAGING_SVCLASS_ID		},
	{ "synch",	IRMC_SYNC_SVCLASS_ID		},
	{ "dun",	DIALUP_NET_SVCLASS_ID		},
	{ "opp",	OBEX_OBJPUSH_SVCLASS_ID		},
	{ "fax",	FAX_SVCLASS_ID			},
	{ "spp",	SERIAL_PORT_SVCLASS_ID		},
	{ NULL }
};

typedef enum {
	TTY_PROXY,
	UNIX_SOCKET_PROXY,
	TCP_SOCKET_PROXY,
	UNKNOWN_PROXY_TYPE = 0xFF
} proxy_type_t;

struct proxy {
	bdaddr_t	src;
	bdaddr_t	dst;
	char		*uuid128;	/* UUID 128 */
	char		*address;	/* TTY or Unix socket name */
	short int	port;		/* TCP port */
	proxy_type_t	type;		/* TTY or Unix socket */
	struct termios  sys_ti;		/* Default TTY setting */
	struct termios  proxy_ti;	/* Proxy TTY settings */
	uint8_t		channel;	/* RFCOMM channel */
	uint32_t	record_id;	/* Service record id */
	guint		listen_watch;	/* Server listen watch */
	guint		rfcomm_watch;	/* RFCOMM watch: Remote */
	guint		local_watch;	/* Local watch: TTY or Unix socket */
};

static DBusConnection *connection = NULL;
static GSList *pending_connects = NULL;
static GSList *ports_paths = NULL;
static GSList *proxies_paths = NULL;
static int rfcomm_ctl = -1;
static int sk_counter = 0;

static void proxy_free(struct proxy *prx)
{
	g_free(prx->address);
	g_free(prx->uuid128);
	g_free(prx);
}

static void pending_connect_free(struct pending_connect *pc)
{
	if (pc->conn)
		dbus_connection_unref(pc->conn);
	if (pc->msg)
		dbus_message_unref(pc->msg);
	if (pc->bda)
		g_free(pc->bda);
	if (pc->pattern)
		g_free(pc->pattern);
	if (pc->adapter_path)
		g_free(pc->adapter_path);
	if (pc->dev)
		g_free(pc->dev);
	if (pc->io_id > 0)
		g_source_remove(pc->io_id);
	if (pc->io) {
		g_io_channel_close(pc->io);
		g_io_channel_unref(pc->io);
	}
	g_free(pc);
}

static struct pending_connect *find_pending_connect_by_pattern(const char *bda,
							const char *pattern)
{
	GSList *l;

	/* Pattern can be friendly name, uuid128, record handle or channel */
	for (l = pending_connects; l != NULL; l = l->next) {
		struct pending_connect *pending = l->data;
		if (!strcasecmp(pending->bda, bda) &&
				!strcasecmp(pending->pattern, pattern))
			return pending;
	}

	return NULL;
}

static void transaction_owner_exited(const char *name, void *data)
{
	GSList *l, *tmp = NULL;
	debug("transaction owner %s exited", name);

	/* Remove all pending calls that belongs to this owner */
	for (l = pending_connects; l != NULL; l = l->next) {
		struct pending_connect *pc = l->data;
		if (strcmp(name, dbus_message_get_sender(pc->msg)) != 0) {
			tmp = g_slist_append(tmp, pc);
			continue;
		}

		if (pc->id >= 0)
			rfcomm_release(pc->id);

		pending_connect_free(pc);
	}

	g_slist_free(pending_connects);
	pending_connects = tmp;
}

static void pending_connect_remove(struct pending_connect *pc)
{
	/* Remove the connection request owner */
	name_listener_remove(pc->conn, dbus_message_get_sender(pc->msg),
				(name_cb_t) transaction_owner_exited, NULL);

	pending_connects = g_slist_remove(pending_connects, pc);
	pending_connect_free(pc);
}

static void open_notify(int fd, int err, struct pending_connect *pc)
{
	DBusMessage *reply;
	bdaddr_t dst;

	if (err) {
		/* Max tries exceeded */
		rfcomm_release(pc->id);
		error_connection_attempt_failed(pc->conn, pc->msg, err);
		return;
	}

	if (pc->canceled) {
		rfcomm_release(pc->id);
		error_canceled(pc->conn, pc->msg, "Connection canceled");
		return;
	}

	/* Reply to the requestor */
	reply = dbus_message_new_method_return(pc->msg);
	dbus_message_append_args(reply,
			DBUS_TYPE_STRING, &pc->dev,
			DBUS_TYPE_INVALID);
	send_message_and_unref(pc->conn, reply);

	/* Send the D-Bus signal */
	dbus_connection_emit_signal(pc->conn, SERIAL_MANAGER_PATH,
			SERIAL_MANAGER_INTERFACE, "ServiceConnected" ,
			DBUS_TYPE_STRING, &pc->dev,
			DBUS_TYPE_INVALID);

	str2ba(pc->bda, &dst);

	/* Add the RFCOMM connection listener */
	port_add_listener(pc->conn, pc->id, &dst, fd,
			pc->dev, dbus_message_get_sender(pc->msg));
}

static gboolean open_continue(struct pending_connect *pc)
{
	int fd;

	if (!g_slist_find(pending_connects, pc))
		return FALSE; /* Owner exited */

	fd = open(pc->dev, O_RDONLY | O_NOCTTY);
	if (fd < 0) {
		int err = errno;
		error("Could not open %s: %s (%d)",
				pc->dev, strerror(err), err);
		if (++pc->ntries >= MAX_OPEN_TRIES) {
			/* Reporting error */
			open_notify(fd, err, pc);
			pending_connect_remove(pc);
			return FALSE;
		}
		return TRUE;
	}
	/* Connection succeeded */
	open_notify(fd, 0, pc);
	pending_connect_remove(pc);
	return FALSE;
}

int port_open(struct pending_connect *pc)
{
	int fd;

	fd = open(pc->dev, O_RDONLY | O_NOCTTY);
	if (fd < 0) {
		g_timeout_add(OPEN_WAIT, (GSourceFunc) open_continue, pc);
		return -EINPROGRESS;
	}

	return fd;
}

static uint16_t str2class(const char *pattern)
{
	int i;

	for (i = 0; serial_services[i].name; i++) {
		if (strcasecmp(serial_services[i].name, pattern) == 0)
			return serial_services[i].class;
	}

	return 0;
}

int rfcomm_release(int16_t id)
{
	struct rfcomm_dev_req req;

	memset(&req, 0, sizeof(req));
	req.dev_id = id;

	/*
	 * We are hitting a kernel bug inside RFCOMM code when
	 * RFCOMM_HANGUP_NOW bit is set on request's flags passed to
	 * ioctl(RFCOMMRELEASEDEV)!
	 */
	req.flags = (1 << RFCOMM_HANGUP_NOW);

	if (ioctl(rfcomm_ctl, RFCOMMRELEASEDEV, &req) < 0) {
		int err = errno;
		error("Can't release device %d: %s (%d)",
				id, strerror(err), err);
		return -err;
	}

	return 0;
}

static int rfcomm_bind(bdaddr_t *src, bdaddr_t *dst, int16_t dev_id, uint8_t ch)
{
	struct rfcomm_dev_req req;
	int id;

	memset(&req, 0, sizeof(req));
	req.dev_id = dev_id;
	req.flags = 0;
	bacpy(&req.src, src);
	bacpy(&req.dst, dst);
	req.channel = ch;

	id = ioctl(rfcomm_ctl, RFCOMMCREATEDEV, &req);
	if (id < 0) {
		int err = errno;
		error("RFCOMMCREATEDEV failed: %s (%d)", strerror(err), err);
		return -err;
	}

	return id;
}

static gboolean rfcomm_connect_cb(GIOChannel *chan,
		GIOCondition cond, struct pending_connect *pc)
{
	struct rfcomm_dev_req req;
	int sk, err, fd, ret;
	socklen_t len;

	if (pc->canceled) {
		error_canceled(pc->conn, pc->msg, "Connection canceled");
		goto fail;
	}

	if (cond & G_IO_NVAL) {
		/* Avoid close invalid file descriptor */
		g_io_channel_unref(pc->io);
		pc->io = NULL;
		error_canceled(pc->conn, pc->msg, "Connection canceled");
		goto fail;
	}

	sk = g_io_channel_unix_get_fd(chan);
	len = sizeof(ret);
	if (getsockopt(sk, SOL_SOCKET, SO_ERROR, &ret, &len) < 0) {
		err = errno;
		error("getsockopt(SO_ERROR): %s (%d)",
				strerror(err), err);
		error_connection_attempt_failed(pc->conn, pc->msg, err);
		goto fail;
	}

	if (ret != 0) {
		error("connect(): %s (%d)", strerror(ret), ret);
		error_connection_attempt_failed(pc->conn, pc->msg, ret);
		goto fail;
	}

	debug("rfcomm_connect_cb: connected");

	memset(&req, 0, sizeof(req));
	req.dev_id = -1;
	req.flags = (1 << RFCOMM_REUSE_DLC) | (1 << RFCOMM_RELEASE_ONHUP);
	bacpy(&req.src, &pc->src);
	str2ba(pc->bda, &req.dst);
	req.channel = pc->channel;

	pc->id = ioctl(sk, RFCOMMCREATEDEV, &req);
	if (pc->id < 0) {
		err = errno;
		error("ioctl(RFCOMMCREATEDEV): %s (%d)", strerror(err), err);
		error_connection_attempt_failed(pc->conn, pc->msg, err);
		goto fail;
	}
	pc->dev	= g_new0(char, 16);
	snprintf(pc->dev, 16, "/dev/rfcomm%d", pc->id);

	/* Addressing connect port */
	fd = port_open(pc);
	if (fd < 0)
		/* Open in progress: Wait the callback */
		return FALSE;

	open_notify(fd, 0, pc);
fail:
	pending_connect_remove(pc);
	return FALSE;
}

static int rfcomm_connect(struct pending_connect *pc)
{
	struct sockaddr_rc addr;
	int sk, err;

	sk = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
	if (sk < 0)
		return -errno;

	memset(&addr, 0, sizeof(addr));
	addr.rc_family	= AF_BLUETOOTH;
	bacpy(&addr.rc_bdaddr, &pc->src);
	addr.rc_channel	= 0;

	if (bind(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0)
		goto fail;

	if (set_nonblocking(sk) < 0)
		goto fail;

	pc->io = g_io_channel_unix_new(sk);
	addr.rc_family	= AF_BLUETOOTH;
	str2ba(pc->bda, &addr.rc_bdaddr);
	addr.rc_channel	= pc->channel;

	if (connect(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		/* BlueZ returns EAGAIN eventhough it should return EINPROGRESS */
		if (!(errno == EAGAIN || errno == EINPROGRESS)) {
			error("connect() failed: %s (%d)",
					strerror(errno), errno);
			g_io_channel_unref(pc->io);
			pc->io = NULL;
			goto fail;
		}

		debug("Connect in progress");
		pc->io_id = g_io_add_watch(pc->io,
				G_IO_OUT | G_IO_ERR | G_IO_NVAL | G_IO_HUP,
				(GIOFunc) rfcomm_connect_cb, pc);
	} else {
		debug("Connect succeeded with first try");
		(void) rfcomm_connect_cb(pc->io, G_IO_OUT, pc);
	}

	return 0;
fail:
	err = errno;
	close(sk);
	errno = err;

	return -err;
}

static void record_reply(DBusPendingCall *call, void *data)
{
	struct pending_connect *pc;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	sdp_record_t *rec = NULL;
	const uint8_t *rec_bin;
	sdp_list_t *protos;
	DBusError derr;
	int len, scanned, ch, err;

	/* Owner exited? */
	if (!g_slist_find(pending_connects, data)) {
		dbus_message_unref(reply);
		return;
	}

	pc = data;
	if (pc->canceled) {
		error_canceled(pc->conn, pc->msg, "Connection canceled");
		goto fail;
	}

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {
		/* FIXME : forward error as is */
		if (dbus_error_has_name(&derr,
				"org.bluez.Error.ConnectionAttemptFailed"))
			error_connection_attempt_failed(pc->conn, pc->msg,
					EIO);
		else
			error_not_supported(pc->conn, pc->msg);

		error("GetRemoteServiceRecord: %s(%s)",
					derr.name, derr.message);
		dbus_error_free(&derr);
		goto fail;
	}

	if (!dbus_message_get_args(reply, &derr,
				DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &rec_bin, &len,
				DBUS_TYPE_INVALID)) {
		error_not_supported(pc->conn, pc->msg);
		error("%s: %s", derr.name, derr.message);
		dbus_error_free(&derr);
		goto fail;
	}

	if (len == 0) {
		error_not_supported(pc->conn, pc->msg);
		error("Invalid service record length");
		goto fail;
	}

	rec = sdp_extract_pdu(rec_bin, &scanned);
	if (!rec) {
		error("Can't extract SDP record.");
		error_not_supported(pc->conn, pc->msg);
		goto fail;
	}

	if (len != scanned || (sdp_get_access_protos(rec, &protos) < 0)) {
		error_not_supported(pc->conn, pc->msg);
		goto fail;
	}

	ch = sdp_get_proto_port(protos, RFCOMM_UUID);
	sdp_list_foreach(protos, (sdp_list_func_t) sdp_list_free, NULL);
	sdp_list_free(protos, NULL);

	if (ch < 1 || ch > 30) {
		error("Channel out of range: %d", ch);
		error_not_supported(pc->conn, pc->msg);
		goto fail;
	}
	if (dbus_message_has_member(pc->msg, "CreatePort")) {
		char path[MAX_PATH_LENGTH], port_name[16];
		const char *ppath = path;
		sdp_data_t *d;
		char *svcname = NULL;
		DBusMessage *reply;
		bdaddr_t dst;

		str2ba(pc->bda, &dst);
		err = rfcomm_bind(&pc->src, &dst, -1, ch);
		if (err < 0) {
			error_failed_errno(pc->conn, pc->msg, -err);
			goto fail;
		}
		snprintf(port_name, sizeof(port_name), "/dev/rfcomm%d", err);

		d = sdp_data_get(rec, SDP_ATTR_SVCNAME_PRIMARY);
		if (d) {
			svcname = g_new0(char, d->unitSize);
			snprintf(svcname, d->unitSize, "%.*s",
					d->unitSize, d->val.str);
		}

		port_store(&pc->src, &dst, err, ch, svcname);

		port_register(pc->conn, err, &pc->src, &dst, port_name,
			      path, svcname);
		if (svcname)
			g_free(svcname);

		ports_paths = g_slist_append(ports_paths, g_strdup(path));

		reply = dbus_message_new_method_return(pc->msg);
		dbus_message_append_args(reply,
				DBUS_TYPE_STRING, &ppath,
				DBUS_TYPE_INVALID);
		send_message_and_unref(pc->conn, reply);

		dbus_connection_emit_signal(pc->conn, SERIAL_MANAGER_PATH,
				SERIAL_MANAGER_INTERFACE, "PortCreated" ,
				DBUS_TYPE_STRING, &ppath,
				DBUS_TYPE_INVALID);
	} else {
		/* ConnectService */
		pc->channel = ch;
		err = rfcomm_connect(pc);
		if (err < 0) {
			error("RFCOMM connection failed");
			error_connection_attempt_failed(pc->conn,
					pc->msg, -err);
			goto fail;
		}

		/* Wait the connect callback */
		goto done;
	}

fail:
	pending_connect_remove(pc);
done:
	if (rec)
		sdp_record_free(rec);
	dbus_message_unref(reply);
}

static int get_record(struct pending_connect *pc, uint32_t handle,
					DBusPendingCallNotifyFunction cb)
{
	DBusMessage *msg;
	DBusPendingCall *call;

	msg = dbus_message_new_method_call("org.bluez", pc->adapter_path,
			"org.bluez.Adapter", "GetRemoteServiceRecord");
	if (!msg)
		return -1;

	dbus_message_append_args(msg,
			DBUS_TYPE_STRING, &pc->bda,
			DBUS_TYPE_UINT32, &handle,
			DBUS_TYPE_INVALID);

	if (!dbus_connection_send_with_reply(pc->conn, msg, &call, -1)) {
		error("Can't send D-Bus message.");
		dbus_message_unref(msg);
		return -1;
	}

	dbus_pending_call_set_notify(call, cb, pc, NULL);
	dbus_pending_call_unref(call);
	dbus_message_unref(msg);

	return 0;
}

static void handles_reply(DBusPendingCall *call, void *data)
{
	struct pending_connect *pc;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusError derr;
	uint32_t *phandle;
	int len;

	/* Owner exited? */
	if (!g_slist_find(pending_connects, data)) {
		dbus_message_unref(reply);
		return;
	}

	pc = data;
	if (pc->canceled) {
		error_canceled(pc->conn, pc->msg, "Connection canceled");
		goto fail;
	}

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {
		/* FIXME : forward error as is */
		if (dbus_error_has_name(&derr,
				"org.bluez.Error.ConnectionAttemptFailed"))
			error_connection_attempt_failed(pc->conn,
					pc->msg, EIO);
		else
			error_not_supported(pc->conn, pc->msg);

		error("GetRemoteServiceHandles: %s(%s)",
					derr.name, derr.message);
		dbus_error_free(&derr);
		goto fail;
	}

	if (!dbus_message_get_args(reply, &derr,
				DBUS_TYPE_ARRAY, DBUS_TYPE_UINT32, &phandle,
				&len, DBUS_TYPE_INVALID)) {
		error_not_supported(pc->conn, pc->msg);
		error("%s: %s", derr.name, derr.message);
		dbus_error_free(&derr);
		goto fail;
	}

	if (len == 0) {
		error_not_supported(pc->conn, pc->msg);
		goto fail;
	}

	if (get_record(pc, *phandle, record_reply) < 0) {
		error_not_supported(pc->conn, pc->msg);
		goto fail;
	}

	dbus_message_unref(reply);
	return;
fail:
	dbus_message_unref(reply);
	pending_connect_remove(pc);
}

static int get_handles(struct pending_connect *pc, const char *uuid,
					DBusPendingCallNotifyFunction cb)
{
	DBusMessage *msg;
	DBusPendingCall *call;

	msg = dbus_message_new_method_call("org.bluez", pc->adapter_path,
				"org.bluez.Adapter", "GetRemoteServiceHandles");
	if (!msg)
		return -1;

	dbus_message_append_args(msg,
			DBUS_TYPE_STRING, &pc->bda,
			DBUS_TYPE_STRING, &uuid,
			DBUS_TYPE_INVALID);

	if (!dbus_connection_send_with_reply(pc->conn, msg, &call, -1)) {
		error("Can't send D-Bus message.");
		dbus_message_unref(msg);
		return -1;
	}

	dbus_pending_call_set_notify(call, cb, pc, NULL);
	dbus_pending_call_unref(call);
	dbus_message_unref(msg);

	return 0;
}

static int pattern2uuid128(const char *pattern, char *uuid, size_t size)
{
	uint16_t cls;

	/* Friendly name */
	cls = str2class(pattern);
	if (cls) {
		uuid_t uuid16, uuid128;

		sdp_uuid16_create(&uuid16, cls);
		sdp_uuid16_to_uuid128(&uuid128, &uuid16);
		sdp_uuid2strn(&uuid128, uuid, size);
		return 0;
	}

	/* UUID 128*/
	if ((strlen(pattern) == 36) &&
		(strncasecmp(BASE_UUID, pattern, 3) == 0) &&
		(strncasecmp(BASE_UUID + 8, pattern + 8, 28) == 0)) {

		strncpy(uuid, pattern, size);
		return 0;
	}

	return -EINVAL;
}

static int pattern2long(const char *pattern, long *pval)
{
	char *endptr;
	long val;

	errno = 0;
	val = strtol(pattern, &endptr, 0);
	if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN)) ||
			(errno != 0 && val == 0) || (pattern == endptr)) {
		return -EINVAL;
	}

	*pval = val;

	return 0;
}

static DBusHandlerResult create_port(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	char path[MAX_PATH_LENGTH], port_name[16], uuid[MAX_LEN_UUID_STR];
	const char *bda, *pattern, *ppath = path;
	struct pending_connect *pending, *pc;
	DBusMessage *reply;
	DBusError derr;
	bdaddr_t src, dst;
	long val;
	int dev_id, err;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &bda,
				DBUS_TYPE_STRING, &pattern,
				DBUS_TYPE_INVALID)) {
		error_invalid_arguments(conn, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	pending = find_pending_connect_by_pattern(bda, pattern);
	if (pending)
		return error_in_progress(conn, msg, "Connection in progress");

	dev_id = hci_get_route(NULL);
	if ((dev_id < 0) ||  (hci_devba(dev_id, &src) < 0))
		return error_failed(conn, msg, "Adapter not available");

	pc = g_new0(struct pending_connect, 1);
	bacpy(&pc->src, &src);
	pc->conn = dbus_connection_ref(conn);
	pc->msg = dbus_message_ref(msg);
	pc->bda = g_strdup(bda);
	pc->id = -1;
	pc->pattern = g_strdup(pattern);
	pc->adapter_path = g_malloc0(16);
	snprintf(pc->adapter_path, 16, "/org/bluez/hci%d", dev_id);

	memset(uuid, 0, sizeof(uuid));

	/* Friendly name or uuid128 */
	if (pattern2uuid128(pattern, uuid, sizeof(uuid)) == 0) {
		if (get_handles(pc, uuid, handles_reply) < 0) {
			pending_connect_free(pc);
			return error_not_supported(conn, msg);
		}
		pending_connects = g_slist_append(pending_connects, pc);
		name_listener_add(conn, dbus_message_get_sender(msg),
				(name_cb_t) transaction_owner_exited, NULL);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	/* Record handle or channel */
	err = pattern2long(pattern, &val);
	if (err < 0) {
		pending_connect_free(pc);
		return error_invalid_arguments(conn, msg, "invalid pattern");
	}

	/* Record handle: starts at 0x10000 */
	if (strncasecmp("0x", pattern, 2) == 0) {
		if (val < 0x10000) {
			pending_connect_free(pc);
			return error_invalid_arguments(conn, msg,
					"invalid record handle");
		}

		if (get_record(pc, val, record_reply) < 0) {
			pending_connect_free(pc);
			return error_not_supported(conn, msg);
		}
		pending_connects = g_slist_append(pending_connects, pc);
		name_listener_add(conn, dbus_message_get_sender(msg),
				(name_cb_t) transaction_owner_exited, NULL);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	pending_connect_free(pc);
	/* RFCOMM Channel range: 1 - 30 */
	if (val < 1 || val > 30)
		return error_invalid_arguments(conn, msg,
				"invalid RFCOMM channel");

	str2ba(bda, &dst);
	err = rfcomm_bind(&src, &dst, -1, val);
	if (err < 0)
		return error_failed_errno(conn, msg, -err);

	snprintf(port_name, sizeof(port_name), "/dev/rfcomm%d", err);
	port_store(&src, &dst, err, val, NULL);
	port_register(conn, err, &src, &dst, port_name, path, NULL);
	ports_paths = g_slist_append(ports_paths, g_strdup(path));

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	dbus_message_append_args(reply,
			DBUS_TYPE_STRING, &ppath,
			DBUS_TYPE_INVALID);
	send_message_and_unref(conn, reply);

	dbus_connection_emit_signal(conn, SERIAL_MANAGER_PATH,
			SERIAL_MANAGER_INTERFACE, "PortCreated" ,
			DBUS_TYPE_STRING, &ppath,
			DBUS_TYPE_INVALID);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static void message_append_paths(DBusMessage *msg, const GSList *list)
{
	const GSList *l;
	const char *path;
	DBusMessageIter iter, iter_array;

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_TYPE_STRING_AS_STRING, &iter_array);

	for (l = list; l; l = l->next) {
		path = l->data;
		dbus_message_iter_append_basic(&iter_array,
				DBUS_TYPE_STRING, &path);
	}

	dbus_message_iter_close_container(&iter, &iter_array);
}

static DBusHandlerResult list_ports(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	DBusMessage *reply;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	message_append_paths(reply, ports_paths);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult remove_port(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct rfcomm_dev_info di;
	DBusError derr;
	const char *path;
	GSList *l;
	int16_t id;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &path,
				DBUS_TYPE_INVALID)) {
		error_invalid_arguments(conn, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (sscanf(path, SERIAL_MANAGER_PATH"/rfcomm%hd", &id) != 1)
		return error_does_not_exist(conn, msg, "Invalid RFCOMM node");

	di.id = id;
	if (ioctl(rfcomm_ctl, RFCOMMGETDEVINFO, &di) < 0)
		return error_does_not_exist(conn, msg, "Invalid RFCOMM node");
	port_delete(&di.src, &di.dst, id);

	if (port_unregister(path) < 0)
		return error_does_not_exist(conn, msg, "Invalid RFCOMM node");

	send_message_and_unref(conn,
			dbus_message_new_method_return(msg));

	dbus_connection_emit_signal(conn, SERIAL_MANAGER_PATH,
			SERIAL_MANAGER_INTERFACE, "PortRemoved" ,
			DBUS_TYPE_STRING, &path,
			DBUS_TYPE_INVALID);

	l = g_slist_find_custom(ports_paths, path, (GCompareFunc) strcmp);
	if (l) {
		g_free(l->data);
		ports_paths = g_slist_remove(ports_paths, l->data);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static int rfcomm_listen(bdaddr_t *src, uint8_t *channel, int opts)
{
	struct sockaddr_rc laddr;
	socklen_t alen;
	int err, sk;

	sk = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
	if (sk < 0)
		return -errno;

	if (setsockopt(sk, SOL_RFCOMM, RFCOMM_LM, &opts, sizeof(opts)) < 0)
		goto fail;

	memset(&laddr, 0, sizeof(laddr));
	laddr.rc_family = AF_BLUETOOTH;
	bacpy(&laddr.rc_bdaddr, src);
	laddr.rc_channel = (channel ? *channel : 0);

	alen = sizeof(laddr);
	if (bind(sk, (struct sockaddr *) &laddr, alen) < 0)
		goto fail;

	if (listen(sk, 1) < 0)
		goto fail;

	if (!channel)
		return sk;

	memset(&laddr, 0, sizeof(laddr));
	if (getsockname(sk, (struct sockaddr *)&laddr, &alen) < 0)
		goto fail;

	*channel = laddr.rc_channel;

	return sk;

fail:
	err = errno;
	close(sk);
	errno = err;

	return -err;
}

static void add_lang_attr(sdp_record_t *r)
{
	sdp_lang_attr_t base_lang;
	sdp_list_t *langs = 0;

	/* UTF-8 MIBenum (http://www.iana.org/assignments/character-sets) */
	base_lang.code_ISO639 = (0x65 << 8) | 0x6e;
	base_lang.encoding = 106;
	base_lang.base_offset = SDP_PRIMARY_LANG_BASE;
	langs = sdp_list_append(0, &base_lang);
	sdp_set_lang_attr(r, langs);
	sdp_list_free(langs, 0);
}

static int str2uuid(uuid_t *uuid, const char *string)
{
	uint16_t data1, data2, data3, data5;
	uint32_t data0, data4;

	if (strlen(string) == 36 &&
			string[8] == '-' &&
			string[13] == '-' &&
			string[18] == '-' &&
			string[23] == '-' &&
			sscanf(string, "%08x-%04hx-%04hx-%04hx-%08x%04hx",
				&data0, &data1, &data2, &data3, &data4, &data5) == 6) {
		uint8_t val[16];

		data0 = htonl(data0);
		data1 = htons(data1);
		data2 = htons(data2);
		data3 = htons(data3);
		data4 = htonl(data4);
		data5 = htons(data5);

		memcpy(&val[0], &data0, 4);
		memcpy(&val[4], &data1, 2);
		memcpy(&val[6], &data2, 2);
		memcpy(&val[8], &data3, 2);
		memcpy(&val[10], &data4, 4);
		memcpy(&val[14], &data5, 2);

		sdp_uuid128_create(uuid, val);

		return 0;
	}

	return -1;
}

static int create_proxy_record(sdp_buf_t *buf, const char *uuid128, uint8_t channel)
{
	sdp_list_t *apseq, *aproto, *profiles, *proto[2], *root, *svclass_id;
	uuid_t uuid, root_uuid, l2cap, rfcomm;
	sdp_profile_desc_t profile;
	sdp_record_t record;
	sdp_data_t *ch;
	int ret;

	memset(&record, 0, sizeof(sdp_record_t));
	record.handle = 0xffffffff;
	sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
	root = sdp_list_append(NULL, &root_uuid);
	sdp_set_browse_groups(&record, root);
	sdp_list_free(root, NULL);

	str2uuid(&uuid, uuid128);
	svclass_id = sdp_list_append(NULL, &uuid);
	sdp_set_service_classes(&record, svclass_id);
	sdp_list_free(svclass_id, NULL);

	sdp_uuid16_create(&profile.uuid, SERIAL_PORT_PROFILE_ID);
	profile.version = 0x0100;
	profiles = sdp_list_append(NULL, &profile);
	sdp_set_profile_descs(&record, profiles);
	sdp_list_free(profiles, NULL);

	sdp_uuid16_create(&l2cap, L2CAP_UUID);
	proto[0] = sdp_list_append(NULL, &l2cap);
	apseq = sdp_list_append(NULL, proto[0]);

	sdp_uuid16_create(&rfcomm, RFCOMM_UUID);
	proto[1] = sdp_list_append(NULL, &rfcomm);
	ch = sdp_data_alloc(SDP_UINT8, &channel);
	proto[1] = sdp_list_append(proto[1], ch);
	apseq = sdp_list_append(apseq, proto[1]);

	aproto = sdp_list_append(NULL, apseq);
	sdp_set_access_protos(&record, aproto);

	add_lang_attr(&record);

	sdp_set_info_attr(&record, "Port Proxy Entity",
				NULL, "Port Proxy Entity");

	ret = sdp_gen_record_pdu(&record, buf);

	sdp_data_free(ch);
	sdp_list_free(proto[0], NULL);
	sdp_list_free(proto[1], NULL);
	sdp_list_free(apseq, NULL);
	sdp_list_free(aproto, NULL);
	sdp_list_free(record.attrlist, (sdp_free_func_t) sdp_data_free);
	sdp_list_free(record.pattern, free);

	return ret;
}

static GIOError channel_write(GIOChannel *chan, char *buf, size_t size)
{
	GIOError err = G_IO_ERROR_NONE;
	gsize wbytes, written;

	wbytes = written = 0;
	while (wbytes < size) {
		err = g_io_channel_write(chan,
				buf + wbytes,
				size - wbytes,
				&written);

		if (err != G_IO_ERROR_NONE)
			return err;

		wbytes += written;
	}

	return err;
}

static gboolean forward_data(GIOChannel *chan, GIOCondition cond, gpointer data)
{
	char buf[BUF_SIZE];
	GIOChannel *dest = data;
	GIOError err;
	size_t rbytes;

	if (cond & G_IO_NVAL)
		return FALSE;

	if (cond & (G_IO_HUP | G_IO_ERR)) {
		/* Try forward remaining data */
		do {
			rbytes = 0;
			err = g_io_channel_read(chan, buf, sizeof(buf), &rbytes);
			if (err != G_IO_ERROR_NONE || rbytes == 0)
				break;

			err = channel_write(dest, buf, rbytes);
		} while (err == G_IO_ERROR_NONE);

		g_io_channel_close(dest);
		return FALSE;
	}

	rbytes = 0;
	err = g_io_channel_read(chan, buf, sizeof(buf), &rbytes);
	if (err != G_IO_ERROR_NONE)
		return FALSE;

	err = channel_write(dest, buf, rbytes);
	if (err != G_IO_ERROR_NONE)
		return FALSE;

	return TRUE;
}

static uint32_t add_proxy_record(DBusConnection *conn, sdp_buf_t *buf)
{
	DBusMessage *msg, *reply;
	DBusError derr;
	dbus_uint32_t rec_id;

	msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
			"org.bluez.Database", "AddServiceRecord");
	if (!msg) {
		error("Can't allocate new method call");
		return 0;
	}

	dbus_message_append_args(msg,
			DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
			&buf->data, buf->data_size,
			DBUS_TYPE_INVALID);

	dbus_error_init(&derr);
	reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &derr);

	free(buf->data);
	dbus_message_unref(msg);

	if (dbus_error_is_set(&derr) ||
			dbus_set_error_from_message(&derr, reply)) {
		error("Adding service record failed: %s", derr.message);
		dbus_error_free(&derr);
		return 0;
	}

	dbus_message_get_args(reply, &derr,
			DBUS_TYPE_UINT32, &rec_id,
			DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&derr)) {
		error("Invalid arguments to AddServiceRecord reply: %s",
				derr.message);
		dbus_message_unref(reply);
		dbus_error_free(&derr);
		return 0;
	}

	dbus_message_unref(reply);

	return rec_id;
}

static int remove_proxy_record(DBusConnection *conn, uint32_t rec_id)
{
	DBusMessage *msg, *reply;
	DBusError derr;

	msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
				"org.bluez.Database", "RemoveServiceRecord");
	if (!msg) {
		error("Can't allocate new method call");
		return -ENOMEM;
	}

	dbus_message_append_args(msg,
			DBUS_TYPE_UINT32, &rec_id,
			DBUS_TYPE_INVALID);

	dbus_error_init(&derr);
	reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &derr);

	dbus_message_unref(msg);

	if (dbus_error_is_set(&derr)) {
		error("Removing service record 0x%x failed: %s",
						rec_id, derr.message);
		dbus_error_free(&derr);
		return -1;
	}

	dbus_message_unref(reply);

	return 0;
}

static inline int unix_socket_connect(const char *address)
{
	struct sockaddr_un addr;
	int err, sk;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = PF_UNIX;

	if (strncmp("x00", address, 3) == 0) {
		/*
		 * Abstract namespace: first byte NULL, x00
		 * must be removed from the original address.
		 */
		strcpy(addr.sun_path + 1, address + 3);
	} else {
		/* Filesystem address */
		strcpy(addr.sun_path, address);
	}

	/* Unix socket */
	sk = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sk < 0) {
		err = errno;
		error("Unix socket(%s) create failed: %s(%d)",
				address, strerror(err), err);
		return -err;
	}

	if (connect(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		err = errno;
		error("Unix socket(%s) connect failed: %s(%d)",
				address, strerror(err), err);
		close(sk);
		errno = err;
		return -err;
	}

	return sk;
}

static int tcp_socket_connect(const char *address)
{
	struct sockaddr_in addr;
	int err, sk;
	unsigned short int port;

	memset(&addr, 0, sizeof(addr));

	if (strncmp(address, "localhost", 9) != 0) {
		error("Address should have the form localhost:port.");
		return -1;
	}
	port = atoi(strchr(address, ':') + 1);
	if (port <= 0) {
		error("Invalid port '%d'.", port);
		return -1;
	}
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	addr.sin_port = htons(port);

	sk = socket(PF_INET, SOCK_STREAM, 0);
	if (sk < 0) {
		err = errno;
		error("TCP socket(%s) create failed %s(%d)", address,
							strerror(err), err);
		return -err;
	}
	if (connect(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		err = errno;
		error("TCP socket(%s) connect failed: %s(%d)",
						address, strerror(err), err);
		close(sk);
		errno = err;
		return -err;
	}
	return sk;
}

static inline int tty_open(const char *tty, struct termios *ti)
{
	int err, sk;

	sk = open(tty, O_RDWR | O_NOCTTY);
	if (sk < 0) {
		err = errno;
		error("Can't open TTY %s: %s(%d)", tty, strerror(err), err);
		return -err;
	}

	if (ti && tcsetattr(sk, TCSANOW, ti) < 0) {
		err = errno;
		error("Can't change serial settings: %s(%d)",
				strerror(err), err);
		close(sk);
		errno = err;
		return -err;
	}

	return sk;
}

static gboolean connect_event(GIOChannel *chan,
			GIOCondition cond, gpointer data)
{
	struct proxy *prx = data;
	struct sockaddr_rc raddr;
	GIOChannel *rio, *lio;
	socklen_t alen;
	int sk, rsk, lsk;

	if (cond & G_IO_NVAL)
		return FALSE;

	if (cond & (G_IO_ERR | G_IO_HUP)) {
		g_io_channel_close(chan);
		return FALSE;
	}

	sk = g_io_channel_unix_get_fd(chan);

	memset(&raddr, 0, sizeof(raddr));
	alen = sizeof(raddr);
	rsk = accept(sk, (struct sockaddr *) &raddr, &alen);
	if (rsk < 0)
		return TRUE;

	bacpy(&prx->dst, &raddr.rc_bdaddr);

	switch (prx->type) {
	case UNIX_SOCKET_PROXY:
		lsk = unix_socket_connect(prx->address);
		break;
	case TTY_PROXY:
		lsk = tty_open(prx->address, &prx->proxy_ti);
		break;
	case TCP_SOCKET_PROXY:
		lsk = tcp_socket_connect(prx->address);
		break;
	default:
		lsk = -1;
	}

	if (lsk < 0) {
		close(rsk);
		return TRUE;
	}

	rio = g_io_channel_unix_new(rsk);
	g_io_channel_set_close_on_unref(rio, TRUE);
	lio = g_io_channel_unix_new(lsk);
	g_io_channel_set_close_on_unref(lio, TRUE);

	prx->rfcomm_watch = g_io_add_watch(rio,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				forward_data, lio);

	prx->local_watch = g_io_add_watch(lio,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				forward_data, rio);

	g_io_channel_unref(rio);
	g_io_channel_unref(lio);

	return TRUE;
}

static void listen_watch_notify(gpointer data)
{
	struct proxy *prx = data;

	prx->listen_watch = 0;

	if (prx->rfcomm_watch) {
		g_source_remove(prx->rfcomm_watch);
		prx->rfcomm_watch = 0;
	}

	if (prx->local_watch) {
		g_source_remove(prx->local_watch);
		prx->local_watch = 0;
	}

	remove_proxy_record(connection, prx->record_id);
	prx->record_id = 0;
}

static DBusHandlerResult proxy_enable(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct proxy *prx = data;
	GIOChannel *io;
	sdp_buf_t buf;
	int sk;

	if (prx->listen_watch)
		return error_failed(conn, msg, "Already enabled");

	/* Listen */
	/* FIXME: missing options */
	sk = rfcomm_listen(&prx->src, &prx->channel, 0);
	if (sk < 0) {
		const char *strerr = strerror(errno);
		error("RFCOMM listen socket failed: %s(%d)", strerr, errno);
		return error_failed(conn, msg, strerr);
	}

	/* Create the record */
	create_proxy_record(&buf, prx->uuid128, prx->channel);

	/* Register the record */
	prx->record_id = add_proxy_record(conn, &buf);
	if (!prx->record_id) {
		close(sk);
		return error_failed(conn, msg, "Service registration failed");
	}

	/* Add incomming connection watch */
	io = g_io_channel_unix_new(sk);
	g_io_channel_set_close_on_unref(io, TRUE);
	prx->listen_watch = g_io_add_watch_full(io, G_PRIORITY_DEFAULT,
			G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			connect_event, prx, listen_watch_notify);
	g_io_channel_unref(io);

	return send_message_and_unref(conn,
			dbus_message_new_method_return(msg));
}

static DBusHandlerResult proxy_disable(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct proxy *prx = data;

	if (!prx->listen_watch)
		return error_failed(conn, msg, "Not enabled");

	/* Remove the watches and unregister the record: see watch notify */
	g_source_remove(prx->listen_watch);

	return send_message_and_unref(conn,
			dbus_message_new_method_return(msg));
}

static DBusHandlerResult proxy_get_info(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct proxy *prx = data;
	DBusMessage *reply;
	DBusMessageIter iter, dict;
	dbus_bool_t boolean;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	dbus_message_iter_append_dict_entry(&dict, "uuid",
			DBUS_TYPE_STRING, &prx->uuid128);

	dbus_message_iter_append_dict_entry(&dict, "address",
			DBUS_TYPE_STRING, &prx->address);

	if (prx->channel)
		dbus_message_iter_append_dict_entry(&dict, "channel",
				DBUS_TYPE_BYTE, &prx->channel);

	boolean = (prx->listen_watch ? TRUE : FALSE);
	dbus_message_iter_append_dict_entry(&dict, "enabled",
			DBUS_TYPE_BOOLEAN, &boolean);

	boolean = (prx->rfcomm_watch ? TRUE : FALSE);
	dbus_message_iter_append_dict_entry(&dict, "connected",
			DBUS_TYPE_BOOLEAN, &boolean);

	/* If connected: append the remote address */
	if (boolean) {
		char bda[18];
		const char *pstr = bda;

		ba2str(&prx->dst, bda);
		dbus_message_iter_append_dict_entry(&dict, "address",
				DBUS_TYPE_STRING, &pstr);
	}

	dbus_message_iter_close_container(&iter, &dict);

	return send_message_and_unref(conn, reply);
}

static struct {
	const char	*str;
	speed_t		speed;
} supported_speed[]  = {
	{"50",		B50	},
	{"300",		B300	},
	{"600",		B600	},
	{"1200",	B1200	},
	{"1800",	B1800	},
	{"2400",	B2400	},
	{"4800",	B4800	},
	{"9600",	B9600	},
	{"19200",	B19200	},
	{"38400",	B38400	},
	{"57600",	B57600	},
	{"115200",	B115200	},
	{ NULL,		B0	}
};

static speed_t str2speed(const char *str, speed_t *speed)
{
	int i;

	for (i = 0; supported_speed[i].str; i++) {
		if (strcmp(supported_speed[i].str, str) != 0)
			continue;

		if (speed)
			*speed = supported_speed[i].speed;

		return supported_speed[i].speed;
	}

	return B0;
}

static int set_parity(const char *str, tcflag_t *ctrl)
{
	if (strcasecmp("even", str) == 0) {
		*ctrl |= PARENB;
		*ctrl &= ~PARODD;
	} else if (strcasecmp("odd", str) == 0) {
		*ctrl |= PARENB;
		*ctrl |= PARODD;
	} else if (strcasecmp("mark", str) == 0)
		*ctrl |= PARENB;
	else if ((strcasecmp("none", str) == 0) ||
			(strcasecmp("space", str) == 0))
		*ctrl &= ~PARENB;
	else
		return -1;

	return 0;
}

static int set_databits(uint8_t databits, tcflag_t *ctrl)
{
	if (databits < 5 || databits > 8)
		return -EINVAL;

	*ctrl &= ~CSIZE;
	switch (databits) {
	case 5:
		*ctrl |= CS5;
		break;
	case 6:
		*ctrl |= CS6;
		break;
	case 7:
		*ctrl |= CS7;
		break;
	case 8:
		*ctrl |= CS8;
		break;
	}

	return 0;
}

static int set_stopbits(uint8_t stopbits, tcflag_t *ctrl)
{
	/* 1.5 will not be allowed */
	switch (stopbits) {
	case 1:
		*ctrl &= ~CSTOPB;
		return 0;
	case 2:
		*ctrl |= CSTOPB;
		return 0;
	}

	return -EINVAL;
}

static DBusHandlerResult proxy_set_serial_params(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusError derr;
	struct proxy *prx = data;
	const char *ratestr, *paritystr;
	uint8_t databits, stopbits;
	tcflag_t ctrl;		/* Control mode flags */
	speed_t speed = B0;	/* In/Out speed */

	/* Don't allow change TTY settings if it is open */
	if (prx->local_watch)
		return error_failed(conn, msg, "Not allowed");

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &ratestr,
				DBUS_TYPE_BYTE, &databits,
				DBUS_TYPE_BYTE, &stopbits,
				DBUS_TYPE_STRING, &paritystr,
				DBUS_TYPE_INVALID)) {
		error_invalid_arguments(conn, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (str2speed(ratestr, &speed)  == B0)
		return error_invalid_arguments(conn, msg, "Invalid baud rate");

	ctrl = prx->proxy_ti.c_cflag;
	if (set_databits(databits, &ctrl) < 0)
		return error_invalid_arguments(conn, msg, "Invalid data bits");

	if (set_stopbits(stopbits, &ctrl) < 0)
		return error_invalid_arguments(conn, msg, "Invalid stop bits");

	if (set_parity(paritystr, &ctrl) < 0)
		return error_invalid_arguments(conn, msg, "Invalid parity");

	prx->proxy_ti.c_cflag = ctrl;
	prx->proxy_ti.c_cflag |= (CLOCAL | CREAD);
	cfsetispeed(&prx->proxy_ti, speed);
	cfsetospeed(&prx->proxy_ti, speed);

	return send_message_and_unref(conn,
			dbus_message_new_method_return(msg));
}

static DBusMethodVTable proxy_methods[] = {
	{ "Enable",			proxy_enable,			"",	""	},
	{ "Disable",			proxy_disable,			"",	""	},
	{ "GetInfo",			proxy_get_info,			"",	"a{sv}"	},
	{ "SetSerialParameters",	proxy_set_serial_params,	"syys",	""	},
	{ NULL, NULL, NULL, NULL },
};

static void proxy_handler_unregister(DBusConnection *conn, void *data)
{
	struct proxy *prx = data;
	int sk;

	info("Unregistered proxy: %s", prx->address);

	if (prx->type != TTY_PROXY)
		goto done;

	/* Restore the initial TTY configuration */
	sk =  open(prx->address, O_RDWR | O_NOCTTY);
	if (sk) {
		tcsetattr(sk, TCSAFLUSH, &prx->sys_ti);
		close(sk);
	}

done:
	if (prx->listen_watch)
		g_source_remove(prx->listen_watch);

	proxy_free(prx);
}

static int register_proxy_object(struct proxy *prx, char *outpath, size_t size)
{
	char path[MAX_PATH_LENGTH + 1];

	snprintf(path, MAX_PATH_LENGTH, "/org/bluez/serial/proxy%d",
			sk_counter++);

	if (!dbus_connection_create_object_path(connection, path, prx,
				proxy_handler_unregister)) {
		error("D-Bus failed to register %s path", path);
		return -1;
	}

	dbus_connection_register_interface(connection, path,
			SERIAL_PROXY_INTERFACE, proxy_methods, NULL, NULL);
	proxies_paths = g_slist_append(proxies_paths, g_strdup(path));

	if (outpath)
		strncpy(outpath, path, size);

	info("Registered proxy:%s", path);

	return 0;
}

static int proxy_tty_register(bdaddr_t *src, const char *uuid128,
		const char *address, struct termios *ti, char *outpath, size_t size)
{
	struct termios sys_ti;
	struct proxy *prx;
	int sk, ret;

	sk = open(address, O_RDONLY | O_NOCTTY);
	if (sk < 0) {
		error("Cant open TTY: %s(%d)", strerror(errno), errno);
		return -EINVAL;
	}

	prx = g_new0(struct proxy, 1);
	prx->address = g_strdup(address);
	prx->uuid128 = g_strdup(uuid128);
	prx->type = TTY_PROXY;
	bacpy(&prx->src, src);

	/* Current TTY settings */
	memset(&sys_ti, 0, sizeof(sys_ti));
	tcgetattr(sk, &sys_ti);
	memcpy(&prx->sys_ti, &sys_ti, sizeof(sys_ti));
	close(sk);

	if (!ti) {
		/* Use current settings */
		memcpy(&prx->proxy_ti, &sys_ti, sizeof(sys_ti));
	} else {
		/* New TTY settings: user provided */
		memcpy(&prx->proxy_ti, ti, sizeof(*ti));
	}

	ret = register_proxy_object(prx, outpath, size);
	if (ret < 0)
		proxy_free(prx);

	return ret;
}

static int proxy_socket_register(bdaddr_t *src, const char *uuid128,
		const char *address, char *outpath, size_t size)
{
	struct proxy *prx;
	int ret;

	prx = g_new0(struct proxy, 1);
	prx->address = g_strdup(address);
	prx->uuid128 = g_strdup(uuid128);
	prx->type = UNIX_SOCKET_PROXY;
	bacpy(&prx->src, src);

	ret = register_proxy_object(prx, outpath, size);
	if (ret < 0)
		proxy_free(prx);

	return ret;
}

static int proxy_tcp_register(bdaddr_t *src, const char *uuid128,
			const char *address, char *outpath, size_t size)
{
	struct proxy *prx;
	int ret;

	prx = g_new0(struct proxy, 1);
	prx->address = g_strdup(address);
	prx->uuid128 = g_strdup(uuid128);
	prx->type = TCP_SOCKET_PROXY;
	bacpy(&prx->src, src);

	ret = register_proxy_object(prx, outpath, size);
	if (ret < 0)
		proxy_free(prx);

	return ret;
}

static proxy_type_t addr2type(const char *address)
{
	struct stat st;

	if (stat(address, &st) < 0) {
		/*
		 * Unix socket: if the sun_path starts with null byte
		 * it refers to abstract namespace. 'x00' will be used
		 * to represent the null byte.
		 */
		if (strncmp("localhost:", address, 10) == 0)
			return TCP_SOCKET_PROXY;
		if (strncmp("x00", address, 3) != 0)
			return UNKNOWN_PROXY_TYPE;
		else
			return UNIX_SOCKET_PROXY;
	} else {
		/* Filesystem: char device or unix socket */
		if (S_ISCHR(st.st_mode) && strncmp("/dev/", address, 4) == 0)
			return TTY_PROXY;
		else if (S_ISSOCK(st.st_mode))
			return UNIX_SOCKET_PROXY;
		else
			return UNKNOWN_PROXY_TYPE;
	}
}

static int proxycmp(const char *path, const char *address)
{
	struct proxy *prx = NULL;

	if (!dbus_connection_get_object_user_data(connection,
				path, (void *) &prx) || !prx)
		return -1;

	return strcmp(prx->address, address);
}

static DBusHandlerResult create_proxy(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	char path[MAX_PATH_LENGTH + 1];
	const char *uuid128, *address, *ppath = path;
	DBusMessage *reply;
	proxy_type_t type;
	DBusError derr;
	bdaddr_t src;
	uuid_t uuid;
	int dev_id, ret;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &uuid128,
				DBUS_TYPE_STRING, &address,
				DBUS_TYPE_INVALID)) {
		error_invalid_arguments(conn, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (str2uuid(&uuid, uuid128) < 0)
		return error_invalid_arguments(conn, msg, "Invalid UUID");

	type = addr2type(address);
	if (type == UNKNOWN_PROXY_TYPE)
		return error_invalid_arguments(conn, msg, "Invalid address");

	/* Only one proxy per address(TTY or unix socket) is allowed */
	if (g_slist_find_custom(proxies_paths,
				address, (GCompareFunc) proxycmp))
		return error_already_exists(conn, msg, "Proxy already exists");

	dev_id = hci_get_route(NULL);
	if ((dev_id < 0) || (hci_devba(dev_id, &src) < 0)) {
		error("Adapter not available");
		return error_failed(conn, msg, "Adapter not available");
	}

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	switch (type) {
	case UNIX_SOCKET_PROXY:
		ret = proxy_socket_register(&src, uuid128,
						address, path, sizeof(path));
		break;
	case TTY_PROXY:
		ret = proxy_tty_register(&src, uuid128,
				address, NULL, path, sizeof(path));
		break;
	case TCP_SOCKET_PROXY:
		ret = proxy_tcp_register(&src, uuid128, address,
						path, sizeof(path));
		break;
	default:
		ret = -1;
	}
	if (ret < 0) {
		dbus_message_unref(reply);
		return error_failed(conn, msg, "Create object path failed");
	}

	dbus_connection_emit_signal(connection, SERIAL_MANAGER_PATH,
			SERIAL_MANAGER_INTERFACE, "ProxyCreated",
			DBUS_TYPE_STRING, &ppath,
			DBUS_TYPE_INVALID);

	dbus_message_append_args(reply,
			DBUS_TYPE_STRING, &ppath,
			DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult list_proxies(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	DBusMessage *reply;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	message_append_paths(reply, proxies_paths);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult remove_proxy(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct proxy *prx = NULL;
	const char *path;
	GSList *l;
	DBusError derr;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &path,
				DBUS_TYPE_INVALID)) {
		error_invalid_arguments(conn, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	l = g_slist_find_custom(proxies_paths, path, (GCompareFunc) strcmp);
	if (!l)
		return error_does_not_exist(conn, msg, "Invalid proxy path");

	/* Remove from storage */
	if (dbus_connection_get_object_user_data(conn,
				path, (void *) &prx) && prx)
		proxy_delete(&prx->src, prx->address);

	g_free(l->data);
	proxies_paths = g_slist_remove(proxies_paths, l->data);

	dbus_connection_destroy_object_path(conn, path);

	dbus_connection_emit_signal(conn, SERIAL_MANAGER_PATH,
			SERIAL_MANAGER_INTERFACE, "ProxyRemoved",
			DBUS_TYPE_STRING, &path,
			DBUS_TYPE_INVALID);

	return send_message_and_unref(conn,
			dbus_message_new_method_return(msg));
}

static DBusHandlerResult connect_service_from_devid(DBusConnection *conn,
				DBusMessage *msg, void *data, int dev_id,
				const char *bda, const char *pattern)
{
	struct pending_connect *pending, *pc;
	bdaddr_t src;
	long val;
	int err;
	char uuid[MAX_LEN_UUID_STR];

	pending = find_pending_connect_by_pattern(bda, pattern);
	if (pending)
		return error_in_progress(conn, msg, "Connection in progress");

	if ((dev_id < 0) || (hci_devba(dev_id, &src) < 0))
		return error_failed(conn, msg, "Adapter not available");

	pc = g_new0(struct pending_connect, 1);
	bacpy(&pc->src, &src);
	pc->conn = dbus_connection_ref(conn);
	pc->msg = dbus_message_ref(msg);
	pc->bda = g_strdup(bda);
	pc->id = -1;
	pc->pattern = g_strdup(pattern);
	pc->adapter_path = g_malloc0(16);
	snprintf(pc->adapter_path, 16, "/org/bluez/hci%d", dev_id);

	memset(uuid, 0, sizeof(uuid));

	/* Friendly name or uuid128 */
	if (pattern2uuid128(pattern, uuid, sizeof(uuid)) == 0) {
		if (get_handles(pc, uuid, handles_reply) < 0) {
			pending_connect_free(pc);
			return error_not_supported(conn, msg);
		}
		pending_connects = g_slist_append(pending_connects, pc);
		goto done;
	}

	/* Record handle or channel */
	err = pattern2long(pattern, &val);
	if (err < 0) {
		pending_connect_free(pc);
		return error_invalid_arguments(conn, msg, "invalid pattern");
	}

	/* Record handle: starts at 0x10000 */
	if (strncasecmp("0x", pattern, 2) == 0) {
		if (val < 0x10000) {
			pending_connect_free(pc);
			return error_invalid_arguments(conn, msg,
					"invalid record handle");
		}

		if (get_record(pc, val, record_reply) < 0) {
			pending_connect_free(pc);
			return error_not_supported(conn, msg);
		}
		pending_connects = g_slist_append(pending_connects, pc);
		goto done;
	}

	/* RFCOMM Channel range: 1 - 30 */
	if (val < 1 || val > 30) {
		pending_connect_free(pc);
		return error_invalid_arguments(conn, msg,
				"invalid RFCOMM channel");
	}

	/* Add here since connect() in the first try can happen */
	pending_connects = g_slist_append(pending_connects, pc);

	pc->channel = val;
	err = rfcomm_connect(pc);
	if (err < 0) {
		const char *strerr = strerror(-err);
		error("RFCOMM connect failed: %s(%d)", strerr, -err);
		pending_connects = g_slist_remove(pending_connects, pc);
		pending_connect_free(pc);
		return error_connection_attempt_failed(conn, msg, -err);
	}
done:
	name_listener_add(conn, dbus_message_get_sender(msg),
			(name_cb_t) transaction_owner_exited, NULL);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult connect_service(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusError derr;
	const char *bda, *pattern;
	int devid;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &bda,
				DBUS_TYPE_STRING, &pattern,
				DBUS_TYPE_INVALID)) {
		error_invalid_arguments(conn, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	devid = hci_get_route(NULL);

	return connect_service_from_devid(conn, msg, data, devid, bda, pattern);
}

static DBusHandlerResult connect_service_from_adapter(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusError derr;
	const char *adapter, *bda, *pattern;
	int devid;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &adapter,
				DBUS_TYPE_STRING, &bda,
				DBUS_TYPE_STRING, &pattern,
				DBUS_TYPE_INVALID)) {
		error_invalid_arguments(conn, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	devid = hci_devid(adapter);

	return connect_service_from_devid(conn, msg, data, devid, bda, pattern);
}

static DBusHandlerResult disconnect_service(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusError derr;
	const char *name;
	int err, id;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &name,
				DBUS_TYPE_INVALID)) {
		error_invalid_arguments(conn, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (sscanf(name, "/dev/rfcomm%d", &id) != 1)
		return error_invalid_arguments(conn, msg, "invalid RFCOMM node");

	err = port_remove_listener(dbus_message_get_sender(msg), name);
	if (err < 0)
		return error_does_not_exist(conn, msg, "Invalid RFCOMM node");

	send_message_and_unref(conn,
			dbus_message_new_method_return(msg));

	dbus_connection_emit_signal(conn, SERIAL_MANAGER_PATH,
			SERIAL_MANAGER_INTERFACE, "ServiceDisconnected" ,
			DBUS_TYPE_STRING, &name,
			DBUS_TYPE_INVALID);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult cancel_connect_service(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct pending_connect *pending;
	DBusMessage *reply;
	DBusError derr;
	const char *bda, *pattern;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &bda,
				DBUS_TYPE_STRING, &pattern,
				DBUS_TYPE_INVALID)) {
		error_invalid_arguments(conn, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	pending = find_pending_connect_by_pattern(bda, pattern);
	if (!pending)
		return error_does_not_exist(conn, msg,
				"No such connection request");

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	pending->canceled = 1;

	return send_message_and_unref(conn, reply);
}

static void proxy_path_free(gpointer data, gpointer udata)
{
	DBusConnection *conn = udata;
	const char *path = data;
	struct proxy *prx = NULL;

	/* Store/Update the proxy entries before exit */
	if (dbus_connection_get_object_user_data(conn,
				path, (void *) &prx) && prx) {
		struct termios *ti;

		ti = (prx->type == TTY_PROXY ? &prx->proxy_ti : NULL);
		proxy_store(&prx->src, prx->uuid128, prx->address, NULL,
				prx->channel, 0, ti);
	}

	g_free(data);
}

static void manager_unregister(DBusConnection *conn, void *data)
{
	char **dev;
	int i;

	if (pending_connects) {
		g_slist_foreach(pending_connects,
				(GFunc) pending_connect_free, NULL);
		g_slist_free(pending_connects);
		pending_connects = NULL;
	}

	if (proxies_paths) {
		g_slist_foreach(proxies_paths,
				proxy_path_free, conn);
		g_slist_free(proxies_paths);
		proxies_paths = NULL;
	}

	if (ports_paths) {
		g_slist_foreach(ports_paths,
				(GFunc) g_free, NULL);
		g_slist_free(ports_paths);
		ports_paths = NULL;
	}

	/* Unregister all paths in serial hierarchy */
	if (!dbus_connection_list_registered(conn, SERIAL_MANAGER_PATH, &dev))
		return;

	for (i = 0; dev[i]; i++) {
		char dev_path[MAX_PATH_LENGTH];

		snprintf(dev_path, sizeof(dev_path), "%s/%s", SERIAL_MANAGER_PATH,
				dev[i]);

		dbus_connection_destroy_object_path(conn, dev_path);
	}

	dbus_free_string_array(dev);
}

static DBusMethodVTable manager_methods[] = {
	{ "CreatePort",			create_port,			"ss",	"s"	},
	{ "ListPorts",			list_ports,			"",	"as"	},
	{ "RemovePort",			remove_port,			"s",	""	},
	{ "CreateProxy",		create_proxy,			"ss",	"s"	},
	{ "ListProxies",		list_proxies,			"",	"as"	},
	{ "RemoveProxy",		remove_proxy,			"s",	""	},
	{ "ConnectService",		connect_service,		"ss",	"s"	},
	{ "ConnectServiceFromAdapter",	connect_service_from_adapter,	"sss",	"s"	},
	{ "DisconnectService",		disconnect_service,		"s",	""	},
	{ "CancelConnectService",	cancel_connect_service,		"ss",	""	},
	{ NULL, NULL, NULL, NULL },
};

static DBusSignalVTable manager_signals[] = {
	{ "PortCreated",		"s"	},
	{ "PortRemoved",		"s"	},
	{ "ProxyCreated",		"s"	},
	{ "ProxyRemoved",		"s"	},
	{ "ServiceConnected",		"s"	},
	{ "ServiceDisconnected",	"s"	},
	{ NULL, NULL }
};

static void parse_port(char *key, char *value, void *data)
{
	char path[MAX_PATH_LENGTH], port_name[16], dst_addr[18], *svc;
	char *src_addr = data;
	bdaddr_t dst, src;
	int ch, id;

	memset(dst_addr, 0, sizeof(dst_addr));
	if (sscanf(key,"%17s#%d", dst_addr, &id) != 2)
		return;

	if (sscanf(value,"%d:", &ch) != 1)
		return;

	svc = strchr(value, ':');
	if (svc && *svc)
		svc++;

	str2ba(dst_addr, &dst);
	str2ba(src_addr, &src);

	if (rfcomm_bind(&src, &dst, id, ch) < 0)
		return;

	snprintf(port_name, sizeof(port_name), "/dev/rfcomm%d", id);

	if (port_register(connection, id, &src, &dst,
				port_name, path, svc) < 0) {
		rfcomm_release(id);
		return;
	}

	ports_paths = g_slist_append(ports_paths, g_strdup(path));
}

static void parse_proxy(char *key, char *value, void *data)
{
	char uuid128[MAX_LEN_UUID_STR], tmp[3];
	char *pvalue, *src_addr = data;
	proxy_type_t type;
	int ch, opts, pos;
	bdaddr_t src;
	struct termios ti;
	uint8_t *pti;

	memset(uuid128, 0, sizeof(uuid128));
	ch = opts = pos = 0;
	if (sscanf(value,"%s %d 0x%04X %n", uuid128, &ch, &opts, &pos) != 3)
		return;

	/* Extracting name */
	value += pos;
	pvalue = strchr(value, ':');
	if (!pvalue)
		return;

	/* FIXME: currently name is not used */
	*pvalue = '\0';

	str2ba(src_addr, &src);
	type = addr2type(key);
	switch (type) {
	case TTY_PROXY:
		/* Extracting termios */
		pvalue++;
		if (!pvalue || strlen(pvalue) != (2 * sizeof(ti)))
			return;

		memset(&ti, 0, sizeof(ti));
		memset(tmp, 0, sizeof(tmp));

		/* Converting to termios struct */
		pti = (uint8_t *) &ti;
		for (pos = 0; pos < sizeof(ti); pos++, pvalue += 2, pti++) {
			memcpy(tmp, pvalue, 2);
			*pti = (uint8_t) strtol(tmp, NULL, 16);
		}

		proxy_tty_register(&src, uuid128, key, &ti, NULL, 0);
		break;
	case UNIX_SOCKET_PROXY:
		proxy_socket_register(&src, uuid128, key, NULL, 0);
		break;
	case TCP_SOCKET_PROXY:
		proxy_tcp_register(&src, uuid128, key, NULL, 0);
		break;
	default:
		return;
	}
}

static void register_stored(void)
{
	char filename[PATH_MAX + 1];
	struct dirent *de;
	DIR *dir;

	snprintf(filename, PATH_MAX, "%s", STORAGEDIR);

	dir = opendir(filename);
	if (!dir)
		return;

	while ((de = readdir(dir)) != NULL) {
		if (!isdigit(de->d_name[0]))
			continue;

		snprintf(filename, PATH_MAX, "%s/%s/serial", STORAGEDIR, de->d_name);
		textfile_foreach(filename, parse_port, de->d_name);

		snprintf(filename, PATH_MAX, "%s/%s/proxy", STORAGEDIR, de->d_name);
		textfile_foreach(filename, parse_proxy, de->d_name);
	}

	closedir(dir);
}

int serial_manager_init(DBusConnection *conn)
{

	if (rfcomm_ctl < 0) {
		rfcomm_ctl = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_RFCOMM);
		if (rfcomm_ctl < 0)
			return -errno;
	}

	if (!dbus_connection_create_object_path(conn, SERIAL_MANAGER_PATH,
						NULL, manager_unregister)) {
		error("D-Bus failed to register %s path", SERIAL_MANAGER_PATH);
		return -1;
	}

	if (!dbus_connection_register_interface(conn, SERIAL_MANAGER_PATH,
						SERIAL_MANAGER_INTERFACE,
						manager_methods,
						manager_signals, NULL)) {
		error("Failed to register %s interface to %s",
				SERIAL_MANAGER_INTERFACE, SERIAL_MANAGER_PATH);
		dbus_connection_destroy_object_path(connection,
							SERIAL_MANAGER_PATH);
		return -1;
	}

	connection = dbus_connection_ref(conn);

	info("Registered manager path:%s", SERIAL_MANAGER_PATH);

	register_stored();

	return 0;
}

void serial_manager_exit(void)
{
	dbus_connection_destroy_object_path(connection, SERIAL_MANAGER_PATH);

	dbus_connection_unref(connection);
	connection = NULL;

	if (rfcomm_ctl >= 0)
		close(rfcomm_ctl);
}
