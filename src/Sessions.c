#include "Sessions.h"
#include "StreamLobby.h"
#include <janus/debug.h>

static GHashTable* connected_peers;
static pthread_mutex_t peer_mutex;

int sessions_init()
{
	connected_peers = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
	pthread_mutex_init(&peer_mutex, NULL);
	return 0;
}

int sessions_shutdown()
{
	pthread_mutex_lock(&peer_mutex);
		if(g_hash_table_size(connected_peers) == 0)
		{
			pthread_mutex_unlock(&peer_mutex);
			return 0;
		}

		GList* items, *current_item;
		int error = 0;
		items = g_hash_table_get_values(connected_peers);
	//FIXME - The mutex really shouldn't be unlocked until all the sessions have been destroyed
	//	- Plus I'll have to double check every line where the mutex is locked to ensure it
	//	- doesn't get locked after the server has started shutting down
	pthread_mutex_unlock(&peer_mutex);
	
	current_item = items;
	while(current_item)
	{
		peer* dude = current_item->data;
		sessions_destroy_session(dude->session, &error);
		if(error != 0) {
			JANUS_LOG(LOG_ERR, "[Stream Lobby] Error #%d while destroying session\n", error);
		}
		current_item = current_item->next;
	}

	pthread_mutex_lock(&peer_mutex);
		g_hash_table_destroy(connected_peers);
	pthread_mutex_unlock(&peer_mutex);

	pthread_mutex_destroy(&peer_mutex);
	return 0;
}

void sessions_create_session(janus_plugin_session* handle, int* error)
{
	if(stream_lobby_is_stopping() || !stream_lobby_is_initialized())
	{
		*error = 1;
		return;
	}
	
	peer* dude = calloc(1, sizeof(peer));
	if(dude == NULL)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Memory allocation failure! Could not create session with peer.\n");
		*error = 2;
		return;
	}
	handle->plugin_handle = dude;
	uuid_generate(dude->uuid);
	snprintf(dude->nick, 64, "Anonymous");
	dude->session = handle;
	pthread_mutex_init(&dude->mutex, NULL);
	pthread_mutex_lock(&peer_mutex);
		g_hash_table_insert(connected_peers, dude->uuid, dude);
	pthread_mutex_unlock(&peer_mutex);
	char id[37];
	uuid_unparse(dude->uuid, id);
	JANUS_LOG(LOG_INFO, "Session %s created.\n", id);
	return;
}

void sessions_destroy_session(janus_plugin_session* handle, int* error)
{
	JANUS_LOG(LOG_DBG, "destroy_session started\n");
	if(!stream_lobby_is_initialized())
	{
		*error = 1;
		return;
	}
	if(handle->plugin_handle == NULL)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] No plugin data for this session. Goodbye you little shit.\n");
		*error = 2;
		return;
	}
	
	peer* dude = handle->plugin_handle;
	lobbies_remove_peer(dude);
	pthread_mutex_lock(&peer_mutex);
		g_hash_table_remove(connected_peers, dude->uuid);
	pthread_mutex_unlock(&peer_mutex);
	char id[37], nick[64];
	uuid_unparse(dude->uuid, id);
	//TODO - If you're going to use sprintf, escape the characters in the peer's nick
	snprintf(nick, 64, "%s", dude->nick);
	pthread_mutex_destroy(&dude->mutex);
	free(dude);
	handle->plugin_handle = NULL;
	JANUS_LOG(LOG_INFO, "Session %s (%s) destroyed.\n", id, nick);
	return;
}

/*json structure
  {
	  "uuid": <string>,
	  "nick": <string>,
	  "lobby": <string>
  }
*/
json_t* sessions_query_session(janus_plugin_session* handle)
{
	if(stream_lobby_is_stopping() || !stream_lobby_is_initialized())
		return NULL;
	if(handle->plugin_handle == NULL)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] No plugin data for this session. Session query rejected.\n");
		return NULL;
	}
	
	json_t* response = json_object();
	peer* dude = handle->plugin_handle;
	char uid[37];
	uuid_unparse(dude->uuid, uid);
	json_object_set_new(response, "uuid", json_string(uid));
	pthread_mutex_lock(&dude->mutex);
		json_object_set_new(response, "nick", json_string(dude->nick));
		pthread_mutex_lock(&dude->current_lobby->mutex);
			json_object_set_new(response, "lobby", json_string(dude->current_lobby->name));
		pthread_mutex_unlock(&dude->current_lobby->mutex);
	pthread_mutex_unlock(&dude->mutex);
	return response;
}

