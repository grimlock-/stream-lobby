#include "Lobbies.h"
#include "Sessions.h"
#include "Audio.h"
#include "Messaging.h"
static unsigned int lobby_limit = 50;
static unsigned int lobby_count;
static int threadinit_result;
static GHashTable* lobbies;
static pthread_mutex_t lobby_mutex;

int lobbies_init()
{
	lobbies = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
	pthread_mutex_init(&lobby_mutex, NULL);
	pthread_mutex_init(&audio_mix_threads_mutex, NULL);
	return 0;
}

int lobbies_shutdown()
{
	if(g_atomic_int_get(&lobby_count) == 0)
		return 0;

	GList *items, *current_item;
	lobby* room;
	char wait = 0;
	
	items = lobbies_get_lobbies();
	current_item = items;

	//Remove all peers and mark rooms with audio for shutdown
	while(current_item)
	{
		room = current_item->data;
		
		pthread_mutex_lock(&room->mutex);
			if(room->audio_enabled)
			{
				wait = 1;
				room->die = 1;
			}
		pthread_mutex_unlock(&room->mutex);
		if(g_atomic_int_get(&room->current_clients) > 0)
			lobbies_remove_all_peers(current_item->data);
		current_item = current_item->next;
	}
	
	//Wait on audio mixing threads
	if(wait)
	{
		pthread_cond_init(&audio_destroy_threads_cond, NULL);
		
		pthread_mutex_lock(&audio_mix_threads_mutex);
			while(g_atomic_int_get(&audio_mix_thread_count) > 0)
				pthread_cond_wait(&audio_destroy_threads_cond, &audio_mix_threads_mutex);
		pthread_mutex_unlock(&audio_mix_threads_mutex);

		pthread_cond_destroy(&audio_destroy_threads_cond);
		pthread_mutex_destroy(&audio_mix_threads_mutex);
	}
	
	//Free lobby resources
	current_item = items;
	while(current_item)
	{
		room = current_item->data;

		pthread_mutex_destroy(&room->mutex);
		//Peer list
		free(room->participants);
		room->participants = NULL;
		pthread_mutex_destroy(&room->peerlist_mutex);
		//Opus stuff
		opus_encoder_destroy(room->encoder);
		room->encoder = NULL;
		pthread_mutex_lock(&lobby_mutex);
			if(g_hash_table_remove(lobbies, room->name))
			{
				g_atomic_int_dec_and_test(&lobby_count);
				JANUS_LOG(LOG_VERB, "[Stream Lobby] Successfully removed lobby \"%s\" from hash table. Remaining lobbies: %lu\n", room->name, g_atomic_int_get(&lobby_count));
				free(room);
			}
			else
			{
				JANUS_LOG(LOG_ERR, "[Stream Lobby] Could not find or could not remove lobby \"%s\" from hash table.\n", room->name);
			}
		pthread_mutex_unlock(&lobby_mutex);
		
		current_item = current_item->next;
	}

	g_list_free(items);

	pthread_mutex_lock(&lobby_mutex);
		g_hash_table_destroy(lobbies);
	pthread_mutex_unlock(&lobby_mutex);

	pthread_mutex_destroy(&lobby_mutex);
	return 0;
}

/* Add pre-initialized lobby scructure to hash table */
int addLobby(lobby* newLobby)
{
	if(newLobby == NULL)
		return 1;
	if(g_atomic_int_get(&lobby_count) >= lobby_limit)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Lobby limit reached[%i]. Not adding lobby \"%s\".\n", lobby_limit, newLobby->name);
		return 2;
	}
	if(g_hash_table_lookup(lobbies, newLobby->name) != NULL)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] A lobby named \"%s\" has already been created.\n", newLobby->name);
		return 3;
	}
	
	if(newLobby->audio_enabled)
	{
		//audio mixing thread
		threadinit_result = pthread_create(&newLobby->mix_thread, NULL, &audio_mix_thread, newLobby);
		if(threadinit_result != 0) //failure
		{
			JANUS_LOG(LOG_ERR, "[Stream Lobby] Couldn't create audio mixing thread for lobby \"%s\". Disabling audio.\n", newLobby->name);
			switch(threadinit_result) {
				case EAGAIN:
					JANUS_LOG(LOG_ERR, "[Stream Lobby] EAGAIN - Insufficient resources OR a system-imposed thread limit was violated\n");
					break;
				case EINVAL:
					JANUS_LOG(LOG_ERR, "[Stream Lobby] EINVAL - Invalid thread attributes\n");
					break;
				case EPERM:
					JANUS_LOG(LOG_ERR, "[Stream Lobby] EPERM - No permission to set the scheduling policy and parameters specified in attr\n");
					break;
				default:
					JANUS_LOG(LOG_ERR, "[Stream Lobby] Unknown error code for creating thread\n");
					break;
			}
			newLobby->audio_enabled = 0;
			newLobby->audio_failed = 1;
			opus_encoder_destroy(newLobby->encoder);
			newLobby->encoder = NULL;
		}
	}
	
	pthread_mutex_lock(&lobby_mutex);
		g_hash_table_insert(lobbies, newLobby->name, newLobby);
	pthread_mutex_unlock(&lobby_mutex);
	g_atomic_int_inc(&lobby_count);
	JANUS_LOG(LOG_INFO, "Lobby \"%s\" created\n", newLobby->name);
	return 0;
}

void removeLobby(lobby* room)
{
	if(room == NULL)
		return;
	
	//Mark lobby for death
	pthread_mutex_lock(&room->mutex);
		if(room->die == 1)
		{
			pthread_mutex_unlock(&room->mutex);
			return;
		}
		JANUS_LOG(LOG_INFO, "Removing lobby \"%s\"\n", room->name);
		room->die = 1;
	pthread_mutex_unlock(&room->mutex);
	
	//Kick everybody out
	if(g_atomic_int_get(&room->current_clients) > 0)
		lobbies_remove_all_peers(room);

	//Wait on the audio mixing thread and destroy audio resources
	if(room->audio_enabled) {
		pthread_join(room->mix_thread, NULL);
		opus_encoder_destroy(room->encoder);
		room->encoder = NULL;
	}

	//Free resources
	free(room->participants);
	room->participants = NULL;
	pthread_mutex_destroy(&room->mutex);
	pthread_mutex_destroy(&room->peerlist_mutex);
	//Destroy the lobby structure
	pthread_mutex_lock(&lobby_mutex);
		int result = g_hash_table_remove(lobbies, room->name);
	pthread_mutex_unlock(&lobby_mutex);
	if(result)
	{
		g_atomic_int_dec_and_test(&lobby_count);
		JANUS_LOG(LOG_VERB, "[Stream Lobby] Successfully removed lobby \"%s\" from hash table. Remaining lobbies: %lu\n", room->name, g_atomic_int_get(&lobby_count));
		free(room);
	}
	else//Failure
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Could not find or could not remove lobby \"%s\" from hash table.\n", room->name);
	}
	return;
}

void lobbies_remove_peer(peer* dude)
{
	JANUS_LOG(LOG_DBG, "lobbies_remove_peer() start\n");
	if(dude == NULL)
		return;
	pthread_mutex_lock(&dude->mutex);
		if(dude->comms_ready)
			audio_hangup_media_no_lock(dude->session);
		if(dude->current_lobby == NULL)
		{
			pthread_mutex_unlock(&dude->mutex);
			JANUS_LOG(LOG_VERB, "[Stream Lobby] Peer is not in any lobby\n");
			return;
		}
		char* lobby_name = dude->current_lobby->name;
		message_lobby(dude->current_lobby, "peer_leave", dude);
		g_atomic_int_dec_and_test(&dude->current_lobby->current_clients);
		//TODO - I'm not certain about this line, particularly the ampersand
		g_atomic_pointer_set(&dude->current_lobby->participants[dude->lobby_id], NULL);
		dude->current_lobby = NULL;
		char id[37];
		uuid_unparse(dude->uuid, id);
		JANUS_LOG(LOG_INFO, "Session %s (%s) removed from lobby (%s)\n", id, dude->nick, lobby_name);
	pthread_mutex_unlock(&dude->mutex);
}

/*
 * Remove all peers from given lobby
 */
void lobbies_remove_all_peers(lobby* room)
{
	JANUS_LOG(LOG_DBG, "lobbies_remove_all_peers() start");
	peer* dude;
	pthread_mutex_lock(&room->peerlist_mutex);
		for(int i = 0; i < room->max_clients; i++)
		{
			if(room->participants[i] == NULL);
				continue;
			dude = room->participants[i];
			pthread_mutex_lock(&dude->mutex);
				if(dude->comms_ready)
					audio_hangup_media_no_lock(dude->session);
				if(dude->current_lobby == NULL)
				{
					pthread_mutex_unlock(&dude->mutex);
					continue;
				}
				message_peer(dude, "peer_leave", dude);
				g_atomic_int_dec_and_test(&dude->current_lobby->current_clients);
				//TODO - I'm not certain about this line, particularly the ampersand
				g_atomic_pointer_set(&dude->current_lobby->participants[dude->lobby_id], NULL);
				dude->current_lobby = NULL;
			pthread_mutex_unlock(&dude->mutex);
		}
	pthread_mutex_unlock(&room->peerlist_mutex);
}

/*
 * Returns a pointer to a newly created GList filled with Lobby structs
 * The GList must be freed by the calling function
 */
GList* lobbies_get_lobbies()
{
	pthread_mutex_lock(&lobby_mutex);
		GList* items = g_hash_table_get_values(lobbies);
	pthread_mutex_unlock(&lobby_mutex);
	return items;
}
void lobbies_set_limit(unsigned int newlimit)
{
	if(newlimit > 0 && newlimit < UINT_MAX)
		lobby_limit = newlimit;
}
unsigned int lobbies_get_limit()
{
	return lobby_limit;
}
lobby* lobbies_get_lobby(const char* name)
{
	pthread_mutex_lock(&lobby_mutex);
		lobby* room = g_hash_table_lookup(lobbies, name);
	pthread_mutex_unlock(&lobby_mutex);
	return room;
}
