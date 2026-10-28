#pragma once

// The talk screen: what was said, the turn as it runs, and the reply; the chat it talks in and how the phone stands
// along the top.

#include "link.h"

void talk_init(void);
void talk_deinit(void);

// What the link hands over, for main.c to wire in.
void talk_on_reply(const LinkReply *reply);
void talk_on_status(const LinkStatus *status);
void talk_on_send_failed(int type);
// Shows an approval or a question in the talk screen's colours.
void talk_on_request(const LinkRequest *request);
