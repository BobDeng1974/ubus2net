#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <limits.h>
#include <errno.h>
#include <time.h>


#include "common.h"
#include "lockqueue.h"
#include "tcp.h"

#include "log.h"
#include "timer.h"
#include "file_event.h"
#include "json_parser.h"

#include <libubox/blobmsg_json.h>
#include <libubox/avl.h>
#include <libubus.h>


#include "jansson.h"



///////////////////////////////////////////////////////////////
//test module
void ubus2net();

///////////////////////////////////////////////////////////////
static int ds_child_died = 0;

struct timer_head th = {
	.first = NULL,
};
///////////////////////////////////////////////////////////////
static void ds_child_exit_handler(int s) {
	ds_child_died = 1;
}
static void ds_sig_exit_handler(int s) {
	log_debug("Caught signal %d", s);
	exit(1);
}
static void ds_sigpipe_handler(int s) {
	log_warn("Caught SIGPIPE");
}
static void ds_exit_handler(void) {
	log_debug("inside exit handler");
}

static void sig_set() {
	struct sigaction sigHandler;

	memset(&sigHandler, 0, sizeof(sigHandler));

	sigHandler.sa_handler = ds_sig_exit_handler;
	sigemptyset(&sigHandler.sa_mask);
	sigaction(SIGINT, &sigHandler, NULL);
	sigaction(SIGTERM, &sigHandler, NULL);

	sigHandler.sa_handler = ds_child_exit_handler;
	sigaction(SIGCHLD, &sigHandler, NULL);

	sigHandler.sa_handler = ds_sigpipe_handler;
	sigaction(SIGPIPE, &sigHandler, NULL);

	atexit(ds_exit_handler);
}

int main(int argc, char *argv[]) {
	sig_set();

	log_init(argv[0], LOG_OPT_DEBUG | LOG_OPT_CONSOLE_OUT | LOG_OPT_TIMESTAMPS | LOG_OPT_FUNC_NAMES);

	while (1) {
		ubus2net();
	}
	return 0;
}

////////////////////////////////////////////////////////////////
int ubus_init(void *_th, void *_fet);
int clie_init(void *_th, void *_fet);

void timerout_cb(struct timer *t) {
	log_info("========================api test==================");
	timer_set(&th, t, 20000);
}

void ubus2net() {
	struct timer tr;
	timer_init(&tr, timerout_cb);
	timer_set(&th, &tr, 1000);

	struct file_event_table fet;
	file_event_init(&fet);

	ubus_init(&th, &fet);
	clie_init(&th, &fet);

	while (1) {
		s64 next_timeout_ms;
		next_timeout_ms = timer_advance(&th);
		if (file_event_poll(&fet, next_timeout_ms) < 0) {
			log_warn("poll error: %m");
		}
	}
}

/* Event */
typedef struct stEvent {
	int type;
	int len;
	void *data;
}stEvent_t;

stEvent_t *event_packet(int _type, int _len, void *data) {
	stEvent_t *p = (stEvent_t *)MALLOC(sizeof(stEvent_t) + _len);
	p->type = _type;
	p->len = _len;
	p->data = p+1;
	if (_len > 0 && data != NULL) {
		memcpy(p->data, data, p->len);
	}
	return p;
}

/* module ubus */
typedef struct stUbusEnv {
	struct ubus_context *ubus_ctx;
	struct ubus_event_handler listener;
	struct file_event_table *fet;
	struct timer_head *th;
	struct timer step_timer;
	stLockQueue_t eq;
	
}stUbusEnv_t;
stUbusEnv_t ue;
static struct blob_buf b;

static void receive_ubus_event(struct ubus_context *ctx,
															 struct ubus_event_handler *ev,
															 const char *type, struct blob_attr *msg);
void ubus_run(struct timer *timer);
void ubus_in(void *arg, int fd);
int clie_push(stEvent_t *e);


int ubus_init(void *_th, void *_fet) {
	ue.th = _th;
	ue.fet = _fet;

	timer_init(&ue.step_timer, ubus_run);

	ue.ubus_ctx = ubus_connect(NULL);
	memset(&ue.listener, 0, sizeof(ue.listener));
	ue.listener.cb = receive_ubus_event;
	ubus_register_event_handler(ue.ubus_ctx, &ue.listener, "DS.GREENPOWER");

	file_event_reg(ue.fet, ue.ubus_ctx->sock.fd, ubus_in, NULL, NULL);

	lockqueue_init(&ue.eq);

	return 0;
}

int ubus_step() {
	timer_cancel(ue.th, &ue.step_timer);
	timer_set(ue.th, &ue.step_timer, 10);
	return 0;
}

int ubus_push(stEvent_t *e) {
	lockqueue_push(&ue.eq, e);
	ubus_step();
	return 0;
}

void ubus_run(struct timer *timer) {
	stEvent_t *e;
	if (!lockqueue_pop(&ue.eq, (void**)&e)) {
		return;
	}
	if (e == NULL) {
		return;
	}
	if (e->type == 0 && e->data != NULL) {
		blob_buf_init(&b, 0);
		blobmsg_add_string(&b, "PKT", (char*)e->data);
		log_debug("ubus send:%s", (char*)e->data);
		ubus_send_event(ue.ubus_ctx, "DS.GATEWAY", b.head);
	}
	
	FREE(e);
	
	ubus_step();
}
void ubus_in(void *arg, int fd) {
	ubus_handle_event(ue.ubus_ctx);
}


static void receive_ubus_event(struct ubus_context *ctx, struct ubus_event_handler *ev,
			  const char *type, struct blob_attr *msg) {
	char *str;

	log_debug("-----------------[ubus msg]: handler ....-----------------");
	str = blobmsg_format_json(msg, true);
	if (str != NULL) {
		log_debug("[ubus msg]: [%s]", str);

		json_error_t error;
		json_t *jmsg = json_loads(str, 0, &error);
		if (jmsg != NULL) {
			const char *spkt = json_get_string(jmsg, "PKT");
			if (spkt != NULL) {
				stEvent_t *e = event_packet(0, strlen(spkt)+1, (void*)spkt);
				clie_push(e);
			} else {
				log_debug("not find 'PKT' item!");
			}
			json_decref(jmsg);
		} else {
			log_debug("error: on line %d: %s", error.line, error.text);
		}
		free(str);
	} else {
		log_debug("[ubus msg]: []");
	}
	log_debug("-----------------[ubus msg]: handler over-----------------");
}


/* module clie */
typedef struct stClieEnv {
	struct timer step_timer;
	stLockQueue_t eq;
	struct file_event_table *fet;
	struct timer_head *th;

	int fd;
}stClieEnv_t;

stClieEnv_t ce;
void clie_run(struct timer *timer);
void clie_in(void *arg, int fd);

int clie_init(void *_th, void *_fet) {
	ce.th = _th;
	ce.fet = _fet;

	timer_init(&ce.step_timer, clie_run);
	lockqueue_init(&ce.eq);

	ce.fd = tcp_init(0, "192.168.0.230", 19000);
	if (ce.fd > 0) {
		file_event_reg(ce.fet, ce.fd, clie_in, NULL, NULL);
	} else {
		log_debug("connect to 192.168.0.230 failed!");
		return -1;
	}

	return 0;
}

int clie_step() {
	timer_cancel(ce.th, &ce.step_timer);
	timer_set(ce.th, &ce.step_timer, 10);
	return 0;
}

int clie_push(stEvent_t *e) {
	lockqueue_push(&ce.eq, e);
	clie_step();
	return 0;
}

void clie_run(struct timer *timer) {
	stEvent_t *e;
	if (!lockqueue_pop(&ce.eq, (void**)&e) != 0) {
		return;
	}
	if (e == NULL) {
		return;
	}

	log_debug("clie msg:%s", (char*)e->data);

	if (e->type == 0 && e->data != NULL) {
		int ret = tcp_send(ce.fd, e->data, e->len, 0, 8000);
		if (ret <= 0) {
			log_debug("socket error !, close it");

			file_event_unreg(ce.fet, ce.fd, NULL, NULL, NULL);

			tcp_free(ce.fd);
			ce.fd = -1;
		} 
	}


	FREE(e);
	
	clie_step();
}

void clie_in(void *arg, int fd) {
	log_debug("[%s]", __func__);

	char buf[1024];	
	int ret = tcp_recv(ce.fd, buf, sizeof(buf), 0, 8000);
	if (ret <= 0) {
		log_debug("socket error, recv: close it");
		file_event_unreg(ce.fet, ce.fd, NULL, NULL, NULL);
		tcp_free(ce.fd);
		ce.fd = -1;
	} else {
		log_debug_hex("recv", buf, ret);
		if (buf[ret-1] == '\n') {
			buf[ret-1] = 0;
			ret--;
		} 

		buf[ret++] = 0;
		log_debug("%s", buf);

		stEvent_t *e = event_packet(0,  ret, buf);
		ubus_push(e);
	}
}


