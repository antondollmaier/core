/* Copyright (C) 2003-2004 Timo Sirainen */

#include "lib.h"
#include "str.h"
#include "hash.h"
#include "ostream.h"
#include "auth-client.h"
#include "auth-server-connection.h"
#include "auth-server-request.h"

#include <stdlib.h>

struct auth_request {
        struct auth_server_connection *conn;

	unsigned int id;

	char *mech, *service;
        enum auth_request_flags flags;
	struct ip_addr local_ip, remote_ip;

	char *initial_resp_base64;

	auth_request_callback_t *callback;
	void *context;

        struct auth_server_connection *next_conn;
	char *plaintext_data; /* for resending to other servers */

	unsigned int init_sent:1;
	unsigned int retrying:1;
};

static int auth_server_send_new_request(struct auth_server_connection *conn,
					struct auth_request *request);
static void auth_client_request_free(struct auth_request *request);

static struct auth_server_connection *
get_next_plain_server(struct auth_server_connection *conn)
{
	conn = conn->next;
	while (conn != NULL) {
		if (conn->has_plain_mech)
			return conn;
		conn = conn->next;
	}
	return NULL;
}

static void
auth_server_request_check_retry(struct auth_request *request, const char *data)
{
	if (strcmp(request->mech, "PLAIN") == 0 && data != NULL &&
	    request->plaintext_data == NULL && request->conn != NULL) {
		request->next_conn = get_next_plain_server(request->conn);
		if (request->next_conn != NULL) {
			/* plaintext authentication - save the data so we can
			   try it for the next */
			request->plaintext_data = i_strdup(data);

			hash_insert(request->next_conn->requests,
				    POINTER_CAST(request->id), request);
			auth_server_send_new_request(request->next_conn,
						     request);
			request->retrying = TRUE;
		}
	}
}

static int auth_server_send_new_request(struct auth_server_connection *conn,
					struct auth_request *request)
{
	string_t *str;
	ssize_t ret;

	t_push();
	str = t_str_new(512);

	str_printfa(str, "AUTH\t%u\t%s\tservice=%s",
		    request->id, request->mech, request->service);
	if ((request->flags & AUTH_REQUEST_FLAG_SECURED) != 0)
		str_append(str, "\tsecured");
	if ((request->flags & AUTH_REQUEST_FLAG_VALID_CLIENT_CERT) != 0)
		str_append(str, "\tvalid-client-cert");

	if (request->local_ip.family != 0)
		str_printfa(str, "\tlip=%s", net_ip2addr(&request->local_ip));
	if (request->remote_ip.family != 0)
		str_printfa(str, "\trip=%s", net_ip2addr(&request->remote_ip));
	if (request->initial_resp_base64 != NULL)
		str_printfa(str, "\tresp=%s", request->initial_resp_base64);
	str_append_c(str, '\n');

	ret = o_stream_send(conn->output, str_data(str), str_len(str));
	t_pop();

	if (ret < 0) {
		errno = conn->output->stream_errno;
		i_warning("Error sending request to auth server: %m");
		auth_server_connection_destroy(conn, TRUE);
		return FALSE;
	}

	auth_server_request_check_retry(request, request->initial_resp_base64);
	return TRUE;
}

static void auth_server_send_continue(struct auth_server_connection *conn,
				      struct auth_request *request,
				      const char *data_base64)
{
	struct const_iovec iov[3];
	const char *prefix;

	prefix = t_strdup_printf("CONT\t%u\t", request->id);

	iov[0].iov_base = prefix;
	iov[0].iov_len = strlen(prefix);
	iov[1].iov_base = data_base64;
	iov[1].iov_len = strlen(data_base64);
	iov[2].iov_base = "\n";
	iov[2].iov_len = 1;

	if (o_stream_sendv(conn->output, iov, 3) < 0) {
		errno = conn->output->stream_errno;
		i_warning("Error sending continue request to auth server: %m");
		auth_server_connection_destroy(conn, TRUE);
	}
}

int auth_client_input_ok(struct auth_server_connection *conn, const char *args)
{
	const char *const *list, *const *args_list, *data_base64;
	struct auth_request *request;
	unsigned int id;

	list = t_strsplit(args, "\t");
	if (list[0] == NULL) {
		i_error("BUG: Authentication server sent broken OK line");
		return FALSE;
	}

	id = (unsigned int)strtoul(list[0], NULL, 10);

	request = hash_lookup(conn->requests, POINTER_CAST(id));
	if (request == NULL) {
		/* We've already destroyed the request */
		return TRUE;
	}

	hash_remove(request->conn->requests, POINTER_CAST(id));
	if (request->next_conn != NULL)
		hash_remove(request->next_conn->requests, POINTER_CAST(id));
	request->conn = conn;
	request->next_conn = NULL;

	data_base64 = NULL;
	for (args_list = ++list; *list != NULL; list++) {
		if (strncmp(*list, "resp=", 5) == 0) {
			data_base64 = *list + 5;
			break;
		}
	}

	request->callback(request, 1, data_base64, args_list, request->context);
	auth_client_request_free(request);
	return TRUE;
}

int auth_client_input_cont(struct auth_server_connection *conn,
			   const char *args)
{
	struct auth_request *request;
	const char *data;
	unsigned int id;

	data = strchr(args, '\t');
	if (data++ == NULL) {
		i_error("BUG: Authentication server sent broken CONT line");
		return FALSE;
	}

	id = (unsigned int)strtoul(args, NULL, 10);

	request = hash_lookup(conn->requests, POINTER_CAST(id));
	if (request == NULL) {
		/* We've already destroyed the request */
		return TRUE;
	}

	if (request->retrying) {
		auth_server_send_continue(conn, request,
					  request->plaintext_data);
	} else {
		request->callback(request, 0, data, NULL, request->context);
	}
	return TRUE;
}

int auth_client_input_fail(struct auth_server_connection *conn,
			   const char *args)
{
	struct auth_request *request;
        struct auth_server_connection *next;
	const char *const *list;
	unsigned int id;

	list = t_strsplit(args, "\t");
	if (list[0] == NULL) {
		i_error("BUG: Authentication server sent broken OK line");
		return FALSE;
	}

	id = (unsigned int)strtoul(list[0], NULL, 10);

	request = hash_lookup(conn->requests, POINTER_CAST(id));
	if (request == NULL) {
		/* We've already destroyed the request */
		return TRUE;
	}

	hash_remove(conn->requests, POINTER_CAST(request->id));
	if (request->retrying) {
		next = request->next_conn == NULL ? NULL :
			get_next_plain_server(request->next_conn);

		if (conn == request->conn)
			request->conn = request->next_conn;
		request->next_conn = NULL;

		if (next == NULL) {
			if (request->conn != NULL) {
				/* the other one hasn't replied yet */
				return TRUE;
			}
			request->conn = conn;
		} else {
			hash_insert(next->requests, POINTER_CAST(request->id),
				    request);
			request->next_conn = next;

			auth_server_send_new_request(next, request);
			return TRUE;
		}
	}

	request->callback(request, -1, NULL, list+1, request->context);
	auth_client_request_free(request);
	return TRUE;
}

static void request_hash_remove(struct auth_server_connection *conn,
                                struct auth_request *request)
{
	if (request->conn == conn) {
		if (request->next_conn == NULL) {
			request->callback(request, -1, NULL, NULL,
					  request->context);
			request->conn = NULL;
		} else {
			request->conn = request->next_conn;
			request->next_conn = NULL;
		}
	} else {
		request->next_conn = NULL;
	}
}

void auth_server_requests_remove_all(struct auth_server_connection *conn)
{
	struct hash_iterate_context *iter;
	void *key, *value;

	iter = hash_iterate_init(conn->requests);
	while (hash_iterate(iter, &key, &value))
		request_hash_remove(conn, value);
	hash_iterate_deinit(iter);
}

struct auth_request *
auth_client_request_new(struct auth_client *client, struct auth_connect_id *id,
			const struct auth_request_info *request_info,
			auth_request_callback_t *callback, void *context,
			const char **error_r)
{
	struct auth_server_connection *conn;
	struct auth_request *request;

	if (id == NULL) {
		conn = auth_server_connection_find_mech(client,
							request_info->mech,
							error_r);
	} else {
		*error_r = NULL;
		conn = client->connections;
		for (; conn != NULL; conn = conn->next) {
			if (conn->connect_uid == id->connect_uid &&
			    conn->server_pid == id->server_pid)
				break;
		}
	}

	if (conn == NULL)
		return NULL;

	request = i_new(struct auth_request, 1);
	request->conn = conn;
	request->mech = i_strdup(request_info->mech);
	request->service = i_strdup(request_info->service);
	request->flags = request_info->flags;
	request->local_ip = request_info->local_ip;
	request->remote_ip = request_info->remote_ip;
	request->id = ++client->request_id_counter;

	if (request_info->initial_resp_base64 != NULL) {
		request->initial_resp_base64 =
			i_strdup(request_info->initial_resp_base64);
	}
	
	if (request->id == 0) {
		/* wrapped - ID 0 not allowed */
		request->id = ++client->request_id_counter;
	}
	request->callback = callback;
	request->context = context;

	hash_insert(conn->requests, POINTER_CAST(request->id), request);

	if (!auth_server_send_new_request(conn, request))
		request = NULL;
	return request;
}

void auth_client_request_continue(struct auth_request *request,
                                  const char *data_base64)
{
	auth_server_send_continue(request->conn, request, data_base64);
	auth_server_request_check_retry(request, data_base64);
}

static void auth_client_request_free(struct auth_request *request)
{
	i_free(request->initial_resp_base64);
	i_free(request->plaintext_data);
	i_free(request->mech);
	i_free(request->service);
	i_free(request);
}

void auth_client_request_abort(struct auth_request *request)
{
	void *id = POINTER_CAST(request->id);

	hash_remove(request->conn->requests, id);
	if (request->next_conn != NULL)
		hash_remove(request->next_conn->requests, id);

	request->callback(request, -1, NULL, NULL, request->context);
	auth_client_request_free(request);
}

unsigned int auth_client_request_get_id(struct auth_request *request)
{
	return request->id;
}

unsigned int auth_client_request_get_server_pid(struct auth_request *request)
{
	return request->conn->server_pid;
}
