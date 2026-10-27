#include "link.h"

// PebbleLink.WATCH_INBOX_BYTES: every message the phone sends fits.
#define INBOX_SIZE 2048
#define OUTBOX_SIZE 512

enum { TYPE_REPLY = 64, TYPE_CHAT_ITEM = 65, TYPE_STATUS = 66 };

static LinkHandlers s_handlers;

// A number the phone sent, whatever size it was written in.
static int32_t prv_int(const Tuple *t) {
  if (!t) return 0;
  switch (t->length) {
    case 1: return t->type == TUPLE_INT ? t->value->int8 : t->value->uint8;
    case 2: return t->type == TUPLE_INT ? t->value->int16 : t->value->uint16;
    default: return t->type == TUPLE_INT ? t->value->int32 : (int32_t)t->value->uint32;
  }
}

// Text the phone sent, or NULL when it sent none (or something else under the key).
static const char *prv_text(DictionaryIterator *in, uint32_t key) {
  const Tuple *t = dict_find(in, key);
  return (t && t->type == TUPLE_CSTRING) ? t->value->cstring : NULL;
}

static void prv_on_reply(DictionaryIterator *in) {
  const char *text = prv_text(in, MESSAGE_KEY_TEXT);
  LinkReply reply = {
    .ask_id = prv_text(in, MESSAGE_KEY_ASK_ID),
    .seq = prv_int(dict_find(in, MESSAGE_KEY_SEQ)),
    .phase = (Phase)prv_int(dict_find(in, MESSAGE_KEY_PHASE)),
    .chat_id = prv_text(in, MESSAGE_KEY_CHAT_ID),
    .title = prv_text(in, MESSAGE_KEY_TITLE),
    .activity = prv_text(in, MESSAGE_KEY_ACTIVITY),
    .error = prv_text(in, MESSAGE_KEY_ERROR),
    .text = text ? text : "",
    .text_offset = prv_int(dict_find(in, MESSAGE_KEY_TEXT_OFFSET)),
    .text_total = prv_int(dict_find(in, MESSAGE_KEY_TEXT_TOTAL)),
  };
  if (!reply.ask_id || reply.phase > PHASE_FAILED) return;
  if (s_handlers.on_reply) s_handlers.on_reply(&reply);
}

static void prv_on_status(DictionaryIterator *in) {
  LinkStatus status = { .flags = prv_int(dict_find(in, MESSAGE_KEY_FLAGS)) };
  status.accent_rev = prv_text(in, MESSAGE_KEY_ACCENT_REV);
  status.has_accent = status.accent_rev != NULL;
  status.fill = prv_int(dict_find(in, MESSAGE_KEY_ACCENT_FILL));
  status.text = prv_int(dict_find(in, MESSAGE_KEY_ACCENT_TEXT));
  status.on_fill = prv_int(dict_find(in, MESSAGE_KEY_ACCENT_ON_FILL));
  status.dim = prv_int(dict_find(in, MESSAGE_KEY_ACCENT_DIM));
  if (s_handlers.on_status) s_handlers.on_status(&status);
}

static void prv_inbox_received(DictionaryIterator *in, void *context) {
  switch (prv_int(dict_find(in, MESSAGE_KEY_TYPE))) {
    case TYPE_REPLY: prv_on_reply(in); break;
    case TYPE_STATUS: prv_on_status(in); break;
    // The chats come with the chats screen (Phase 3).
    case TYPE_CHAT_ITEM: break;
    default: break;
  }
}

static void prv_inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "Inbox dropped a message: %d", (int)reason);
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  const Tuple *type = dict_find(iter, MESSAGE_KEY_TYPE);
  APP_LOG(APP_LOG_LEVEL_WARNING, "Outbox failed: %d", (int)reason);
  if (s_handlers.on_send_failed) s_handlers.on_send_failed(prv_int(type));
}

void link_open(LinkHandlers handlers) {
  s_handlers = handlers;
  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_inbox_dropped(prv_inbox_dropped);
  app_message_register_outbox_failed(prv_outbox_failed);
  app_message_open(INBOX_SIZE, OUTBOX_SIZE);
}

static bool prv_begin(DictionaryIterator **out, int type) {
  AppMessageResult result = app_message_outbox_begin(out);
  if (result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Outbox busy (%d), type %d not sent", (int)result, type);
    return false;
  }
  dict_write_int32(*out, MESSAGE_KEY_TYPE, type);
  return true;
}

static bool prv_send(void) {
  return app_message_outbox_send() == APP_MSG_OK;
}

bool link_ask(const char *ask_id, const char *text, const char *chat_id) {
  DictionaryIterator *out;
  if (!prv_begin(&out, LINK_ASK)) return false;
  dict_write_cstring(out, MESSAGE_KEY_ASK_ID, ask_id);
  dict_write_cstring(out, MESSAGE_KEY_TEXT, text);
  if (chat_id && chat_id[0]) dict_write_cstring(out, MESSAGE_KEY_CHAT_ID, chat_id);
  return prv_send();
}

bool link_stop(const char *ask_id) {
  DictionaryIterator *out;
  if (!prv_begin(&out, LINK_STOP)) return false;
  dict_write_cstring(out, MESSAGE_KEY_ASK_ID, ask_id);
  return prv_send();
}

bool link_status_request(const char *accent_rev) {
  DictionaryIterator *out;
  if (!prv_begin(&out, LINK_STATUS_REQ)) return false;
  if (accent_rev && accent_rev[0]) dict_write_cstring(out, MESSAGE_KEY_ACCENT_REV, accent_rev);
  return prv_send();
}

bool link_resync(const char *ask_id) {
  DictionaryIterator *out;
  if (!prv_begin(&out, LINK_RESYNC)) return false;
  dict_write_cstring(out, MESSAGE_KEY_ASK_ID, ask_id);
  return prv_send();
}

bool link_phone_connected(void) {
  return connection_service_peek_pebble_app_connection();
}
