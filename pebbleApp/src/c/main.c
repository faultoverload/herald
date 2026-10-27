// Herald on a Pebble: talk to Hermes through Herald on the phone.
//
// This is the first, bare watchapp: enough to prove the link (docs/pebble.md, Phase 1). Select dictates an ask,
// a long Select sends a fixed one, Down starts a new chat, Up asks the phone how it stands, a long Up stops the
// turn, and Back always leaves (the turn goes on on the phone). The reply streams in below. The messages are the ones in shared/wearlink's PebbleLink.kt; their keys are in package.json.

#include <pebble.h>

// What a message is (MESSAGE_KEY_TYPE). Below 64 the watch sends it, from 64 the phone does.
enum {
  TYPE_ASK = 1,
  TYPE_STOP = 2,
  TYPE_CHATS_REQ = 3,
  TYPE_STATUS_REQ = 4,
  TYPE_RESYNC = 7,
  TYPE_REPLY = 64,
  TYPE_CHAT_ITEM = 65,
  TYPE_STATUS = 66,
};

// ReplyPhase, in its order.
enum { PHASE_TRANSCRIBING, PHASE_SENDING, PHASE_WORKING, PHASE_WAITING_ON_PHONE, PHASE_DONE, PHASE_FAILED };

#define FLAG_SIGNED_IN 1
#define FLAG_CONNECTED 2
#define FLAG_FAILED 4

#define INBOX_SIZE 2048
#define OUTBOX_SIZE 512
// PebbleLink.MAX_REPLY_BYTES: the phone never sends more.
#define MAX_REPLY 6000
#define TEST_ASK "Say hello in one short sentence."

#define PERSIST_CHAT_ID 1

static Window *s_window;
static TextLayer *s_status_layer;
static TextLayer *s_activity_layer;
static ScrollLayer *s_scroll_layer;
static TextLayer *s_reply_layer;
static DictationSession *s_dictation;

static char s_ask_id[24];
static char s_chat_id[64];
static char s_status[64] = "Herald";
static char s_activity[128] = "Select: speak to Hermes";
static char s_reply[MAX_REPLY + 1];
static int s_reply_len;
static int s_phase = -1;
static int s_ask_count;

static bool prv_running(void) {
  return s_phase >= PHASE_TRANSCRIBING && s_phase <= PHASE_WAITING_ON_PHONE;
}

// A number the phone sent, whatever size it was written in.
static int32_t prv_int(const Tuple *t) {
  if (!t) return 0;
  switch (t->length) {
    case 1: return t->type == TUPLE_INT ? t->value->int8 : t->value->uint8;
    case 2: return t->type == TUPLE_INT ? t->value->int16 : t->value->uint16;
    default: return t->type == TUPLE_INT ? t->value->int32 : (int32_t)t->value->uint32;
  }
}

static void prv_copy(char *to, size_t size, const Tuple *t) {
  if (t && t->type == TUPLE_CSTRING) strncpy(to, t->value->cstring, size - 1); else to[0] = '\0';
  to[size - 1] = '\0';
}

static void prv_redraw(void) {
  text_layer_set_text(s_status_layer, s_status);
  text_layer_set_text(s_activity_layer, s_activity);
  text_layer_set_text(s_reply_layer, s_reply);
  GRect bounds = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer));
  GSize size = text_layer_get_content_size(s_reply_layer);
  size.h += 8;
  if (size.h < bounds.size.h) size.h = bounds.size.h;
  text_layer_set_size(s_reply_layer, GSize(bounds.size.w, size.h));
  scroll_layer_set_content_size(s_scroll_layer, GSize(bounds.size.w, size.h));
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

static void prv_send_simple(int type) {
  DictionaryIterator *out;
  if (!prv_begin(&out, type)) return;
  app_message_outbox_send();
}

static void prv_ask(const char *text) {
  DictionaryIterator *out;
  if (!prv_begin(&out, TYPE_ASK)) return;
  snprintf(s_ask_id, sizeof(s_ask_id), "p%lx%d", (unsigned long)time(NULL), ++s_ask_count);
  dict_write_cstring(out, MESSAGE_KEY_ASK_ID, s_ask_id);
  dict_write_cstring(out, MESSAGE_KEY_TEXT, text);
  if (s_chat_id[0]) dict_write_cstring(out, MESSAGE_KEY_CHAT_ID, s_chat_id);
  app_message_outbox_send();
  s_reply[0] = '\0';
  s_reply_len = 0;
  s_phase = PHASE_SENDING;
  snprintf(s_activity, sizeof(s_activity), "Sending...");
  prv_redraw();
}

static void prv_on_reply(DictionaryIterator *in) {
  char ask_id[sizeof(s_ask_id)];
  prv_copy(ask_id, sizeof(ask_id), dict_find(in, MESSAGE_KEY_ASK_ID));
  if (strcmp(ask_id, s_ask_id) != 0) {
    // Another ask's turn (one from before this app opened, sent again): it takes over.
    strncpy(s_ask_id, ask_id, sizeof(s_ask_id) - 1);
    s_reply[0] = '\0';
    s_reply_len = 0;
  }
  int offset = prv_int(dict_find(in, MESSAGE_KEY_TEXT_OFFSET));
  if (offset > s_reply_len) {
    // Told to add past what this has: it lost the start (restarted). The phone sends it all again.
    DictionaryIterator *out;
    if (prv_begin(&out, TYPE_RESYNC)) {
      dict_write_cstring(out, MESSAGE_KEY_ASK_ID, s_ask_id);
      app_message_outbox_send();
    }
    return;
  }
  // Placed at its offset: the same piece sent twice changes nothing, and 0 replaces the text.
  const Tuple *text = dict_find(in, MESSAGE_KEY_TEXT);
  int len = (text && text->type == TUPLE_CSTRING) ? (int)strlen(text->value->cstring) : 0;
  if (offset + len > MAX_REPLY) len = MAX_REPLY - offset;
  if (len > 0) memcpy(s_reply + offset, text->value->cstring, len);
  s_reply_len = offset + len;
  s_reply[s_reply_len] = '\0';

  int was = s_phase;
  s_phase = prv_int(dict_find(in, MESSAGE_KEY_PHASE));
  const Tuple *chat = dict_find(in, MESSAGE_KEY_CHAT_ID);
  if (chat && chat->type == TUPLE_CSTRING && chat->length > 1) {
    prv_copy(s_chat_id, sizeof(s_chat_id), chat);
    persist_write_string(PERSIST_CHAT_ID, s_chat_id);
  }
  char error[128];
  prv_copy(error, sizeof(error), dict_find(in, MESSAGE_KEY_ERROR));
  prv_copy(s_activity, sizeof(s_activity), dict_find(in, MESSAGE_KEY_ACTIVITY));
  if (s_phase == PHASE_FAILED) {
    snprintf(s_activity, sizeof(s_activity), "%s", error[0] ? error : "It didn't go.");
  } else if (!s_activity[0]) {
    static const char *const names[] = {"Writing down...", "Sending...", "Working...", "Answer on your phone", "Done", ""};
    if (s_phase >= 0 && s_phase <= PHASE_FAILED) snprintf(s_activity, sizeof(s_activity), "%s", names[s_phase]);
  }
  char title[64];
  prv_copy(title, sizeof(title), dict_find(in, MESSAGE_KEY_TITLE));
  if (title[0]) snprintf(s_status, sizeof(s_status), "%s", title);
  if (s_phase != was && (s_phase == PHASE_DONE || s_phase == PHASE_FAILED || s_phase == PHASE_WAITING_ON_PHONE)) vibes_short_pulse();
  prv_redraw();
}

static void prv_on_status(DictionaryIterator *in) {
  int flags = prv_int(dict_find(in, MESSAGE_KEY_FLAGS));
  const char *phone = !(flags & FLAG_SIGNED_IN) ? "sign in on phone"
      : (flags & FLAG_FAILED) ? "can't connect"
      : (flags & FLAG_CONNECTED) ? "ready" : "connecting";
  snprintf(s_status, sizeof(s_status), "Herald: %s", phone);
  prv_redraw();
}

static void prv_inbox_received(DictionaryIterator *in, void *context) {
  switch (prv_int(dict_find(in, MESSAGE_KEY_TYPE))) {
    case TYPE_REPLY: prv_on_reply(in); break;
    case TYPE_STATUS: prv_on_status(in); break;
    case TYPE_CHAT_ITEM: {
      char title[64];
      prv_copy(title, sizeof(title), dict_find(in, MESSAGE_KEY_TITLE));
      APP_LOG(APP_LOG_LEVEL_INFO, "Chat %d/%d: %s", (int)prv_int(dict_find(in, MESSAGE_KEY_INDEX)) + 1,
              (int)prv_int(dict_find(in, MESSAGE_KEY_COUNT)), title);
      break;
    }
    default: break;
  }
}

static void prv_inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "Inbox dropped a message: %d", (int)reason);
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "Outbox failed: %d", (int)reason);
}

static void prv_dictated(DictationSession *session, DictationSessionStatus status, char *transcription, void *context) {
  if (status == DictationSessionStatusSuccess) prv_ask(transcription);
}

static void prv_select(ClickRecognizerRef recognizer, void *context) {
  if (s_dictation) dictation_session_start(s_dictation);
}

static void prv_select_long(ClickRecognizerRef recognizer, void *context) {
  prv_ask(TEST_ASK);
}

static void prv_up(ClickRecognizerRef recognizer, void *context) {
  prv_send_simple(TYPE_STATUS_REQ);
}

static void prv_down(ClickRecognizerRef recognizer, void *context) {
  s_chat_id[0] = '\0';
  persist_delete(PERSIST_CHAT_ID);
  snprintf(s_status, sizeof(s_status), "Herald: new chat");
  prv_redraw();
}

static void prv_down_long(ClickRecognizerRef recognizer, void *context) {
  prv_send_simple(TYPE_CHATS_REQ);
}

static void prv_stop(ClickRecognizerRef recognizer, void *context) {
  if (!prv_running()) return;
  DictionaryIterator *out;
  if (prv_begin(&out, TYPE_STOP)) {
    dict_write_cstring(out, MESSAGE_KEY_ASK_ID, s_ask_id);
    app_message_outbox_send();
  }
}

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, prv_select_long, NULL);
  window_single_click_subscribe(BUTTON_ID_UP, prv_up);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down);
  window_long_click_subscribe(BUTTON_ID_DOWN, 0, prv_down_long, NULL);
  window_long_click_subscribe(BUTTON_ID_UP, 0, prv_stop, NULL);
}

// Up and Down scroll the reply only with Select held; plain Up and Down are the commands above.
static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_status_layer = text_layer_create(GRect(4, 0, bounds.size.w - 8, 22));
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  layer_add_child(root, text_layer_get_layer(s_status_layer));

  s_activity_layer = text_layer_create(GRect(4, 22, bounds.size.w - 8, 22));
  text_layer_set_font(s_activity_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_color(s_activity_layer, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack));
  layer_add_child(root, text_layer_get_layer(s_activity_layer));

  GRect body = GRect(0, 46, bounds.size.w, bounds.size.h - 46);
  s_scroll_layer = scroll_layer_create(body);
  s_reply_layer = text_layer_create(GRect(4, 0, body.size.w - 8, 2000));
  text_layer_set_font(s_reply_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24));
  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_reply_layer));
  layer_add_child(root, scroll_layer_get_layer(s_scroll_layer));
  prv_redraw();
}

static void prv_window_unload(Window *window) {
  text_layer_destroy(s_reply_layer);
  scroll_layer_destroy(s_scroll_layer);
  text_layer_destroy(s_activity_layer);
  text_layer_destroy(s_status_layer);
}

static void prv_ask_status(void *data) {
  prv_send_simple(TYPE_STATUS_REQ);
}

static void prv_init(void) {
  if (persist_exists(PERSIST_CHAT_ID)) persist_read_string(PERSIST_CHAT_ID, s_chat_id, sizeof(s_chat_id));
  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_inbox_dropped(prv_inbox_dropped);
  app_message_register_outbox_failed(prv_outbox_failed);
  app_message_open(INBOX_SIZE, OUTBOX_SIZE);
  s_dictation = dictation_session_create(512, prv_dictated, NULL);

  s_window = window_create();
  window_set_click_config_provider(s_window, prv_click_config);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
  // A moment after opening, once the link is up.
  app_timer_register(500, prv_ask_status, NULL);
}

static void prv_deinit(void) {
  if (s_dictation) dictation_session_destroy(s_dictation);
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
