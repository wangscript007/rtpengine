#include "janus.h"
#include <json-glib/json-glib.h>
#include "websocket.h"
#include "log.h"
#include "main.h"
#include "obj.h"


struct janus_session { // "login" session
	struct obj obj;
	uint64_t id;
	mutex_t lock;
	time_t last_act;
	GHashTable *websockets; // controlling transports
	GHashTable *handles;
};
struct janus_handle { // corresponds to a conference participant
	uint64_t id;
};
struct janus_room {
	uint64_t id;
	int publishers;
	struct janus_session *session; // controlling session
};


static mutex_t janus_lock;
static GHashTable *janus_tokens;
static GHashTable *janus_sessions;
static GHashTable *janus_handles;
static GHashTable *janus_rooms;


static void __janus_session_free(void *p) {
	struct janus_session *s = p;
	g_hash_table_destroy(s->websockets);
	g_hash_table_destroy(s->handles);
	mutex_destroy(&s->lock);
}


// XXX we have several hash tables that hold references to objs - unify all these
static struct janus_session *janus_get_session(uint64_t id) {
	mutex_lock(&janus_lock);
	struct janus_session *ret = g_hash_table_lookup(janus_sessions, &id);
	if (ret)
		obj_hold(ret);
	mutex_unlock(&janus_lock);
	if (!ret)
		return NULL;
	mutex_lock(&ret->lock);
	ret->last_act = rtpe_now.tv_sec;
	mutex_unlock(&ret->lock);
	return ret;
}


const char *websocket_janus_videoroom(struct websocket_message *wm, struct janus_session *session,
		JsonBuilder *builder, JsonReader *reader,
		int *retcodep)
{
	int retcode = 456;
	const char *err = "JSON object does not contain 'message.request' key";
	if (!json_reader_read_member(reader, "request"))
		goto err;
	const char *req = json_reader_get_string_value(reader);
	if (!req)
		goto err;
	str req_str;
	str_init(&req_str, (char *) req);

	switch (__csh_lookup(&req_str)) {
		case CSH_LOOKUP("create"):;
			// create new videoroom
			struct janus_room *room = g_slice_alloc0(sizeof(*room));

			if (json_reader_read_member(reader, "publishers"))
				room->publishers = json_reader_get_int_value(reader);
			json_reader_end_member(reader);
			if (room->publishers <= 0)
				room->publishers = 3;
			room->session = obj_get(session);

			uint64_t room_id;
			mutex_lock(&janus_lock);
			while (1) {
				room_id = room->id = random();
				if (g_hash_table_lookup(janus_rooms, &room->id))
					continue;
				g_hash_table_insert(janus_rooms, &room->id, room);
				break;
			}
			mutex_unlock(&janus_lock);

			ilog(LOG_INFO, "Created new videoroom with ID %" PRIu64, room_id);

			json_builder_set_member_name(builder, "videoroom");
			json_builder_add_string_value(builder, "created");
			json_builder_set_member_name(builder, "room");
			json_builder_add_int_value(builder, room_id);
			json_builder_set_member_name(builder, "permanent");
			json_builder_add_boolean_value(builder, 0);

			break;
		case CSH_LOOKUP("join"):
			// XXX
			break;

		default:
			retcode = 423;
			err = "Unknown videoroom request";
			goto err;
	}

	err = NULL;

err:
	if (err)
		*retcodep = retcode;
	return err;
}


const char *websocket_janus_process(struct websocket_message *wm) {
	JsonParser *parser = NULL;
	JsonReader *reader = NULL;
	const char *err = NULL;
	int retcode = 200;
	const char *transaction = NULL;
	const char *success = "success";
	uint64_t session_id = 0;
	uint64_t handle_id = 0;
	struct janus_session *session = NULL;

	ilog(LOG_DEBUG, "Processing Janus message: '%.*s'", (int) wm->body->len, wm->body->str);

	// prepare response
	JsonBuilder *builder = json_builder_new();
	json_builder_begin_object(builder); // {

	// start parsing message
	parser = json_parser_new();

	retcode = 454;
	err = "Failed to parse JSON";
	if (!json_parser_load_from_data(parser, wm->body->str, wm->body->len, NULL))
		goto err;
	reader = json_reader_new(json_parser_get_root(parser));
	if (!reader)
		goto err;

	retcode = 455;
	err = "JSON string is not an object";
	if (!json_reader_is_object(reader))
		goto err;

	retcode = 456;
	err = "JSON object does not contain 'janus' key";
	if (!json_reader_read_member(reader, "janus"))
		goto err;
	const char *janus_cmd = json_reader_get_string_value(reader);
	err = "'janus' key does not contain a string";
	if (!janus_cmd)
		goto err;
	json_reader_end_member(reader);

	retcode = 456;
	err = "JSON object does not contain 'transaction' key";
	if (!json_reader_read_member(reader, "transaction"))
		goto err;
	transaction = json_reader_get_string_value(reader);
	err = "'transaction' key does not contain a string";
	if (!janus_cmd)
		goto err;
	json_reader_end_member(reader);

	int authorised = 0;

	if (json_reader_read_member(reader, "admin_secret")) {
		const char *admin_secret = json_reader_get_string_value(reader);
		if (janus_cmd && rtpe_config.janus_secret && !strcmp(admin_secret, rtpe_config.janus_secret))
				authorised = 1;
	}
	json_reader_end_member(reader);

	if (json_reader_read_member(reader, "session_id"))
		session_id = json_reader_get_int_value(reader);
	json_reader_end_member(reader);

	if (session_id)
		session = janus_get_session(session_id);

	if (json_reader_read_member(reader, "handle_id"))
		handle_id = json_reader_get_int_value(reader);
	json_reader_end_member(reader);

	ilog(LOG_DEBUG, "Processing '%s' type Janus message", janus_cmd);

	str janus_cmd_str;
	str_init(&janus_cmd_str, (char *) janus_cmd);

	switch (__csh_lookup(&janus_cmd_str)) {
		case CSH_LOOKUP("add_token"):
			if (!authorised)
				goto unauthorised;

			const char *token = NULL;
			if (json_reader_read_member(reader, "token"))
				token = json_reader_get_string_value(reader);
			json_reader_end_member(reader);

			retcode = 456;
			err = "JSON object does not contain 'token' key";
			if (!token)
				goto err;

			time_t *now = g_malloc(sizeof(*now));
			*now = rtpe_now.tv_sec;
			mutex_lock(&janus_lock);
			g_hash_table_replace(janus_tokens, g_strdup(token), now);
			mutex_unlock(&janus_lock);

			json_builder_set_member_name(builder, "data");
			json_builder_begin_object(builder); // {
			json_builder_set_member_name(builder, "plugins");
			json_builder_begin_array(builder); // [
			json_builder_add_string_value(builder, "janus.plugin.videoroom");
			json_builder_end_array(builder); // ]
			json_builder_end_object(builder); // }
			break;
		case CSH_LOOKUP("ping"):
			success = "pong";
			break;
		case CSH_LOOKUP("keepalive"):
			if (!session)
				goto no_session;
			success = "ack";
			break;
		case CSH_LOOKUP("info"):
			success = "server_info";
			json_builder_set_member_name(builder, "name");
			json_builder_add_string_value(builder, "rtpengine Janus interface");
			json_builder_set_member_name(builder, "version_string");
			json_builder_add_string_value(builder, RTPENGINE_VERSION);
			json_builder_set_member_name(builder, "plugins");
			json_builder_begin_object(builder); // {
			json_builder_set_member_name(builder, "janus.plugin.videoroom");
			json_builder_begin_object(builder); // {
			json_builder_set_member_name(builder, "name");
			json_builder_add_string_value(builder, "rtpengine Janus videoroom");
			json_builder_end_object(builder); // }
			json_builder_end_object(builder); // }
			break;
		case CSH_LOOKUP("create"): // create new session
			if (json_reader_read_member(reader, "id"))
				session_id = json_reader_get_int_value(reader);
			else
				session_id = 0;
			json_reader_end_member(reader);

			if (session)
				obj_put(session);

			session = obj_alloc0("janus_session", sizeof(*session), __janus_session_free);
			mutex_init(&session->lock);
			session->last_act = rtpe_now.tv_sec;
			session->websockets = g_hash_table_new(g_direct_hash, g_direct_equal);
			session->handles = g_hash_table_new(g_int64_hash, g_int64_equal);

			g_hash_table_insert(session->websockets, wm->wc, wm->wc);

			do {
				while (!session_id)
					session_id = random();

				mutex_lock(&janus_lock);
				if (g_hash_table_lookup(janus_sessions, &session_id))
					session_id = 0; // pick a random one
				else {
					session->id = session_id;
					g_hash_table_insert(janus_sessions, &session->id, obj_get(session));
				}
				mutex_unlock(&janus_lock);
			}
			while (!session_id);

			ilog(LOG_INFO, "Created new Janus session with ID %" PRIu64, session_id);

			websocket_conn_add_session(wm->wc, obj_get(session));

			json_builder_set_member_name(builder, "data");
			json_builder_begin_object(builder); // {
			json_builder_set_member_name(builder, "id");
			json_builder_add_int_value(builder, session_id);
			json_builder_end_object(builder); // }

			session_id = 0; // don't add it to the reply

			break;
		case CSH_LOOKUP("attach"): // attach to a plugin, obtains handle
			if (!session)
				goto no_session;
			// verify the plugin
			err = "No plugin given";
			retcode = 456;
			if (!json_reader_read_member(reader, "plugin"))
				goto err;
			const char *plugin = json_reader_get_string_value(reader);
			if (!plugin)
				goto err;
			retcode = 460;
			err = "Unsupported plugin";
			if (strcmp(plugin, "janus.plugin.videoroom"))
				goto err;
			json_reader_end_member(reader);

			struct janus_handle *handle = g_slice_alloc0(sizeof(*handle));
			mutex_lock(&janus_lock);
			while (1) {
				handle_id = handle->id = random();
				if (g_hash_table_lookup(janus_handles, &handle->id))
					continue;
				g_hash_table_insert(janus_handles, &handle->id, handle);
				break;
			}
			mutex_unlock(&janus_lock);

			mutex_lock(&session->lock);
			assert(g_hash_table_lookup(session->handles, &handle_id) == NULL);
			g_hash_table_insert(session->handles, &handle->id, handle);
			mutex_unlock(&session->lock);
			// handle is now owned by session

			json_builder_set_member_name(builder, "data");
			json_builder_begin_object(builder); // {
			json_builder_set_member_name(builder, "id");
			json_builder_add_int_value(builder, handle_id);
			json_builder_end_object(builder); // }

			handle_id = 0; // don't respond with "sender"

			break;
		case CSH_LOOKUP("message"):
			// we only pretend to support one plugin so ignore the handle
			// and just go straight to the message
			if (!session)
				goto no_session;
			retcode = 457;
			err = "No plugin handle given";
			if (!handle_id)
				goto err;
			retcode = 456;
			err = "JSON object does not contain 'body' key";
			if (!json_reader_read_member(reader, "body"))
				goto err;

			json_builder_set_member_name(builder, "plugindata");
			json_builder_begin_object(builder); // {
			json_builder_set_member_name(builder, "plugin");
			json_builder_add_string_value(builder, "janus.plugin.videoroom");
			json_builder_set_member_name(builder, "data");
			json_builder_begin_object(builder); // {

			err = websocket_janus_videoroom(wm, session, builder, reader, &retcode);

			json_builder_end_object(builder); // }
			json_builder_end_object(builder); // }

			if (err)
				goto err;

			break;
		default:
			retcode = 457;
			err = "Unhandled request method";
			goto err;
	}

	// done
	err = NULL;
	goto err;

unauthorised:
	retcode = 403;
	err = "Janus 'admin_secret' key not provided or incorrect";
	goto err;

no_session:
	retcode = 458;
	err = "Session ID not found";
	goto err;

err:;
	json_builder_set_member_name(builder, "janus");
	if (err) {
		json_builder_add_string_value(builder, "error");

		json_builder_set_member_name(builder, "error");
		json_builder_begin_object(builder); // {
		json_builder_set_member_name(builder, "code");
		json_builder_add_int_value(builder, retcode);
		json_builder_set_member_name(builder, "reason");
		json_builder_add_string_value(builder, err);
		json_builder_end_object(builder); // }
	}
	else
		json_builder_add_string_value(builder, success);

	if (transaction) {
		json_builder_set_member_name(builder, "transaction");
		json_builder_add_string_value(builder, transaction);
	}
	if (session_id) {
		json_builder_set_member_name(builder, "session_id");
		json_builder_add_int_value(builder, session_id);
	}
	if (handle_id) {
		json_builder_set_member_name(builder, "sender");
		json_builder_add_int_value(builder, handle_id);
	}
	json_builder_end_object(builder); // }

	JsonGenerator *gen = json_generator_new();
	JsonNode *root = json_builder_get_root(builder);
	json_generator_set_root(gen, root);
	char *result = json_generator_to_data(gen, NULL);

	json_node_free(root);
	g_object_unref(gen);
	g_object_unref(builder);
	if (reader)
		g_object_unref(reader);
	if (parser)
		g_object_unref(parser);
	if (session)
		obj_put(session);


	if (wm->method == M_WEBSOCKET)
		websocket_write_text(wm->wc, result);
	else {
		if (websocket_http_response(wm->wc, err ? retcode : 200, "application/json", strlen(result)))
			return "Failed to write Janus response HTTP headers";
		if (websocket_write_http(wm->wc, result))
			return "Failed to write Janus JSON response";
	}

	g_free(result);

	return err;
}


void janus_init(void) {
	mutex_init(&janus_lock);
	janus_tokens = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	janus_sessions = g_hash_table_new(g_int64_hash, g_int64_equal);
	janus_handles = g_hash_table_new(g_int64_hash, g_int64_equal);
	janus_rooms = g_hash_table_new(g_int64_hash, g_int64_equal);
	// XXX timer thread to clean up orphaned sessions
}
void janus_free(void) {
	mutex_destroy(&janus_lock);
	g_hash_table_destroy(janus_tokens);
	g_hash_table_destroy(janus_sessions);
	g_hash_table_destroy(janus_handles);
	g_hash_table_destroy(janus_rooms);
}
