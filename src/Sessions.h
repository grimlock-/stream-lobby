#pragma once
#include <pthread.h>
#include <glib.h>
#include <jansson.h>
#include <uuid/uuid.h>
#include <opus/opus.h>

#include <janus/plugins/plugin.h>

#include "Lobbies.h"

typedef struct peer {
	janus_plugin_session* session;
	uuid_t uuid;
	pthread_mutex_t mutex; //Used to access all fields below
	struct lobby* current_lobby;
	unsigned int lobby_id;
	char nick[64];
	opus_int32 *buffer_head, *buffer_tail, *buffer_start, *buffer_end;
	int sample_count;
	struct timeval buffering_start;
	GList* packets;
	pthread_mutex_t packets_mutex;
	pthread_t decoder_thread;
	uint16_t next_seq_num;
	int opus_pt;
	OpusDecoder* decoder;
	unsigned int is_admin      : 1;
	unsigned int comms_ready   : 1;
	unsigned int receive_audio : 1;
	unsigned int receive_video : 1;
	unsigned int finished_buffering : 1;
} peer;

int sessions_init();
int sessions_shutdown();
void sessions_create_session(janus_plugin_session* handle, int* error);
void sessions_destroy_session(janus_plugin_session*, int*);
json_t* sessions_query_session(janus_plugin_session*);
