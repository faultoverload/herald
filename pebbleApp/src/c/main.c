// Herald on a Pebble: talk to Hermes through Herald on the phone, which holds the gateway connection and the
// sign-in. See docs/pebble.md.

#include <pebble.h>
#include "link.h"
#include "talk.h"
#include "chats.h"
#include "request.h"

int main(void) {
  link_open((LinkHandlers) {
    .on_reply = talk_on_reply,
    .on_chat_item = chats_on_item,
    .on_request = talk_on_request,
    .on_request_end = request_end,
    .on_status = talk_on_status,
    .on_send_failed = talk_on_send_failed,
  });
  talk_init();
  app_event_loop();
  talk_deinit();
}
