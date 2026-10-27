#pragma once

// What Herald on the phone and this watchapp say to each other: the message types and keys of
// shared/wearlink's PebbleLink.kt (keys from package.json's messageKeys), read into plain structs.

#include <pebble.h>

// ReplyPhase, in its order.
typedef enum {
  PHASE_TRANSCRIBING,
  PHASE_SENDING,
  PHASE_WORKING,
  PHASE_WAITING_ON_PHONE,
  PHASE_DONE,
  PHASE_FAILED,
} Phase;

#define LINK_FLAG_SIGNED_IN 1
#define LINK_FLAG_CONNECTED 2
#define LINK_FLAG_FAILED 4

// One piece of a turn: its state, and text to place at byte offset of the reply.
typedef struct {
  const char *ask_id;
  int seq;
  Phase phase;
  const char *chat_id;   // NULL until the phone names it
  const char *title;     // NULL when unchanged
  const char *activity;  // NULL for none
  const char *error;     // NULL for none
  const char *text;
  int text_offset;
  int text_total;
} LinkReply;

typedef struct {
  int flags;
  bool has_accent;
  const char *accent_rev;
  uint32_t fill, text, on_fill, dim;  // 0xRRGGBB
} LinkStatus;

typedef struct {
  void (*on_reply)(const LinkReply *reply);
  void (*on_status)(const LinkStatus *status);
  // A message to the phone that didn't go, by its type.
  void (*on_send_failed)(int type);
} LinkHandlers;

enum {
  LINK_ASK = 1,
  LINK_STOP = 2,
  LINK_CHATS_REQ = 3,
  LINK_STATUS_REQ = 4,
  LINK_RESYNC = 7,
};

// Opens AppMessage with the inbox the phone's messages are sized for.
void link_open(LinkHandlers handlers);

// Each is false when the outbox was busy and nothing went.
bool link_ask(const char *ask_id, const char *text, const char *chat_id);
bool link_stop(const char *ask_id);
bool link_status_request(const char *accent_rev);
bool link_resync(const char *ask_id);

// Whether the Pebble phone app is connected at all.
bool link_phone_connected(void);
