#include "talk.h"
#include "chats.h"
#include "request.h"
#include "dictate.h"

// PebbleLink.MAX_REPLY_BYTES: the phone never sends more.
#define MAX_REPLY 6000
// The phone answers a status request at once; none within this, Herald isn't there.
#define ANSWER_WITHIN_MS 5000
// While open, the phone's state is asked again this often (as the Wear watch does).
#define STATUS_EVERY_MS 30000

#define HEADER_H 30
#define ACTIVITY_H 24
#define PAD 6

// Persisted.
enum { KEY_CHAT_ID = 1, KEY_CHAT_TITLE = 2, KEY_ACCENT_REV = 3, KEY_ACCENT_FILL = 4, KEY_ACCENT_ON_FILL = 5 };

typedef enum {
  PHONE_UNKNOWN,
  // No phone is connected to the watch.
  PHONE_NOT_CONNECTED,
  // A phone is, but Herald there didn't answer.
  PHONE_NO_HERALD,
  PHONE_SIGNED_OUT,
  // Signed in, and Herald gave up reaching the gateway.
  PHONE_UNREACHABLE,
  PHONE_RECONNECTING,
  PHONE_READY,
} PhoneState;

static Window *s_window;
static Layer *s_header_layer;
static TextLayer *s_activity_layer;
static ScrollLayer *s_scroll_layer;
static TextLayer *s_said_layer;
static TextLayer *s_reply_layer;
static AppTimer *s_answer_timer;
static AppTimer *s_status_timer;

static PhoneState s_phone = PHONE_UNKNOWN;
static char s_chat_id[64];
static char s_chat_title[64];
static char s_accent_rev[48];
static GColor s_fill;
static GColor s_on_fill;

// The turn on screen: the ask it answers, what was said (when said here), the reply so far.
static bool s_has_turn;
static char s_ask_id[24];
static char s_said[300];
static char s_reply[MAX_REPLY + 1];
static int s_reply_len;
static Phase s_phase;
// A chat's last exchange was asked for and hasn't come.
static bool s_loading_chat;
static char s_activity[128];
static char s_error[128];
static int s_ask_count;

static bool prv_running(void) {
  return s_has_turn && s_phase <= PHASE_WAITING_ON_PHONE;
}

// --- What the screen says

static const char *prv_phone_problem_short(void) {
  switch (s_phone) {
    case PHONE_NOT_CONNECTED: return "Phone not connected";
    case PHONE_NO_HERALD: return "No answer from Herald";
    case PHONE_SIGNED_OUT: return "Sign in on your phone";
    case PHONE_UNREACHABLE: return "Gateway out of reach";
    default: return NULL;
  }
}

// The same words as the Wear OS watch.
static const char *prv_phone_problem(void) {
  switch (s_phone) {
    case PHONE_NOT_CONNECTED: return "Your phone isn't connected.";
    case PHONE_NO_HERALD: return "Herald on your phone didn't answer. Open it there, or update it.";
    case PHONE_SIGNED_OUT: return "Sign in to Herald on your phone first.";
    case PHONE_UNREACHABLE: return "Herald can't reach your gateway. Check it on your phone.";
    default: return NULL;
  }
}

static const char *prv_phase_line(void) {
  switch (s_phase) {
    case PHASE_TRANSCRIBING: return "Writing it down...";
    case PHASE_SENDING: return "Sending...";
    case PHASE_WORKING: return "Thinking...";
    case PHASE_WAITING_ON_PHONE: return "Waiting for you on your phone";
    default: return "";
  }
}

// The line under the header: the turn's state, else the phone's, else what Select does.
static const char *prv_activity_line(void) {
  if (s_has_turn && s_phase == PHASE_FAILED) {
    // With no reply, the body says why at length.
    if (!s_reply[0]) return "It didn't go";
    return s_error[0] ? s_error : "Something went wrong.";
  }
  if (prv_running()) return s_activity[0] ? s_activity : prv_phase_line();
  if (s_loading_chat) return "Opening the chat...";
  const char *problem = prv_phone_problem_short();
  if (problem) return problem;
  return s_has_turn ? "Select to reply" : "Select to talk";
}

static GColor prv_dot(void) {
  switch (s_phone) {
    case PHONE_READY: return PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorWhite);
    case PHONE_RECONNECTING: return PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite);
    case PHONE_UNKNOWN: return GColorClear;
    default: return PBL_IF_COLOR_ELSE(GColorRed, GColorWhite);
  }
}

static void prv_header_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, s_fill);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, s_on_fill);
  graphics_draw_text(ctx, s_chat_title[0] ? s_chat_title : "New chat", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(PAD, 2, bounds.size.w - PAD * 2 - 14, HEADER_H - 4), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
  GColor dot = prv_dot();
  if (!gcolor_equal(dot, GColorClear)) {
    graphics_context_set_fill_color(ctx, dot);
    graphics_fill_circle(ctx, GPoint(bounds.size.w - PAD - 4, HEADER_H / 2), 4);
    graphics_context_set_stroke_color(ctx, s_on_fill);
    graphics_draw_circle(ctx, GPoint(bounds.size.w - PAD - 4, HEADER_H / 2), 4);
  }
}

// Sizes the said and reply text to what they hold, the reply below what was said.
static void prv_layout(void) {
  GRect bounds = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer));
  int w = bounds.size.w - PAD * 2;
  int y = 4;
  if (s_said[0]) {
    text_layer_set_size(s_said_layer, GSize(w, 2000));
    int h = text_layer_get_content_size(s_said_layer).h + 4;
    layer_set_frame(text_layer_get_layer(s_said_layer), GRect(PAD, y, w, h));
    y += h + 4;
  } else {
    layer_set_frame(text_layer_get_layer(s_said_layer), GRect(PAD, y, w, 0));
  }
  layer_set_frame(text_layer_get_layer(s_reply_layer), GRect(PAD, y, w, 2000));
  int h = text_layer_get_content_size(s_reply_layer).h + 6;
  layer_set_frame(text_layer_get_layer(s_reply_layer), GRect(PAD, y, w, h));
  scroll_layer_set_content_size(s_scroll_layer, GSize(bounds.size.w, y + h + 12));
}

static void prv_redraw(void) {
  if (!s_window) return;
  layer_mark_dirty(s_header_layer);
  bool failed = s_has_turn && s_phase == PHASE_FAILED;
  text_layer_set_text_color(s_activity_layer, failed ? PBL_IF_COLOR_ELSE(GColorDarkCandyAppleRed, GColorBlack)
                                                     : PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack));
  text_layer_set_text(s_activity_layer, prv_activity_line());
  text_layer_set_text(s_said_layer, s_said);
  if (s_has_turn && !s_reply[0] && s_phase == PHASE_FAILED) {
    // Nothing came back: the whole of why, where there's room for it.
    text_layer_set_text(s_reply_layer, s_error[0] ? s_error : "Something went wrong.");
  } else if (s_has_turn) {
    text_layer_set_text(s_reply_layer, s_reply);
  } else if (s_loading_chat) {
    text_layer_set_text(s_reply_layer, "");
  } else {
    // Nothing asked yet: what's wrong, or how to start.
    const char *problem = prv_phone_problem();
    text_layer_set_text(s_reply_layer, problem ? problem : "Press Select and speak to Hermes.\n\nHold Select for your chats.");
  }
  prv_layout();
}

// --- The phone

static void prv_answer_timeout(void *data) {
  s_answer_timer = NULL;
  if (s_phone != PHONE_NOT_CONNECTED) s_phone = PHONE_NO_HERALD;
  prv_redraw();
}

static void prv_ask_status(void) {
  if (!link_phone_connected()) {
    s_phone = PHONE_NOT_CONNECTED;
    prv_redraw();
    return;
  }
  if (!link_status_request(s_accent_rev)) return;
  if (!s_answer_timer) s_answer_timer = app_timer_register(ANSWER_WITHIN_MS, prv_answer_timeout, NULL);
}

static void prv_status_tick(void *data) {
  s_status_timer = app_timer_register(STATUS_EVERY_MS, prv_status_tick, NULL);
  prv_ask_status();
}

void talk_on_status(const LinkStatus *status) {
  if (s_answer_timer) {
    app_timer_cancel(s_answer_timer);
    s_answer_timer = NULL;
  }
  if (!(status->flags & LINK_FLAG_SIGNED_IN)) s_phone = PHONE_SIGNED_OUT;
  else if (status->flags & LINK_FLAG_FAILED) s_phone = PHONE_UNREACHABLE;
  else if (status->flags & LINK_FLAG_CONNECTED) s_phone = PHONE_READY;
  else s_phone = PHONE_RECONNECTING;
  // Opened in a chat with no turn to show: its last exchange, once the phone's there to send it.
  static bool s_asked_history;
  if (!s_asked_history && !s_has_turn && s_chat_id[0] && s_phone == PHONE_READY) {
    s_asked_history = true;
    s_loading_chat = link_history_request(s_chat_id);
  }
  if (status->has_accent) {
    // The phone's dark accent: its fill for the header, its on-fill for the header's text.
    s_fill = PBL_IF_COLOR_ELSE(GColorFromHEX(status->fill), GColorBlack);
    s_on_fill = PBL_IF_COLOR_ELSE(GColorFromHEX(status->on_fill), GColorWhite);
    strncpy(s_accent_rev, status->accent_rev, sizeof(s_accent_rev) - 1);
    persist_write_string(KEY_ACCENT_REV, s_accent_rev);
    persist_write_int(KEY_ACCENT_FILL, (int32_t)status->fill);
    persist_write_int(KEY_ACCENT_ON_FILL, (int32_t)status->on_fill);
  }
  prv_redraw();
}

static void prv_phone_connection(bool connected) {
  if (connected) {
    prv_ask_status();
  } else {
    s_phone = PHONE_NOT_CONNECTED;
    prv_redraw();
  }
}

// --- The turn

static void prv_copy(char *to, size_t size, const char *from) {
  strncpy(to, from ? from : "", size - 1);
  to[size - 1] = '\0';
}

static void prv_ask(const char *text) {
  snprintf(s_ask_id, sizeof(s_ask_id), "p%lx%d", (unsigned long)time(NULL), ++s_ask_count);
  s_has_turn = true;
  prv_copy(s_said, sizeof(s_said), text);
  s_reply[0] = '\0';
  s_reply_len = 0;
  s_activity[0] = '\0';
  s_error[0] = '\0';
  s_phase = PHASE_SENDING;
  if (!link_ask(s_ask_id, text, s_chat_id)) {
    s_phase = PHASE_FAILED;
    prv_copy(s_error, sizeof(s_error), "Couldn't send that. Try again.");
  }
  scroll_layer_set_content_offset(s_scroll_layer, GPointZero, false);
  prv_redraw();
}

void talk_on_send_failed(int type) {
  if (type == LINK_ASK && s_has_turn && s_phase == PHASE_SENDING) {
    s_phase = PHASE_FAILED;
    prv_copy(s_error, sizeof(s_error), "Couldn't reach Herald on your phone.");
    vibes_double_pulse();
    prv_redraw();
  } else if (type == LINK_STATUS_REQ) {
    // Not even delivered: the answer won't come.
    if (s_answer_timer) app_timer_cancel(s_answer_timer);
    prv_answer_timeout(NULL);
  }
}

void talk_on_reply(const LinkReply *reply) {
  s_loading_chat = false;
  if (!s_has_turn || strcmp(reply->ask_id, s_ask_id) != 0) {
    // Another ask's turn: one from before this app opened, which the phone sends again on opening.
    s_has_turn = true;
    prv_copy(s_ask_id, sizeof(s_ask_id), reply->ask_id);
    s_said[0] = '\0';
    s_reply[0] = '\0';
    s_reply_len = 0;
  }
  if (reply->text_offset > s_reply_len) {
    // Told to add past what this has: it lost the start. The phone sends it all again.
    link_resync(s_ask_id);
    return;
  }
  // Placed at its offset: the same piece sent twice changes nothing, and 0 replaces the text.
  int len = strlen(reply->text);
  if (reply->text_offset + len > MAX_REPLY) len = MAX_REPLY - reply->text_offset;
  memcpy(s_reply + reply->text_offset, reply->text, len);
  s_reply_len = reply->text_offset + len;
  s_reply[s_reply_len] = '\0';

  Phase was = s_phase;
  s_phase = reply->phase;
  prv_copy(s_activity, sizeof(s_activity), reply->activity);
  prv_copy(s_error, sizeof(s_error), reply->error);
  if (reply->said) prv_copy(s_said, sizeof(s_said), reply->said);
  if (reply->chat_id && reply->chat_id[0] && strcmp(reply->chat_id, s_chat_id) != 0) {
    prv_copy(s_chat_id, sizeof(s_chat_id), reply->chat_id);
    persist_write_string(KEY_CHAT_ID, s_chat_id);
  }
  if (reply->title && reply->title[0] && strcmp(reply->title, s_chat_title) != 0) {
    prv_copy(s_chat_title, sizeof(s_chat_title), reply->title);
    persist_write_string(KEY_CHAT_TITLE, s_chat_title);
  }
  // A turn only answers once it has a phone to answer it.
  if (s_phone == PHONE_NO_HERALD || s_phone == PHONE_UNKNOWN) s_phone = PHONE_READY;
  if (s_phase != was) {
    if (s_phase == PHASE_DONE) vibes_short_pulse();
    else if (s_phase == PHASE_FAILED || s_phase == PHASE_WAITING_ON_PHONE) vibes_double_pulse();
  }
  prv_redraw();
}

void talk_on_request(const LinkRequest *request) {
  request_show(request, s_fill, s_on_fill);
}

// Talks in [chat_id] from the next ask on; an empty id is a new chat.
static void prv_use_chat(const char *chat_id, const char *title) {
  prv_copy(s_chat_id, sizeof(s_chat_id), chat_id);
  prv_copy(s_chat_title, sizeof(s_chat_title), title);
  if (s_chat_id[0]) {
    persist_write_string(KEY_CHAT_ID, s_chat_id);
    persist_write_string(KEY_CHAT_TITLE, s_chat_title);
  } else {
    persist_delete(KEY_CHAT_ID);
    persist_delete(KEY_CHAT_TITLE);
  }
  s_has_turn = false;
  s_said[0] = '\0';
  s_ask_id[0] = '\0';
  // An old chat opens on its last exchange.
  s_loading_chat = s_chat_id[0] && link_history_request(s_chat_id);
  scroll_layer_set_content_offset(s_scroll_layer, GPointZero, false);
  prv_redraw();
}

// --- Buttons

static void prv_dictated(const char *text) {
  prv_ask(text);
}

static void prv_select(ClickRecognizerRef recognizer, void *context) {
  if (!link_phone_connected()) {
    s_phone = PHONE_NOT_CONNECTED;
    vibes_short_pulse();
    prv_redraw();
    return;
  }
  dictate(prv_dictated);
}

static void prv_menu_new_chat(ActionMenu *menu, const ActionMenuItem *item, void *context) {
  prv_use_chat("", "");
}

static void prv_menu_chats(ActionMenu *menu, const ActionMenuItem *item, void *context) {
  chats_open(s_chat_id, s_fill, s_on_fill, prv_use_chat);
}

static void prv_menu_stop(ActionMenu *menu, const ActionMenuItem *item, void *context) {
  if (!prv_running() || !link_stop(s_ask_id)) return;
  // The phone stops telling once it stops the turn: what came so far stays as the reply.
  s_phase = PHASE_DONE;
  s_activity[0] = '\0';
  prv_redraw();
}

static void prv_menu_check_phone(ActionMenu *menu, const ActionMenuItem *item, void *context) {
  s_phone = PHONE_UNKNOWN;
  prv_ask_status();
  prv_redraw();
}

static void prv_menu_closed(ActionMenu *menu, const ActionMenuItem *performed, void *context) {
  action_menu_hierarchy_destroy(action_menu_get_root_level(menu), NULL, NULL);
}

static void prv_menu(ClickRecognizerRef recognizer, void *context) {
  ActionMenuLevel *root = action_menu_level_create(4);
  if (prv_running()) action_menu_level_add_action(root, "Stop", prv_menu_stop, NULL);
  action_menu_level_add_action(root, "New chat", prv_menu_new_chat, NULL);
  action_menu_level_add_action(root, "Chats", prv_menu_chats, NULL);
  action_menu_level_add_action(root, "Check phone", prv_menu_check_phone, NULL);
  ActionMenuConfig config = {
    .root_level = root,
    .colors = { .background = s_fill, .foreground = s_on_fill },
    .align = ActionMenuAlignCenter,
    .did_close = prv_menu_closed,
  };
  action_menu_open(&config);
}

// Up and Down stay the scroll layer's: they scroll the reply.
static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, prv_menu, NULL);
}

// --- The window

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  window_set_background_color(window, GColorWhite);

  s_header_layer = layer_create(GRect(0, 0, bounds.size.w, HEADER_H));
  layer_set_update_proc(s_header_layer, prv_header_update);
  layer_add_child(root, s_header_layer);

  s_activity_layer = text_layer_create(GRect(PAD, HEADER_H, bounds.size.w - PAD * 2, ACTIVITY_H));
  text_layer_set_font(s_activity_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_overflow_mode(s_activity_layer, GTextOverflowModeTrailingEllipsis);
  layer_add_child(root, text_layer_get_layer(s_activity_layer));

  int top = HEADER_H + ACTIVITY_H;
  s_scroll_layer = scroll_layer_create(GRect(0, top, bounds.size.w, bounds.size.h - top));
  scroll_layer_set_shadow_hidden(s_scroll_layer, true);
  scroll_layer_set_click_config_onto_window(s_scroll_layer, window);
  scroll_layer_set_callbacks(s_scroll_layer, (ScrollLayerCallbacks) { .click_config_provider = prv_click_config });

  s_said_layer = text_layer_create(GRect(PAD, 4, bounds.size.w - PAD * 2, 0));
  text_layer_set_font(s_said_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_color(s_said_layer, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack));
  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_said_layer));

  s_reply_layer = text_layer_create(GRect(PAD, 4, bounds.size.w - PAD * 2, 0));
  text_layer_set_font(s_reply_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24));
  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_reply_layer));
  layer_add_child(root, scroll_layer_get_layer(s_scroll_layer));

  prv_redraw();
}

static void prv_window_unload(Window *window) {
  text_layer_destroy(s_reply_layer);
  text_layer_destroy(s_said_layer);
  scroll_layer_destroy(s_scroll_layer);
  text_layer_destroy(s_activity_layer);
  layer_destroy(s_header_layer);
}

static void prv_first_status(void *data) {
  prv_status_tick(NULL);
}

void talk_init(void) {
  // Herald blue on white until the phone tells its accent.
  s_fill = PBL_IF_COLOR_ELSE(GColorFromHEX(0x2F6BF5), GColorBlack);
  s_on_fill = GColorWhite;
  if (persist_exists(KEY_CHAT_ID)) persist_read_string(KEY_CHAT_ID, s_chat_id, sizeof(s_chat_id));
  if (persist_exists(KEY_CHAT_TITLE)) persist_read_string(KEY_CHAT_TITLE, s_chat_title, sizeof(s_chat_title));
  if (persist_exists(KEY_ACCENT_REV)) {
    persist_read_string(KEY_ACCENT_REV, s_accent_rev, sizeof(s_accent_rev));
    s_fill = PBL_IF_COLOR_ELSE(GColorFromHEX(persist_read_int(KEY_ACCENT_FILL)), GColorBlack);
    s_on_fill = PBL_IF_COLOR_ELSE(GColorFromHEX(persist_read_int(KEY_ACCENT_ON_FILL)), GColorWhite);
  }
  connection_service_subscribe((ConnectionHandlers) { .pebble_app_connection_handler = prv_phone_connection });

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
  // A moment after opening, once the link is up.
  app_timer_register(300, prv_first_status, NULL);
}

void talk_deinit(void) {
  connection_service_unsubscribe();
  dictate_deinit();
  window_destroy(s_window);
  s_window = NULL;
}
