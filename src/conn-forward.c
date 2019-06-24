/*
    Copyright (C) 2014-2019 Flexible Software Solutions S.L.U.

    This file is part of flexVDI Client.

    flexVDI Client is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    flexVDI Client is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with flexVDI Client. If not, see <https://www.gnu.org/licenses/>.
*/

#include <glib.h>
#include <gio/gio.h>
#include <string.h>
#include <flexdp.h>
#include "conn-forward.h"

/*
static void conn_forwarder_send_command(
    uint32_t command, const uint8_t * data, uint32_t data_size, gpointer user_data) {
    agent_msg_queue((SpiceMainChannel *)channel, command, data_size, data);
    if (&SPICE_CHANNEL(channel)->priv->coroutine != g_coroutine_self())
        spice_channel_wakeup(SPICE_CHANNEL(channel), FALSE);
    else
        agent_send_msg_queue((SpiceMainChannel *)channel);
}
*/


static gboolean tokenize_redirection(gchar * redir, gchar ** bind_address, gchar ** port,
                                     gchar ** host, gchar ** host_port) {
    if ((* bind_address = strtok(redir, ":")) &&
            (* port = strtok(NULL, ":")) &&
            (* host = strtok(NULL, ":"))) {
        if (!(* host_port = strtok(NULL, ":"))) {
            // bind_address was not provided
            * host_port = * host;
            * host = * port;
            * port = * bind_address;
            * bind_address = NULL;
        }
        return TRUE;
    } else
        return FALSE;
}

struct ConnForwarder {
    conn_forwarder_send_command_cb send_command;
    gpointer user_data;
    GHashTable * remote_assocs;
    GHashTable * connections;
    GSocketListener * listener;
    GCancellable * listener_cancellable;
};

static void send_command(ConnForwarder * cf, guint32 command,
                         const guint8 * data, guint32 data_size) {
    cf->send_command(command, data, data_size, cf->user_data);
}

#define WINDOW_SIZE 10*1024*1024
#define MAX_MSG_SIZE FLEXVDI_MAX_MESSAGE_LENGTH - sizeof(FlexVDIMessageHeader)

typedef struct Connection {
    GSocketClient * socket;
    GSocketConnection * conn;
    GCancellable * cancellable;
    GQueue * write_buffer;
    guint8 * read_buffer;
    guint32 data_sent, data_received, ack_interval;
    gboolean connecting;
    ConnForwarder * cf;
    int refs;
    guint32 id;
} Connection;

static Connection * new_connection(ConnForwarder * cf, int id, guint32 ack_int) {
    Connection * conn = (Connection *)g_malloc0(sizeof(Connection));
    if (conn) {
        conn->cancellable = g_cancellable_new();
        conn->refs = 1;
        conn->id = id;
        conn->cf = cf;
        conn->ack_interval = ack_int;
        conn->connecting = TRUE;
        conn->write_buffer = g_queue_new();
        conn->read_buffer = (guint8 *)g_malloc(MAX_MSG_SIZE);
    }
    return conn;
}

static void unref_connection(gpointer value) {
    Connection * conn = (Connection *) value;
    if (!--conn->refs) {
        g_debug("Closing connection %u", conn->id);
        g_object_unref(conn->cancellable);
        if (conn->conn) {
            g_io_stream_close((GIOStream *)conn->conn, NULL, NULL);
        }
        if (conn->socket) {
            g_object_unref(conn->socket);
        }
        g_queue_free_full(conn->write_buffer, (GDestroyNotify)g_bytes_unref);
        g_free(conn->read_buffer);
        g_free(conn);
    }
}

static Connection * new_connection_with_socket(ConnForwarder * cf, int id, guint32 ack_int) {
    Connection * conn = new_connection(cf, id, ack_int);
    if (conn) {
        conn->socket = g_socket_client_new();
        if (!conn->socket) {
            unref_connection(conn);
            return NULL;
        }
    }
    return conn;
}

static Connection * new_open_connection(ConnForwarder * cf, int id, guint32 ack_int,
                                       GSocketConnection * open_conn) {
    Connection * conn = new_connection(cf, id, ack_int);
    if (conn) {
        conn->conn = open_conn;
    }
    return conn;
}

static void close_agent_connection(ConnForwarder * cf, int id) {
    FlexVDIForwardCloseMsg closeMsg;
    closeMsg.id = id;
    send_command(cf, FLEXVDI_FWDCLOSE, (const guint8 *)&closeMsg, sizeof(closeMsg));
}

static void close_connection_no_notify(Connection * conn) {
    g_debug("Start closing connection %u with %d refs", conn->id, conn->refs);
    if (!g_cancellable_is_cancelled(conn->cancellable))
        g_cancellable_cancel(conn->cancellable);
    if (!g_hash_table_remove(conn->cf->connections, GUINT_TO_POINTER(conn->id)))
        g_debug("Connection not found in hash table with id %p???", GUINT_TO_POINTER(conn->id));
}

static void close_connection(Connection * conn) {
    close_agent_connection(conn->cf, conn->id);
    close_connection_no_notify(conn);
}

#define TYPE_ADDRESS_PORT            (address_port_get_type ())
#define ADDRESS_PORT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), TYPE_ADDRESS_PORT, AddressPort))
#define IS_ADDRESS_PORT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), TYPE_ADDRESS_PORT))
#define ADDRESS_PORT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), TYPE_ADDRESS_PORT, AddressPortClass))
#define IS_ADDRESS_PORT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), TYPE_ADDRESS_PORT))
#define ADDRESS_PORT_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), TYPE_ADDRESS_PORT, AddressPortClass))

typedef struct _AddressPort {
    GObject parent_instance;
    guint16 port;
    gchar * address;
} AddressPort;

typedef struct _AddressPortClass {
    GObjectClass parent_class;
} AddressPortClass;

static GType address_port_get_type(void);

G_DEFINE_TYPE(AddressPort, address_port, G_TYPE_OBJECT);

static void address_port_dispose(GObject * gobject) {
    G_OBJECT_CLASS(address_port_parent_class)->dispose(gobject);
}

static void address_port_finalize(GObject * gobject) {
    AddressPort * self = ADDRESS_PORT(gobject);
    g_free(self->address);
    G_OBJECT_CLASS(address_port_parent_class)->finalize(gobject);
}

static void address_port_class_init(AddressPortClass * klass) {
    GObjectClass * gobject_class = G_OBJECT_CLASS(klass);
    gobject_class->dispose = address_port_dispose;
    gobject_class->finalize = address_port_finalize;
}

static void address_port_init (AddressPort * self) {
}

static gpointer address_port_new(guint16 port, const char * address) {
    AddressPort * p = g_object_new(TYPE_ADDRESS_PORT, NULL);
    p->port = port;
    p->address = g_strdup(address);
    return p;
}

static void listener_accept_callback(GObject * source_object, GAsyncResult * res,
                                     gpointer user_data);

ConnForwarder * conn_forwarder_new(conn_forwarder_send_command_cb cb, gpointer user_data) {
    ConnForwarder * cf = g_malloc(sizeof(ConnForwarder));
    if (cf) {
        g_debug("Created new port forwarder");
        cf->send_command = cb;
        cf->user_data = user_data;
        cf->remote_assocs = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                  NULL, g_object_unref);
        cf->connections = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                NULL, unref_connection);
        cf->listener = g_socket_listener_new();
        cf->listener_cancellable = g_cancellable_new();
        if (!cf->remote_assocs || !cf->connections ||
                !cf->listener || !cf->listener_cancellable) {
            conn_forwarder_delete(cf);
            cf = NULL;
        }
    }
    return cf;
}

void conn_forwarder_delete(ConnForwarder * cf) {
    if (cf) {
        g_debug("Deleting port forwarder");
        if (cf->remote_assocs) {
            g_hash_table_destroy(cf->remote_assocs);
        }
        if (cf->connections) {
            g_hash_table_destroy(cf->connections);
        }
        if (cf->listener_cancellable) {
            g_object_unref(cf->listener_cancellable);
        }
        if (cf->listener) {
            g_socket_listener_close(cf->listener);
        }
        g_free(cf);
    }
}

void conn_forwarder_agent_disconnected(ConnForwarder * cf) {
    g_debug("Agent disconnected, close all connections");
    g_hash_table_remove_all(cf->remote_assocs);
    g_hash_table_remove_all(cf->connections);
}


static gboolean conn_forwarder_disassociate_remote(ConnForwarder * cf, guint16 rport) {
    FlexVDIForwardShutdownMsg msg;

    if (!g_hash_table_remove(cf->remote_assocs, GUINT_TO_POINTER(rport))) {
        g_warning("Remote port %d is not associated with a local port.", rport);
        return FALSE;
    } else {
        g_debug("Disassociate remote port %d", rport);
        msg.listenId = rport;
        send_command(cf, FLEXVDI_FWDSHUTDOWN, (const guint8 *)&msg, sizeof(msg));
        return TRUE;
    }
}


/*
 * XXX Check capability before calling this function
 */
gboolean conn_forwarder_associate_remote(ConnForwarder * cf, const gchar * remote) {
    gchar * bind_address, * guest_port, * host, * host_port;
    if (!tokenize_redirection(remote, &bind_address, &guest_port, &host, &host_port)) {
        g_warning("Unknown redirection '%s'", remote);
        return FALSE;
    }

    guint16 rport = atoi(guest_port), lport = atoi(host_port);
    g_debug("Associate guest %s, port %d -> %s port %d", bind_address, rport, host, lport);
    if (g_hash_table_lookup(cf->remote_assocs, GUINT_TO_POINTER(rport))) {
        conn_forwarder_disassociate_remote(cf, rport);
    }
    g_hash_table_insert(cf->remote_assocs, GUINT_TO_POINTER(rport),
                        address_port_new(lport, host));

    if (!bind_address) {
        bind_address = "localhost";
    }
    int addr_len = strlen(bind_address);
    int msg_len = sizeof(FlexVDIForwardListenMsg) + addr_len + 1;
    FlexVDIForwardListenMsg * msg = g_malloc0(msg_len);
    msg->id = msg->port = rport;
    msg->proto = FLEXVDI_FWDPROTO_TCP;
    msg->addressLength = addr_len;
    strcpy(msg->address, bind_address);
    send_command(cf, FLEXVDI_FWDLISTEN, (const guint8 *)msg, msg_len);
    g_free(msg);
    return TRUE;
}


/*
 * XXX Check capability before calling this function
 */
gboolean conn_forwarder_associate_local(ConnForwarder * cf, const gchar * local) {
    gchar * bind_address, * local_port, * host, * host_port;
    if (!tokenize_redirection(local, &bind_address, &local_port, &host, &host_port)) {
        g_warning("Unknown redirection '%s'", local);
        return FALSE;
    }

    guint16 lport = atoi(local_port), rport = atoi(host_port);
    // Listen and wait for a connection
    gboolean res;
    if (bind_address) {
        GInetAddress * inet_address = g_inet_address_new_from_string(bind_address);
        GSocketAddress * socket_address = g_inet_socket_address_new(inet_address, lport);
        res = g_socket_listener_add_address(cf->listener, socket_address,
                                            G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
                                            address_port_new(rport, host), NULL, NULL);
    } else {
        res = g_socket_listener_add_inet_port(cf->listener, lport,
                                              address_port_new(rport, host), NULL);
    }
    g_cancellable_cancel(cf->listener_cancellable);
    g_object_unref(cf->listener_cancellable);
    cf->listener_cancellable = g_cancellable_new();
    g_socket_listener_accept_async(cf->listener, cf->listener_cancellable,
                                   listener_accept_callback, cf);
    return res;
}

#define DATA_HEAD_SIZE sizeof(FlexVDIMessageHeader)
#define BUFFER_SIZE MAX_MSG_SIZE - DATA_HEAD_SIZE

static guint32 generate_connection_id(void) {
    static guint32 seq = 0;
    return --seq;
}

static void listener_accept_callback(GObject * source_object, GAsyncResult * res,
                                     gpointer user_data) {
    ConnForwarder * cf = (ConnForwarder *)user_data;
    GError * error = NULL;
    GSocketConnection * sc = g_socket_listener_accept_finish(cf->listener, res,
                                                            &source_object, &error);
    if (error) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning("Could not accept connection");
    } else {
        GInetSocketAddress * local_address =
                (GInetSocketAddress *)g_socket_connection_get_local_address(sc, NULL);
        guint16 port = g_inet_socket_address_get_port(local_address);
        AddressPort * host = ADDRESS_PORT(source_object);
        g_debug("Accepted connection on port %d to %s:%d",
                    port, host->address, host->port);

        Connection * conn = new_open_connection(cf, generate_connection_id(),
                                               WINDOW_SIZE / 2, sc);
        if (conn) {
            int addr_len = strlen(host->address);
            int msg_len = sizeof(FlexVDIForwardConnectMsg) + addr_len + 1;
            FlexVDIForwardConnectMsg * msg = g_malloc0(msg_len);
            msg->id = conn->id;
            msg->winSize = conn->ack_interval * 2;
            msg->proto = FLEXVDI_FWDPROTO_TCP;
            msg->port = host->port;
            msg->addressLength = addr_len;
            strcpy(msg->address, host->address);
            send_command(cf, FLEXVDI_FWDCONNECT, (const guint8 *)msg, msg_len);
            g_hash_table_insert(cf->connections, GUINT_TO_POINTER(msg->id), conn);
            g_debug("Inserted connection in table with id %p", GUINT_TO_POINTER(msg->id));
            g_free(msg);
        }

        g_socket_listener_accept_async(cf->listener, cf->listener_cancellable,
                                       listener_accept_callback, cf);
    }
}

static void connection_read_callback(GObject * source_object, GAsyncResult * res,
                                     gpointer user_data);

static void program_read(Connection * conn) {
    GInputStream * stream = g_io_stream_get_input_stream((GIOStream *)conn->conn);
    guint8 * data = conn->read_buffer + DATA_HEAD_SIZE;
    g_input_stream_read_async(stream, data, BUFFER_SIZE, G_PRIORITY_DEFAULT,
                              conn->cancellable, connection_read_callback, conn);
}

static void connection_read_callback(GObject * source_object, GAsyncResult * res,
                                     gpointer user_data) {
    Connection * conn = (Connection *)user_data;
    ConnForwarder * cf = conn->cf;
    GError * error = NULL;
    GInputStream * stream = (GInputStream *)source_object;
    gssize bytes = g_input_stream_read_finish(stream, res, &error);
    FlexVDIForwardDataMsg * msg = (FlexVDIForwardDataMsg *)conn->read_buffer;

    if (g_cancellable_is_cancelled(conn->cancellable)) {
        unref_connection(conn);
        return;
    }

    if (error || bytes == 0) {
        /* Error or connection closed by peer */
        if (error)
            g_debug("Read error on connection %u: %s", conn->id, error->message);
        else
            g_debug("Connection %u reset by peer", conn->id);
        close_connection(conn);
        unref_connection(conn);
    } else {
        msg->id = conn->id;
        msg->size = bytes;
        send_command(cf, FLEXVDI_FWDDATA,
                     conn->read_buffer, DATA_HEAD_SIZE + msg->size);
        conn->data_sent += msg->size;
        if (conn->data_sent < WINDOW_SIZE) {
            program_read(conn);
        } else {
            unref_connection(conn);
        }
    }
}

static void connection_write_callback(GObject * source_object, GAsyncResult * res,
                                      gpointer user_data) {
    Connection * conn = (Connection *)user_data;
    GOutputStream * stream = (GOutputStream *)source_object;
    GBytes * bytes, * new_bytes;
    GError * error = NULL;
    int num_written = g_output_stream_write_finish(stream, res, &error);
    int remaining;
    FlexVDIForwardAckMsg msg;

    if (error != NULL) {
        /* Error or connection closed by peer */
        g_debug("Write error on connection %u: %s", conn->id, error->message);
        close_connection(conn);
        unref_connection(conn);
    } else {
        bytes = (GBytes *)g_queue_pop_head(conn->write_buffer);
        g_debug("Written %d bytes on connection %u", num_written, conn->id);
        remaining = g_bytes_get_size(bytes) - num_written;
        if (remaining) {
            g_debug("Still %d bytes to go on connection %u", remaining, conn->id);
            new_bytes = g_bytes_new_from_bytes(bytes, num_written, remaining);
            g_queue_push_head(conn->write_buffer, new_bytes);
        }
        if(!g_queue_is_empty(conn->write_buffer)) {
            new_bytes = g_queue_peek_head(conn->write_buffer);
            g_output_stream_write_async(stream, g_bytes_get_data(new_bytes, NULL),
                                        g_bytes_get_size(new_bytes), G_PRIORITY_DEFAULT,
                                        NULL, connection_write_callback, conn);
        } else {
            unref_connection(conn);
        }
        g_bytes_unref(bytes);

        conn->data_received += num_written;
        if (conn->data_received >= conn->ack_interval) {
            msg.id = conn->id;
            msg.size = conn->data_received;
            msg.winSize = conn->ack_interval * 2;
            conn->data_received = 0;
            send_command(conn->cf, FLEXVDI_FWDACK,
                         (const guint8 *)&msg, sizeof(msg));
        }
    }
}

static void connection_connect_callback(GObject * source_object, GAsyncResult * res,
                                        gpointer user_data) {
    Connection * conn = (Connection *)user_data;
    FlexVDIForwardAckMsg msg = {.id = conn->id, .size = 0, .winSize = WINDOW_SIZE};

    if (g_cancellable_is_cancelled(conn->cancellable)) {
        unref_connection(conn);
        return;
    }

    conn->conn = g_socket_client_connect_to_host_finish((GSocketClient *)source_object, res, NULL);
    if (!conn->conn) {
        /* Error */
        g_debug("Connection %u could not connect", conn->id);
        close_connection(conn);
        unref_connection(conn);
    } else {
        conn->connecting = FALSE;
        program_read(conn);
        send_command(conn->cf, FLEXVDI_FWDACK,
                     (const guint8 *)&msg, sizeof(msg));
    }
}

static void handle_accepted(ConnForwarder * cf, FlexVDIForwardAcceptedMsg * msg) {
    gpointer id = GUINT_TO_POINTER(msg->id), rport = GUINT_TO_POINTER(msg->listenId);
    AddressPort * local;
    Connection * conn = g_hash_table_lookup(cf->connections, id);
    if (conn) {
        g_warning("Connection %u already exists.", msg->id);
        close_connection_no_notify(conn);
    }

    local = ADDRESS_PORT(g_hash_table_lookup(cf->remote_assocs, rport));
    g_debug("Connection command, id %u on remote port %d -> %s port %d",
            msg->id, msg->listenId, local->address, local->port);
    if (local) {
        conn = new_connection_with_socket(cf, msg->id, msg->winSize / 2);
        if (conn) {
            g_hash_table_insert(cf->connections, id, conn);
            conn->refs++;
            g_socket_client_connect_to_host_async(conn->socket, local->address,
                                                  local->port, conn->cancellable,
                                                  connection_connect_callback, conn);
        } else {
            /* Error, close connection in agent */
            close_agent_connection(cf, msg->id);
        }
    } else {
        g_warning("Remote port %d is not associated with a local port.", msg->listenId);
        close_agent_connection(cf, msg->id);
    }
}

static void handle_data(ConnForwarder * cf, FlexVDIForwardDataMsg * msg) {
    Connection * conn = g_hash_table_lookup(cf->connections, GUINT_TO_POINTER(msg->id));
    GBytes * chunk;
    GOutputStream * stream;

    if (!conn) {
        /* Ignore, this is usually an already closed connection */
        g_debug("Connection %u does not exist.", msg->id);
    } else if (conn->connecting) {
        g_warning("Connection %u is still not connected!", conn->id);
    } else {
        chunk = g_bytes_new(msg->data, msg->size);
        g_queue_push_tail(conn->write_buffer, chunk);
        if (g_queue_get_length(conn->write_buffer) == 1) {
            conn->refs++;
            stream = g_io_stream_get_output_stream((GIOStream *)conn->conn);
            g_output_stream_write_async(stream, g_bytes_get_data(chunk, NULL),
                                        g_bytes_get_size(chunk), G_PRIORITY_DEFAULT,
                                        NULL, connection_write_callback, conn);
        }
    }
}

static void handle_close(ConnForwarder * cf, FlexVDIForwardCloseMsg * msg) {
    Connection * conn = g_hash_table_lookup(cf->connections, GUINT_TO_POINTER(msg->id));
    if (conn) {
        g_debug("Close command for connection %u", conn->id);
        close_connection_no_notify(conn);
    } else {
        /* This is usually an already closed connection */
        g_debug("Connection %u does not exists.", msg->id);
        close_agent_connection(cf, msg->id);
    }
}

static void handle_ack(ConnForwarder * cf, FlexVDIForwardAckMsg * msg) {
    Connection * conn = g_hash_table_lookup(cf->connections, GUINT_TO_POINTER(msg->id));
    g_debug("ACK command for connection %u with %d bytes", msg->id, (int)msg->size);
    if (conn) {
        if (conn->connecting) {
            conn->connecting = FALSE;
            conn->ack_interval = msg->winSize / 2;
            conn->refs++;
            program_read(conn);
        } else {
            guint32 data_sent_before = conn->data_sent;
            conn->data_sent -= msg->size;
            if (conn->data_sent < WINDOW_SIZE && data_sent_before >= WINDOW_SIZE) {
                conn->refs++;
                program_read(conn);
            }
        }
    } else {
        /* Ignore, this is usually an already closed connection */
        g_debug("Connection %u does not exists.", msg->id);
    }
}

void conn_forwarder_handle_message(ConnForwarder* cf, guint32 command, gpointer msg) {
    switch (command) {
        case FLEXVDI_FWDACCEPTED:
            handle_accepted(cf, (FlexVDIForwardAcceptedMsg *)msg);
            break;
        case FLEXVDI_FWDDATA:
            handle_data(cf, (FlexVDIForwardDataMsg *)msg);
            break;
        case FLEXVDI_FWDCLOSE:
            handle_close(cf, (FlexVDIForwardCloseMsg *)msg);
            break;
        case FLEXVDI_FWDACK:
            handle_ack(cf, (FlexVDIForwardAckMsg *)msg);
            break;
        default:
            break;
    }
}
