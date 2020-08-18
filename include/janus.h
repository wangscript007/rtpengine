#ifndef __JANUS_H__
#define __JANUS_H__

struct websocket_conn;
struct websocket_message;


void janus_init(void);
void janus_free(void);

const char *websocket_janus_process(struct websocket_message *wm);


#endif
