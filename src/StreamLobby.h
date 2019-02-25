#pragma once
#include <stdio.h>
#include <ogg/ogg.h>

// Plugin info
#define PLUGIN_VERSION			1
#define PLUGIN_VERSION_STRING		"1"
#define PLUGIN_DESCRIPTION		"Creates a voice and text chat room which can also send out a video stream"
#define PLUGIN_NAME			"Stream Lobby"
#define PLUGIN_AUTHOR			"grimlock-"
#define PLUGIN_PACKAGE			"plugin.streamlobby"

/* Initialization error codes: */
#define INIT_ERROR_INVALID_ARGS 		100
#define INIT_ERROR_MEM_ALLOC_FAIL 		101
#define INIT_ERROR_THREAD_CREATION_FAIL 	102
#define INIT_ERROR_LOBBY_CREATION_FAIL 		103
#define INIT_ERROR_SESSION_CREATION_FAIL	104
#define INIT_ERROR_CONFIG_ERROR			110

extern janus_callbacks* janus_gateway;
extern janus_plugin stream_lobby_plugin;

int stream_lobby_init(janus_callbacks*, const char*);
void stream_lobby_shutdown(void);
void stream_lobby_enable_admin();
void stream_lobby_disable_admin();
int stream_lobby_is_admin_enabled();
void stream_lobby_set_admin_pass(const char*);
int stream_lobby_is_initialized();
int stream_lobby_is_stopping();
void stream_lobby_set_initialized(int);
void stream_lobby_set_stopping(int);

int stream_lobby_get_api_compatibility(void);
int stream_lobby_get_version(void);
const char* stream_lobby_get_version_string(void);
const char* stream_lobby_get_description(void);
const char* stream_lobby_get_name(void);
const char* stream_lobby_get_author(void);
const char* stream_lobby_get_package(void);


