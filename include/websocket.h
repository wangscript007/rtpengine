#ifndef __WEBSOCKET_H__
#define __WEBSOCKET_H__

#include "str.h"


struct websocket_conn;
struct websocket_message;
enum lws_write_protocol;

typedef const char *(*websocket_message_func_t)(struct websocket_message *);


struct websocket_message {
	struct websocket_conn *wc;
	char *uri;
	enum {
		M_UNKNOWN = 0,
		M_WEBSOCKET,
		M_GET,
		M_POST,
	} method;
	enum {
		CT_UNKNOWN = 0,
		CT_JSON,
	} content_type;
	GString *body;

	websocket_message_func_t func;
};


int websocket_init(void);
void websocket_start(void);

// appends to output buffer without triggering a response
void websocket_queue_raw(struct websocket_conn *wc, const char *msg, size_t len);
// adds data to output buffer (can be null) and triggers specified response
int websocket_write_raw(struct websocket_conn *wc, const char *msg, size_t len,
		enum lws_write_protocol protocol);
// adds data to output buffer (can be null) and triggers specified response: http or binary websocket
int websocket_write_http_len(struct websocket_conn *wc, const char *msg, size_t len);
int websocket_write_http(struct websocket_conn *wc, const char *msg);
int websocket_write_text(struct websocket_conn *wc, const char *msg);
int websocket_write_binary(struct websocket_conn *wc, const char *msg, size_t len);
// num bytes in output buffer
size_t websocket_queue_len(struct websocket_conn *wc);

// write HTTP response headers
int websocket_http_response(struct websocket_conn *wc, int status, const char *content_type,
		ssize_t content_length);

#endif
