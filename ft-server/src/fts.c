#include "fts.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <pthread.h>

#include <event2/event.h>
#include <event2/thread.h>

#include "ft_service.h"
#include "ftcp.h"
#include "socket2.h"
#include "nproto/nproto_ipv4.h"
#include "tproto/tproto_tcp.h"
#include "tpool.h"
#include "rwfslock.h"
#include "try.h"
#include "utilities.h"

#ifdef __unix__
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

#define BACKLOG 4096

#ifdef WIN32
#define evthread_use_threads evthread_use_windows_threads
#define nprocs() 2
#elif __unix__
#define nprocs() get_nprocs()
#define evthread_use_threads evthread_use_pthreads
#endif

static void fts_close(evutil_socket_t signal, short events, void* arg);
static void dispatch_request(evutil_socket_t fd, short events, void* arg);
static void* handle_request(void* arg);
static void handle_list_request(struct socket2* socket, ftcp_pp_t request);
static void handle_get_request(struct socket2* socket, ftcp_pp_t request);
static void handle_put_request(struct socket2* socket, ftcp_pp_t request);
static void handle_invalid_request(struct socket2* socket, ftcp_pp_t request);

static ft_service_t service;
static tpool_t thread_pool;
static struct event_base* event_base;
static struct rwfslock rwfslock;

extern int fts_start(int port, ft_service_t ft_service) {
	struct event* socket_event;
	struct event* signal_event;
	struct nproto_ipv4 ipv4;
	struct tproto_tcp tcp;
	struct socket2* socket;
	char* url;
	service = ft_service;
	try(asprintf(&url, "127.0.0.1:%d", port), -1, fail);
	try(thread_pool = tpool_init(nprocs()), NULL, fail);
	try(evthread_use_threads(), -1, fail);
	try(event_base = event_base_new(), NULL, fail);
	try(rwfslock_init(&rwfslock), 1, fail);
	nproto_ipv4_init(&ipv4);
	tproto_tcp_init(&tcp);
	try(socket = new(socket2, &tcp.super.tproto, &ipv4.super.nproto, true), NULL, fail);
	try(socket2_set_blocking(socket, false), 1, fail);
	try(socket2_listen(socket, url, BACKLOG), 1, fail);
	try(socket_event = event_new(event_base, socket2_get_fd(socket), EV_READ | EV_PERSIST, dispatch_request, socket), NULL, fail);
	try(signal_event = evsignal_new(event_base, SIGINT, fts_close, (void*)event_base), NULL, fail);
	try(event_add(socket_event, NULL), -1, fail);
	try(event_add(signal_event, NULL), -1, fail);
	try((printf("Server started.\n") < 0), true, fail);
	try(event_base_dispatch(event_base), -1, fail);
	event_free(socket_event);
	event_free(signal_event);
	event_base_free(event_base);
	libevent_global_shutdown();
	try(delete(socket), 1, fail);
	try(tpool_wait(thread_pool), 1, fail);
	try(tpool_destroy(thread_pool), 1, fail);
	try(rwfslock_destroy(&rwfslock), 1, fail);
	free(url);
	return 0;
fail:
	return 1;
}

static void fts_close(evutil_socket_t signal, short events, void* user_data) {
	struct event_base* base = user_data;
	struct timeval delay = { 1, 0 };
	printf("\nShutting down the server...\n");
	event_base_loopexit(base, &delay);
}

static void dispatch_request(evutil_socket_t fd, short events, void* arg) {
	struct socket2* listening_socket = arg;
	struct socket2* accepted = socket2_accept(listening_socket);
	tpool_add_work(thread_pool, handle_request, accepted);
}

static void* handle_request(void* arg) {
	struct socket2* socket = arg;
	ftcp_pp_t request = malloc(ftcp_pp_size());
	socket2_recv(socket, request, ftcp_pp_size());
	switch (ftcp_get_type(request)) {
	case COMMAND:
		switch (ftcp_get_operation(request)) {
		case LIST:
			handle_list_request(socket, request);
			break;
		case GET:
			handle_get_request(socket, request);
			break;
		case PUT:
			handle_put_request(socket, request);
			break;
		default:
			handle_invalid_request(socket, request);
		}
		break;
	default:
		handle_invalid_request(socket, request);
	}
	delete(socket);
	free(request);
}

static void handle_list_request(struct socket2* socket, ftcp_pp_t request) {
	ftcp_pp_t reply;
	FILE* pipe;
	char* filelist = ft_service_get_filelist(service);
	reply = ftcp_pp_init(RESPONSE, SUCCESS, NULL, strlen(filelist));
	socket2_send(socket, reply, ftcp_pp_size());
	socket2_ssend(socket, filelist);
	free(filelist);
	free(reply);
}

static void handle_get_request(struct socket2* socket, ftcp_pp_t request) {
	ftcp_pp_t reply;
	char* frpath;
	char* fpath;
	FILE* file;
	uint64_t flen;
	bool fexist;
	frpath = ftcp_get_arg(request);
	if (strstr(frpath, "/")) {
		reply = ftcp_pp_init(RESPONSE, INVALID_ARGUMENT, NULL, 0);
		socket2_send(socket, reply, ftcp_pp_size());
		return;
	}
	asprintf(&fpath, "%s/%s", ft_service_get_base_dir(service), frpath);
	if (!access(fpath, F_OK)) {
		try(rwfslock_rdlock(&rwfslock, fpath), 1, fail);
		file = fopen(fpath, "r");
		fseek(file, 0L, SEEK_END);
		flen = ftell(file);
		fseek(file, 0L, SEEK_SET);
		reply = ftcp_pp_init(RESPONSE, FILE_EXIST, frpath, flen);
		socket2_send(socket, reply, ftcp_pp_size());
		socket2_fsend(socket, file);
		fclose(file);
		try(rwfslock_unlock(&rwfslock, fpath), 1, fail);
	}
	else {
		reply = ftcp_pp_init(RESPONSE, FILE_NOT_EXIST, NULL, 0);
		socket2_send(socket, reply, ftcp_pp_size());
	}
	free(fpath);
	free(reply);
fail:
	return;
}

static void handle_put_request(struct socket2* socket, ftcp_pp_t request) {
	ftcp_pp_t reply;
	FILE* file;
	char* fpath;
	enum ftcp_result result;
	asprintf(&fpath, "%s/%s", ft_service_get_base_dir(service), ftcp_get_arg(request));
	result = access(fpath, F_OK) ? FILE_NOT_EXIST : FILE_EXIST;
	try(rwfslock_wrlock(&rwfslock, fpath), 1, fail);
	file = fopen(fpath, "w");
	reply = ftcp_pp_init(RESPONSE, result, NULL, 0);
	socket2_send(socket, reply, ftcp_pp_size());
	free(reply);
	socket2_frecv(socket, file, ftcp_get_dplen(request));
	reply = ftcp_pp_init(RESPONSE, SUCCESS, NULL, 0);
	socket2_send(socket, reply, ftcp_pp_size());
	fclose(file);
	try(rwfslock_unlock(&rwfslock, fpath), 1, fail);
	free(fpath);
	free(reply);
fail:
	return;
}

static void handle_invalid_request(struct socket2* socket, ftcp_pp_t request) {
	ftcp_pp_t reply;
	reply = ftcp_pp_init(RESPONSE, ERROR, NULL, 0);
	socket2_send(socket, reply, ftcp_pp_size());
	free(reply);
}
