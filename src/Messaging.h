#pragma once
#include <jansson.h>
#include <janus/plugins/plugin.h>

#include "Lobbies.h"
#include "Sessions.h"

#define MSG_ERROR_JSON_GENERIC_ERROR		200
#define MSG_ERROR_JSON_NOT_AN_OBJECT		201
#define MSG_ERROR_JSON_MISSING_ELEMENT		202
#define MSG_ERROR_JSON_INVALID_ELEMENT		203
#define MSG_ERROR_UNKNOWN_COMMAND		210
#define MSG_ERROR_COMMAND_NOT_IMPLEMENTED	211
#define MSG_ERROR_SDP_ERROR			220
#define MSG_ERROR_SDP_NO_LOBBY			221
#define MSG_ERROR_SDP_NO_MEDIA			222
#define MSG_ERROR_SDP_NO_VIDEO			223
#define MSG_ERROR_SDP_SEND_FAIL			224
#define MSG_ERROR_SDP_INVALID_OFFER		225
#define MSG_ERROR_JOIN_INVALID_LOBBY		230
#define MSG_ERROR_JOIN_LOBBY_FULL		231
#define MSG_ERROR_NICK_EMPTY			240

int message_sanity_checks(janus_plugin_session*, json_t*, char*);
void message_lobby(lobby*, const char*, peer*);
void message_peer(peer*, const char*, peer*);
janus_plugin_result* handle_message(janus_plugin_session*, char*, json_t*, json_t*);
