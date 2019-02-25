#pragma once
#include <stdio.h>
#include <glib.h>
#include <pthread.h>
#include <sys/time.h>
#include <ogg/ogg.h>
#include <opus/opus.h>

#include <janus/plugins/plugin.h>
#include <uuid/uuid.h>

#define LOBBY_ERROR_LOBBY_LIMIT_REACHED		100

typedef struct lobby {
	char name[256], desc[256], subj[128], video_auth[64], video_key[256];
	unsigned int max_clients;
	unsigned int current_clients;
	pthread_t mix_thread;
	struct peer** participants; //array
	pthread_mutex_t mutex; //for lobby properties (i.e. name, desc, etc.)
	pthread_mutex_t peerlist_mutex; //for participants array and client count
	OpusEncoder* encoder;
	ogg_stream_state* in_ss, *out_ss;
	FILE* in_file, *out_file;
	char video_vcodec[16], video_acodec[16];
	int video_asample, video_achannels;
	unsigned int audio_enabled	: 1;
	unsigned int audio_failed	: 1;
	unsigned int video_enabled	: 1;
	unsigned int video_active	: 1;
	unsigned int is_private		: 1;
	unsigned int die		: 1;
} lobby;

int lobbies_init();
int lobbies_shutdown();

int addLobby(lobby*);
void removeLobby(lobby*);
void lobbies_remove_peer(struct peer*);
void lobbies_remove_all_peers(lobby*);
lobby* lobbies_get_lobby(const char*);
void lobbies_set_limit(unsigned int);
GList* lobbies_get_lobbies();

