#include <pthread.h>
#include <strings.h> //strcasecmp
#include <janus/utils.h> //janus_get_monotonic_time
#include <janus/plugins/plugin.h>
#include <janus/apierror.h>

#include "Messaging.h"
#include "StreamLobby.h"
#include "Config.h"
#include "Sessions.h"

int message_sanity_checks(janus_plugin_session* handle, json_t* message, char* error)
{
	JANUS_LOG(LOG_DBG, "Starting message sanity checks\n");
	if(!json_is_object(message)) {
		if(error != NULL)
			snprintf(error, 256, "JSON error: not an object");
		json_decref(message);
		return MSG_ERROR_JSON_NOT_AN_OBJECT;
	}
	json_t* request = json_object_get(message, "request");
	if(request == NULL) {
		if(error != NULL)
			snprintf(error, 256, "Missing element (request)");
		json_decref(message);
		return MSG_ERROR_JSON_MISSING_ELEMENT;
	}
	if(!json_is_string(request)) {
		if(error != NULL)
			snprintf(error, 256, "Invalid element (request should be a string)");
		json_decref(message);
		return MSG_ERROR_JSON_INVALID_ELEMENT;
	}
	return 0;
}


/*Response json structure:
  {
	  "status": "ok" | "error",
	  "error_code": <int> (not present if status is 'ok'),
	  "stuff": <object> (not present if there's no data to return),
  }
*/
struct janus_plugin_result* handle_message(janus_plugin_session *handle, char* transaction, json_t* message, json_t* jsep)
{
	JANUS_LOG(LOG_DBG, "handle_message() start\n");
	if(stream_lobby_is_stopping() || !stream_lobby_is_initialized() || handle == NULL || handle->plugin_handle == NULL || message == NULL)
		return janus_plugin_result_new(JANUS_PLUGIN_ERROR, "Error with Janus Gateway", NULL);
	
	char error_msg[256];
	int error = message_sanity_checks(handle, message, error_msg);
	JANUS_LOG(LOG_DBG, "Sanity check result: %d\n", error);
	if(error != 0)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Error processing message. %d: %s\n", error, error_msg);
		return janus_plugin_result_new(JANUS_PLUGIN_OK, error_msg, NULL);
	}
	json_t* response = json_object();
	char response_sdp[1024] = {0};
	const char* request = json_string_value(json_object_get(message, "request"));
	
	//TODO - Have to put a safeguard in here to stop clients from blocking everything with stupid shit like while(1){ send_message("list_rooms"); }
	if(strcasecmp(request, "list_rooms") == 0)
	{
		JANUS_LOG(LOG_DBG, "list_rooms start\n");
		
		peer* dude = handle->plugin_handle;
		char all_rooms = 0;
		pthread_mutex_lock(&dude->mutex);
			if(dude->is_admin)
			{
				json_t* hidden_json = json_object_get(message, "include_hidden");
				if(hidden_json != NULL && json_is_boolean(hidden_json))
					all_rooms = json_is_true(hidden_json);
			}
		pthread_mutex_unlock(&dude->mutex);
		
		json_t* rooms_json = json_array();
		GList *rooms, *cr;
		rooms = lobbies_get_lobbies();
		cr = rooms;
		while(cr)
		{
			lobby* room = cr->data;
			if(room->is_private && !all_rooms)
			{
				cr = cr->next;
				continue;
			}
			json_t* tmp_json = json_object();
			pthread_mutex_lock(&room->mutex);
				json_object_set_new(tmp_json, "name", json_string(room->name));
				json_object_set_new(tmp_json, "subject", json_string(room->subj));
				json_object_set_new(tmp_json, "description", json_string(room->desc));
				json_object_set_new(tmp_json, "audio_enabled", json_integer(room->audio_enabled));
				json_object_set_new(tmp_json, "video_enabled", json_integer(room->video_enabled));
				json_object_set_new(tmp_json, "video_active", json_integer(room->video_active));
				json_object_set_new(tmp_json, "max_clients", json_integer(room->max_clients));
			pthread_mutex_unlock(&room->mutex);
			pthread_mutex_lock(&room->peerlist_mutex);
				json_object_set_new(tmp_json, "connected_clients", json_integer(g_atomic_int_get(&room->current_clients)));
			pthread_mutex_unlock(&room->peerlist_mutex);
			json_array_append_new(rooms_json, tmp_json);
			cr = cr->next;
		}
		json_object_set_new(response, "status", json_string("ok"));
		json_object_set(response, "stuff", rooms_json);
		json_decref(rooms_json);
	} //end list_rooms
	
	else if(strcasecmp(request, "join_room") == 0)
	{
		JANUS_LOG(LOG_DBG, "join_room start\n");
		peer* dude = handle->plugin_handle;
		lobby* room;
		json_t* room_json = json_object_get(message, "room");
		if(room_json == NULL)
		{
			error = MSG_ERROR_JSON_MISSING_ELEMENT;
			snprintf(error_msg, 256, "No lobby name provided");
			goto error;
		}
		if(!json_is_string(room_json))
		{
			error = MSG_ERROR_JSON_INVALID_ELEMENT;
			snprintf(error_msg, 256, "Lobby name is not a string");
			goto error;
		}
		room = lobbies_get_lobby(json_string_value(room_json));
		if(room == NULL)
		{
			error = MSG_ERROR_JOIN_INVALID_LOBBY;
			snprintf(error_msg, 256, "Requested lobby does not exist");
			goto error;
		}
		pthread_mutex_lock(&room->mutex);
			if(room->die)
			{
				pthread_mutex_unlock(&room->mutex);
				error = MSG_ERROR_JOIN_INVALID_LOBBY;
				snprintf(error_msg, 256, "Requested lobby is shutting down");
				goto error;
			}
		pthread_mutex_unlock(&room->mutex);
		if(g_atomic_int_get(&room->current_clients) >= room->max_clients)
		{
			error = MSG_ERROR_JOIN_LOBBY_FULL;
			snprintf(error_msg, 256, "Requested lobby is full");
			goto error;
		}
		
		//Error checks finished, start setting stuff up
		lobbies_remove_peer(dude);
		
		//Generate lobby id for session, and assign session to that slot
		srand(time(NULL));
		unsigned int tmp_id;
		char joined;
		do
		{
			tmp_id = rand() % room->max_clients;
			//TODO - I'm uncertain about this line, particularly the ampersand
			joined = g_atomic_pointer_compare_and_exchange(&room->participants[tmp_id], NULL, dude);
		} while(g_atomic_int_get(&room->current_clients) < room->max_clients && !joined);

		if(!joined)
		{
			error = MSG_ERROR_JOIN_LOBBY_FULL;
			snprintf(error_msg, 256, "Requested lobby is full");
			goto error;
		}
		
		pthread_mutex_lock(&dude->mutex);
			do
			{
				dude->lobby_id = rand() % room->max_clients;
			} while(room->participants[dude->lobby_id] != NULL);
			room->participants[dude->lobby_id] = dude;
			dude->current_lobby = room;
			g_atomic_int_inc(&room->current_clients);
			if(!dude->comms_ready)
			{
				dude->opus_pt = 0;
			}
		pthread_mutex_unlock(&dude->mutex);
		message_lobby(room, "peer_join", dude);
		json_object_set_new(response, "status", json_string("ok"));
		//TODO - Return the lobby's properties in the json (i.e. if there's a video stream available)
	} //end join_room

	else if(strcasecmp(request, "leave_room") == 0)
	{
		JANUS_LOG(LOG_DBG, "leave_room start\n");
		peer* dude = handle->plugin_handle;
		lobbies_remove_peer(dude);
		json_object_set_new(response, "status", json_string("ok"));
	}

	else if(strcasecmp(request, "list_peers") == 0)
	{
		JANUS_LOG(LOG_DBG, "list_peers start\n");
	}

	else if(strcasecmp(request, "sdp_pass") == 0)
	{
		JANUS_LOG(LOG_DBG, "sdp_passthrough start\n");
		if(jsep == NULL)
		{
			error = MSG_ERROR_SDP_ERROR;
			snprintf(error_msg, 256, "No JSEP object to process");
			goto error;
		}
		char* sdp = json_dumps(jsep, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
		const char* sdp_type = json_string_value(json_object_get(jsep, "type"));
		//If its an SDP offer, create an answer, otherwise let janus do it's thing
		if(!strcasecmp(sdp_type, "offer"))
		{
			//Reject offer if it isn't sendonly
			if(strstr(sdp, "sendonly") == NULL)
			{
				error = MSG_ERROR_SDP_INVALID_OFFER;
				snprintf(error_msg, 256, "SDP offers must be sendonly");
				goto error;
			}
			peer* dude = handle->plugin_handle;
			pthread_mutex_lock(&dude->mutex);
			if(dude->current_lobby == NULL)
			{
				pthread_mutex_unlock(&dude->mutex);
				error = MSG_ERROR_SDP_NO_LOBBY;
				snprintf(error_msg, 256, "Cannot process SDP before client has entered a lobby");
				goto error;
			}
			lobby* room = dude->current_lobby;
			if(!room->audio_enabled)
			{
				pthread_mutex_unlock(&dude->mutex);
				error = MSG_ERROR_SDP_NO_LOBBY;
				snprintf(error_msg, 256, "Lobby does not support audio");
				goto error;
			}
			pthread_mutex_unlock(&dude->mutex);

			dude->opus_pt = janus_get_codec_pt(sdp, "opus");
			if(dude->opus_pt == -1)
				dude->opus_pt = 0;
			int offset = snprintf(response_sdp, 1024, "v=0\n"
					"o=server %"SCNu64" %"SCNu64" IN IP4 127.0.0.1\n"
					"s=stream session\n"
					"t=0 0\n",
					janus_get_monotonic_time(),
					janus_get_monotonic_time());
			if(offset < 0)
			{
				error = MSG_ERROR_SDP_ERROR;
				snprintf(error_msg, 256, "Error while creating SDP response");
				goto error;
			}
			//Audio
			if(strstr(sdp, "m=audio") == NULL)
			{
				error = MSG_ERROR_SDP_NO_MEDIA;
				snprintf(error_msg, 256, "SDP offers must have at least one audio track");
				goto error;
			}
			else
			{
				offset += snprintf(response_sdp+offset, 1024-offset, "m=audio 1 RTP/SAVPF %d\r\n", dude->opus_pt);
				offset += snprintf(response_sdp+offset, 1024-offset, "a=rtpmap:%d opus/48000/2\r\n", dude->opus_pt);
				offset += snprintf(response_sdp+offset, 1024-offset, "a=fmtp:%d maxplaybackrate=%d;stereo=0;\r\n", dude->opus_pt, SETTINGS_SAMPLE_RATE);
				offset += snprintf(response_sdp+offset, 1024-offset, "a=recvonly\r\n");
				offset += snprintf(response_sdp+offset, 1024-offset, "c=IN IP4 1.1.1.1\r\n");
			}
			//Reject video
			if(strstr(sdp, "m=video") != NULL)
				offset += snprintf(response_sdp+offset, 1024-offset, "m=video 0 RTP/AVP 0\r\n");
			//Reject data channels
			if(strstr(sdp, "DTLS/SCTP") != NULL)
				offset += snprintf(response_sdp+offset, 1024-offset, "m=application 0 DTLS/SCTP 0\r\n");


			json_t* sdp_json = json_object();
			json_object_set_new(sdp_json, "status", json_string("ok"));
			json_t* offer_jsep = json_pack("{ssss}", "type", "answer", "sdp", response_sdp);
			int result = janus_gateway->push_event(handle, &stream_lobby_plugin, "sdp_answer", sdp_json, offer_jsep);
			if(result != JANUS_OK)
			{
				error = MSG_ERROR_SDP_SEND_FAIL;
				snprintf(error_msg, 256, "Error sending you the SDP offer");
				goto error;
			}
		}

		json_object_set_new(response, "status", json_string("ok"));
		//json_object_set_new(response, "stuff", json_string(response_sdp));
		free(sdp);
	}

	else if(strcasecmp(request, "request_sdp_offer") == 0)
	{
		JANUS_LOG(LOG_DBG, "request_sdp_offer start\n");
		peer* dude = handle->plugin_handle;
		pthread_mutex_lock(&dude->mutex);
		if(dude->current_lobby == NULL)
		{
			pthread_mutex_unlock(&dude->mutex);
			error = MSG_ERROR_SDP_NO_LOBBY;
			snprintf(error_msg, 256, "Cannot process SDP before client has entered a lobby");
			goto error;
		}
		lobby* room = dude->current_lobby;
		if(!room->audio_enabled && !room->video_enabled)
		{
			pthread_mutex_unlock(&dude->mutex);
			error = MSG_ERROR_SDP_NO_LOBBY;
			snprintf(error_msg, 256, "Lobby does not support audio or video");
			goto error;
		}
		pthread_mutex_unlock(&dude->mutex);
		char audio = json_integer_value(json_object_get(message, "audio"));
		char video = json_integer_value(json_object_get(message, "video"));
		if(!audio && !video)
		{
			error = MSG_ERROR_SDP_NO_MEDIA;
			snprintf(error_msg, 256, "No media specified");
			goto error;
		}


		int no_media = 1, offset = snprintf(response_sdp, 1024, "v=0\r\n"
			/*username,id,version number,IP addr*/
			"o=server %"SCNu64" %"SCNu64" IN IP4 127.0.0.1\r\n"
			"s=stream session\r\n"
			"t=0 0\r\n",
			janus_get_monotonic_time(),
			janus_get_monotonic_time());
		if(offset<0)
		{
			error = MSG_ERROR_SDP_ERROR;
			snprintf(error_msg, 256, "Error while creating SDP offer");
			goto error;
		}
		if(audio && room->audio_enabled)
		{
			/*Payload types in the range 96-127 are dynamically defined payload types
			Reference: https://tools.ietf.org/html/rfc3551#section-5*/
			offset += snprintf(response_sdp+offset, 1024-offset, "m=audio 1 RTP/SAVPF 96\r\n");
			offset += snprintf(response_sdp+offset, 1024-offset, "a=rtpmap:96 opus/48000/2\r\n");
			offset += snprintf(response_sdp+offset, 1024-offset, "c=IN IP4 1.1.1.1\r\n");
			offset += snprintf(response_sdp+offset, 1024-offset, "a=fmtp:96 maxplaybackrate=48000;stereo=%d;sprop-stereo=%d;useinbandfec=0\r\n", SETTINGS_CHANNELS-1, SETTINGS_CHANNELS-1);
			dude->opus_pt = 96;
			no_media = 0;
		}
		if(video && room->video_active)
		{
			offset += snprintf(response_sdp+offset, 1024-offset, "m=audio 1 RTP/SAVPF 97\r\n");
			offset += snprintf(response_sdp+offset, 1024-offset, "a=sendonly\r\n");
			offset += snprintf(response_sdp+offset, 1024-offset, "a=rtpmap:97 %s/%d/%d\r\n", room->video_acodec, room->video_asample, room->video_achannels);
			offset += snprintf(response_sdp+offset, 1024-offset, "c=IN IP4 1.1.1.1\r\n");
			offset += snprintf(response_sdp+offset, 1024-offset, "m=video 1 RTP/SAVPF 98\r\n");
			offset += snprintf(response_sdp+offset, 1024-offset, "a=sendonly\r\n");
			offset += snprintf(response_sdp+offset, 1024-offset, "a=rtpmap:98 %s/90000\r\n", room->video_vcodec);
			offset += snprintf(response_sdp+offset, 1024-offset, "c=IN IP4 1.1.1.1\r\n");
			no_media = 0;
		}

		if(no_media)
		{
			error = MSG_ERROR_SDP_NO_MEDIA;
			snprintf(error_msg, 256, "This lobby does not support the requested media");
		}
		else
		{
			json_object_set_new(response, "status", json_string("ok"));
			json_t* sdp_json = json_object();
			json_object_set_new(sdp_json, "status", json_string("ok"));
			json_t* offer_jsep = json_pack("{ssss}", "type", "offer", "sdp", response_sdp);
			int result = janus_gateway->push_event(handle, &stream_lobby_plugin, "sdp_offer", sdp_json, offer_jsep);
			if(result != JANUS_OK)
			{
				error = MSG_ERROR_SDP_SEND_FAIL;
				snprintf(error_msg, 256, "Error sending SDP offer");
			}
			json_decref(sdp_json);
			json_decref(offer_jsep);
		}
	}

	else if(strcasecmp(request, "change_nick") == 0)
	{
		JANUS_LOG(LOG_DBG, "[Stream Lobby] change_nick start\n");
		//TODO - Limit on how often nick can be changed
		peer* dude = handle->plugin_handle;
		const char* new_nick = json_string_value(json_object_get(jsep, "nick"));
		if(strlen(new_nick) == 0)
		{
			JANUS_LOG(LOG_ERR, "[Stream Lobby] Empty nick given\n");
			error = MSG_ERROR_NICK_EMPTY;
			snprintf(error_msg, 256, "Empty nick given");
			goto error;
		}
		//TODO - More sanitizing. stop nicks that are just spaces or underscores and the like
		pthread_mutex_lock(&dude->mutex);
			snprintf(dude->nick, 64, "%s", new_nick);
			lobby* room = dude->current_lobby;
		pthread_mutex_unlock(&dude->mutex);
		message_lobby(room, "nick_change", dude);
	}

	else if(strcasecmp(request, "say") == 0)
	{
		JANUS_LOG(LOG_DBG, "[Stream Lobby] say start\n");
		error = MSG_ERROR_COMMAND_NOT_IMPLEMENTED;
		snprintf(error_msg, 256, "Command not implemented");
	}

	else if(strcasecmp(request, "upload_image") == 0)
	{
		JANUS_LOG(LOG_DBG, "[Stream Lobby] upload_image start\n");
		error = MSG_ERROR_COMMAND_NOT_IMPLEMENTED;
		snprintf(error_msg, 256, "Command not implemented");
	}

	else if(strcasecmp(request, "mute") == 0)
	{
		JANUS_LOG(LOG_DBG, "[Stream Lobby] mute start\n");
		error = MSG_ERROR_COMMAND_NOT_IMPLEMENTED;
		snprintf(error_msg, 256, "Command not implemented");
	}

	else if(strcasecmp(request, "request_admin") == 0)
	{
		JANUS_LOG(LOG_DBG, "[Stream Lobby] request_admin start\n");
		error = MSG_ERROR_COMMAND_NOT_IMPLEMENTED;
		snprintf(error_msg, 256, "Command not implemented");
	}

	else
	{
		error = MSG_ERROR_UNKNOWN_COMMAND;
		snprintf(error_msg, 256, "Unknown command %s", request);
	}

	json_decref(message);
	char* result_text = json_dumps(response, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
	JANUS_LOG(LOG_DBG, "Response to peer: %s\n", result_text);
	free(result_text);
	return janus_plugin_result_new(JANUS_PLUGIN_OK, NULL, response);

error:
	json_decref(message);
	JANUS_LOG(LOG_ERR, "[Stream Lobby] Error %d processing message: %s\n", error, error_msg);
	json_t* err_json = json_object();
	json_object_set_new(err_json, "status", json_string("error"));
	json_object_set_new(err_json, "error_code", json_integer(error));
	json_object_set_new(err_json, "error_message", json_string(error_msg));
	return janus_plugin_result_new(JANUS_PLUGIN_OK, NULL, err_json);
}

/*Event message structure
  {
	  "event": <string>,
	  "stuff": {
		  //the data for this object varies depending on the event
		  "uuid": <string> (not always present),
		  "nick": <strong> (not always present),
	  }
  }
*/
void message_lobby(lobby* room, const char* msg_type, peer* dude)
{
	if(room == NULL || msg_type == NULL)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Missing argument, abandoning message_lobby()\n");
		return;
	}
	
	json_t* event_json = json_object();
	json_object_set_new(event_json, "event", json_string(msg_type));
	if(!strcasecmp(msg_type, "peer_leave"))
	{
		if(!dude)
		{
			JANUS_LOG(LOG_ERR, "[Stream Lobby] No peer argument, abandoning message_lobby()\n");
			return;
		}
		int j = 0;
		char uid[37] = {0};
		uuid_unparse(dude->uuid, uid);
		json_t* data_json = json_object();
		
		json_object_set_new(data_json, "uuid", json_string(uid));
		json_object_set_new(event_json, "stuff", data_json);
		
		peer* participants_list[room->max_clients];
		memset(participants_list, 0, sizeof(peer*) * room->max_clients);
		pthread_mutex_lock(&room->peerlist_mutex);
			for(int i = 0; i < room->max_clients; i++)
			{
				if(room->participants[i] != NULL)
					participants_list[j++] = room->participants[i];
			}
		pthread_mutex_unlock(&room->peerlist_mutex);
		for(int i = 0; i < j; i++)
		{
			peer* p = participants_list[i];
			if(p == dude)
				continue;
			janus_gateway->push_event(p->session, &stream_lobby_plugin, NULL, event_json, NULL);
			json_decref(event_json);
		}
	}
	else if(!strcasecmp(msg_type, "peer_join"))
	{
		if(!dude)
		{
			JANUS_LOG(LOG_ERR, "[Stream Lobby] No peer argument, abandoning message_lobby()\n");
			return;
		}
		int j = 0;
		char uid[37] = {0};
		uuid_unparse(dude->uuid, uid);
		json_t* data_json = json_object();
		
		json_object_set_new(data_json, "uuid", json_string(uid));
		pthread_mutex_lock(&dude->mutex);
			json_object_set_new(data_json, "nick", json_string(dude->nick));
		pthread_mutex_unlock(&dude->mutex);
		json_object_set_new(event_json, "stuff", data_json);
		
		peer* participants_list[room->max_clients];
		memset(participants_list, 0, sizeof(peer*) * room->max_clients);
		pthread_mutex_lock(&room->peerlist_mutex);
			for(int i = 0; i < room->max_clients; i++)
			{
				if(room->participants[i] != NULL)
					participants_list[j++] = room->participants[i];
			}
		pthread_mutex_unlock(&room->peerlist_mutex);
		for(int i = 0; i < j; i++)
		{
			peer* p = participants_list[i];
			if(p == dude)
				continue;
			janus_gateway->push_event(p->session, &stream_lobby_plugin, NULL, event_json, NULL);
			json_decref(event_json);
		}
	}
	else if(!strcasecmp(msg_type, "nick_change"))
	{
		if(!dude)
		{
			JANUS_LOG(LOG_ERR, "[Stream Lobby] No peer argument, abandoning message_lobby()\n");
			return;
		}
		int j = 0;
		char uid[37] = {0};
		uuid_unparse(dude->uuid, uid);
		json_t* data_json = json_object();

		json_object_set_new(data_json, "uuid", json_string(uid));
		pthread_mutex_lock(&dude->mutex);
			json_object_set_new(data_json, "nick", json_string(dude->nick));
		pthread_mutex_unlock(&dude->mutex);
		json_object_set_new(event_json, "stuff", data_json);

		peer* participants_list[room->max_clients];
		memset(participants_list, 0, sizeof(peer*) * room->max_clients);
		pthread_mutex_lock(&room->peerlist_mutex);
			for(int i = 0; i < room->max_clients; i++)
			{
				if(room->participants[i] != NULL)
					participants_list[j++] = room->participants[i];
			}
		pthread_mutex_unlock(&room->peerlist_mutex);
		for(int i = 0; i < j; i++)
		{
			peer* p = participants_list[i];
			if(p == dude)
				continue;
			janus_gateway->push_event(p->session, &stream_lobby_plugin, NULL, event_json, NULL);
			json_decref(event_json);
		}
	}
	json_decref(event_json);
}

void message_peer(peer* room, const char* msg_type, peer* dude)
{
	if(room == NULL || msg_type == NULL)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Missing argument, abandoning message_lobby()\n");
		return;
	}
	json_t* event_json = json_object();
	json_object_set_new(event_json, "event", json_string(msg_type));

	if(!strcasecmp(msg_type, "peer_leave"))
	{
		JANUS_LOG(LOG_INFO, "Sending peer_leave message to peer");
	}
	else if(!strcasecmp(msg_type, "peer_join"))
	{
		JANUS_LOG(LOG_INFO, "Sending peer_join message to peer");
	}
	else if(!strcasecmp(msg_type, "nick_change"))
	{
		JANUS_LOG(LOG_INFO, "Sending nick_change message to peer");
	}
	json_decref(event_json);
}
