//(function(){
	document.addEventListener("DOMContentLoaded", startup);
	var ws_connection, connection_type, server_connection, session_id, handle_id,
	server_domain = location.host, log_level = 4,
	audio_element, audio_context, source_nodes = [], processor_nodes = [], gain_nodes = [],
	video_element,
	current_status_light = null, light,
	page_content,
	rooms, current_room, waiting_to_join = -1,
	keepaliveTID = -1, connection_lightTID = -1,
	shutting_down = false,
	//audio_mode = "web_audio",
	audio_mode = "html5",
	ws_close_codes = {"1000":"CLOSE_NORMAL",
	"1001":"CLOSE_GOING_AWAY",		"1002":"CLOSE_PROTOCOL_ERROR",
	"1003":"CLOSE_UNSUPPORTED",		"1005":"CLOSE_NO_STATUS",
	"1006":"CLOSE_ABNORMAL",		"1007":"Unsupported Data",
	"1008":"Policy Violation",		"1009":"CLOSE_TOO_LARGE",
	"1010":"Missing Extension",		"1011":"Internal Error",
	"1012":"Service Restart",		"1015":"TLS Handshake"},
	user_commands = {"sdp_pass":0,
	"list_rooms":1, "join_room":2,
	"list_peers":3, "leave_room":4,
	"request_sdp_offer":5, "sdp_offer":6},
	LOG = {"NORMAL":1,
	"VERB":2, "DBG":3,
	"OGODWTF":4};
	
	function startup()
	{
		if(typeof RTCPeerConnection == "undefined")
		{
			var message = document.createElement('h3');
			message.innerText = "Your web browser does not support the WebRTC Peer Connection API. You must enable this API or use a different web browser to access this website."
			var newDiv = document.createElement('div');
			newDiv.className = "error_screen";
			newDiv.appendChild(message);
			document.body.appendChild(newDiv);
			return;
		}
		light = document.getElementById("connection_status_light");
		page_content = document.getElementById("content");
		//Add stuff to document
		var button = document.createElement("button");
		button.onclick = connect_to_server;
		button.innerText = "Connect";
		button.id = "server_connection_button";
		document.body.appendChild(button);
	}
	
	
	
	//Initialization stuff
		//Connection Init
		function ws_connect(e)
		{
			//Visual elements
			var button = e.currentTarget;
			button.disabled = true;
			button.innerText = "Connecting...";
			light.style.visibility = "visible";
			light.style.opacity = 1;
			set_status_light("yellow", "Connecting to MCU...");
			connection_lightTID = setInterval(toggleConnectionLight, 1000)
			//Non-visual elements
			connection_type = "ws";
			if(typeof ws_connection == "undefined")
			{
				try
				{
					ws_connection = new WebSocket("wss://"+server_domain+"/wsp/", 'janus-protocol');
					ws_connection.addEventListener('open', function(e){
						//Connected to the server, but we still need a session
						clearInterval(connection_lightTID);
						light.style.opacity = 1;
						light.title = "Connected to MCU. Creating application session...";
						//keepaliveTID = setInterval(keepalive_timeout, 45000);
						var msg = {
							"janus":"create",
							"transaction":"createsession"
						}
						ws_connection.send(JSON.stringify(msg));
					});
					ws_connection.addEventListener('close', ws_closed);
					ws_connection.addEventListener('message', ws_incoming_message);
					ws_connection.addEventListener('error', function(e){
						clearInterval(connection_lightTID);
						set_status_light("red", "Connection to MCU failed");
						console.error("Error while establishing websocket connection", e);
					});
				}
				catch(e)
				{
					console.error("Error opening WebSocket connection", e);
				}
			}
			else if(typeof session_id == "undefined")
			{
				//TODO - set status light stuff
				var msg = {
					"janus":"create",
					"transaction":"createsession"
				}
				ws_connection.send(JSON.stringify(msg));
			}
		}
		function http_connect(e)
		{
			//Visual elements
			var button = e.currentTarget;
			button.disabled = true;
			button.innerText = "Connecting...";
			light.style.visibility = "visible";
			light.style.opacity = 1;
			set_status_light("yellow", "Connecting to MCU...");
			connection_lightTID = setInterval(toggleConnectionLight, 1000)
			//Non-visual elements
			connection_type = "http";
			/*var xhr = new XMLHttpRequest();
			xhr.open("POST", "https://"+server_domain+":"+http_port+"/janus");
			xhr.onreadystatechange = function()
			{
				if(xhr.readyState === XMLHttpRequest.DONE)
				{
					if(xhr.status !== 200)
					{
						console.error("Error contacting server. XHR status code: "+xhr.status);
						return;
					}
					var response = JSON.parse(xhr.responseText);
					if(response.janus !== "success")
					{
						console.error("Error creating session.");
						console.error(response.error);
						return;
					}
					session_id = response.data.id;
				}
			}
			xhr.send(null);*/
		}
		/*FIXME - Ideally this function wouldn't exist and the peerconnection would only be created in
		server_audio_start, but Janus currently can't handle SDP renegotations (see these: 
			https://github.com/meetecho/janus-gateway/pull/753#issuecomment-296238409
			https://groups.google.com/forum/#!msg/meetecho-janus/Plvflko6DpY/fczA7NutCAAJ;context-place=msg/meetecho-janus/PGfjsSWiKg0/sOX2ZmB4BAAJ
		), so the act of enabling the microphone means the peerconnection has to be destroyed and re-created*/
		
		//Audio Init
		function server_audio_start()
		{
			log(LOG.DBG, "Starting server audio");
			if(typeof server_connection != "undefined")
			{
				console.error("PeerConnection has already been created");
				return;
			}
			//Create PeerConnection
				server_connection = new RTCPeerConnection({
					iceServers: [
						{
							urls: "turn:"+server_domain,
							username: "streamLobby",
							credential: "KI8zW3PZ9YXaQdHjMBmP"
						}
					]
				});
				//server_connection.onnegotationneeded = start_negotation;//The provided client framework doesn't seem to implement anything for this event
				server_connection.onaddstream = remote_stream_added;// Deprecated in favor of ontrack() I believe
				server_connection.onicecandidate = handle_ice_candidate;
				//server_connection.ontrack = remote_track_added;	// Recieved when a new incoming MediaStreamTrack has been created and associated with an RTCRtpReceiver object
				/*server_connection.onconnectionstatechange = ;		Recieved when the aggregate state of the connection changes
				server_connection.ondatachannel = ;					Recieved when an RTCDataChannel is added to the connection by the remote peer calling createDataChannel()
				server_connection.oniceconnectionstatechange = ;	Recieved when the state of the connection's ICE agent, as represented by the iceConnectionState property, changes
				server_connection.onicegatheringstatechange = ;		Recieved when the ICE agent starts/stops actively gathering candidates
				server_connection.onidentityresult = ;				Recieved when an identity assertion is generated or during the creation of an offer or an answer
				server_connection.onidpassertionerror = ;			Recieved when the associated identity provider (IdP) encounters an error while generating an identity assertion
				server_connection.onidpvalidationerror = ;			Recieved when the associated identity provider (IdP) encounters an error while validating an identity assertion
				server_connection.onpeeridentity = ;				Recieved when an identity assertion has been successfully validated
				server_connection.onremovestream = ;				Recieved when the value of RTCPeerConnection.signalingState changes (caused by setLocalDescription() or setRemoteDescription())
				server_connection.onsignalstatechange = ;			Recieved when the value of RTCPeerConnection.signalingState changes (caused by setLocalDescription() or setRemoteDescription())*/

			//Microphone
			navigator.mediaDevices.getUserMedia({audio: true, video: false}).then(function(localStream)
			{
				server_connection.addStream(localStream);
				//localStream.getTracks().forEach( track => {if(track.kind == "audio") server_connection.addTrack(track, localStream);} );
				//microphone_on = true;
				/*var button = document.getElementById("microphone_button");
				button.innerText = "Turn off microphone";
				button.onclick = local_audio_stop;*/
				log(LOG.NORMAL, "Microphone turned on and connected to PeerConnection object");
				//Initiate SDP
				send_message("request_sdp_offer", {audio:1, video:0});
			}).catch(function(e)
			{
				console.error("Caught exception enabling microphone", e);
				send_message("request_sdp_offer", {audio:1, video:0});
			});
		}
		/*function local_audio_start()
		{
			if(typeof server_connection == "undefined")
			{
				console.error("Attempt to start local audio feed before server connection has been made");
				return;
			}
			if(typeof audio_element == "undefined")
			{
				console.error("Cannot start microphone until we start receiving server audio");
				return;
			}

			/*FIXME - Currently the PeerConnection is being destroyed and recreated because janus doesn't support
			SDP renegotations. When support does get worked in, destroying the PeerConnection won't be necessary,
			I can just call getUserMedia and add the track to the PeerConnection* /
			audio_element.srcObject = null;
			server_connection.close();
			server_connection = new RTCPeerConnection({
				iceServers: [
					{
						urls: "turn:"+server_domain,
						username: "streamLobby",
						credential: "KI8zW3PZ9YXaQdHjMBmP"
					}
				]
			});
			server_connection.onicecandidate = handle_ice_candidate;
			server_connection.ontrack = remote_track_added;
			
			navigator.mediaDevices.getUserMedia({audio: true, video: false}).then(function(localStream)
			{
				localStream.getTracks().forEach( track => {if(track.kind == "audio") server_connection.addTrack(track, localStream);} );
				microphone_on = true;
				var button = document.getElementById("microphone_button");
				button.innerText = "Turn off microphone";
				button.onclick = local_audio_stop;
				log(LOG.NORMAL, "Microphone turned on and connected to PeerConnection object");
				send_message("request_sdp_offer", {audio:1, video:0});
				//Make the offer send only
				/*var c = {
					"offerToReceiveAudio":false,
					"offerToReceiveVideo":false,
					"mandatory": {
						"OfferToReceiveAudio":false,
						"OfferToReceiveVideo":false
					}
				}
				server_connection.createOffer(c).then(function(offer){
					log(LOG.DBG, "SDP offer for Microphone:");
					log(LOG.DBG, offer.sdp);
					return server_connection.setLocalDescription(offer);
				}).then(function(){
					send_message("sdp_pass", server_connection.localDescription);
				}).catch(function(e){
					console.error("Caught exception while creating SDP offer for outgoing PeerConnection", e);
				});* /
			}).catch(function(e)
			{
				console.error("Caught exception enabling microphone", e);
			});
		}
		
		//Video Init
		function server_video_start()
		{
			if(typeof server_connection == "undefined")
			{
				console.error("Attempt to start video feed before server connection has been made");
				return;
			}
			if(typeof video_element != "undefined")
			{
				console.error("Video element has already been created");
				return;
			}

			video_element = new Video();
			video_element.id = "video";
			send_message("request_sdp_offer", {audio:Number(typeof audio_element != "undefined"), video:1});
		}*/
	//END Initialization stuff
	
	
	//Destruction stuff
		function shutdown()
		{
			shutting_down = true;
			server_audio_stop();

			if(typeof current_room != "undefined")
				send_message("leave_room");
			
			if(typeof handle_id != "undefined")
			{
				send_message("destroy_plugin");
				handle_id = undefined;
			}
			//Destroy session handle
			if(typeof session_id != "undefined")
			{
				send_message("destroy_session");
				session_id = undefined;
			}
			//Close WebSocket connection
			if(connection_type == "ws")
				ws_connection.close();

			document_cleanup();
		}
		function server_audio_stop()
		{
			log(LOG.DBG, "server_audio_stop() start");
			if(typeof server_connection == "undefined")
				return;
			/*if(!shutting_down)
			{
				var button = document.getElementById("voice_chat_button");
				button.disabled = true;
				var button = document.getElementById("microphone_button");
				if(button)
					button.disabled = true;
				var button = document.getElementById("video_button");
				if(button)
					button.disabled = true;
			}

			if(microphone_on)
				local_audio_stop();
			if(typeof video_element != "undefined")
				server_video_stop();*/

			log(LOG.VERB, "Stopping server audio");
			send_message("hangup");
			var streams = server_connection.getLocalStreams();
			for(var i in streams)
			{
				streams[i].getTracks().forEach( s => s.stop() );
			}
			streams = server_connection.getRemoteStreams();
			for(var i in streams)
			{
				streams[i].getTracks().forEach( s => s.stop() );
			}
			/*var senders = server_connection.getSenders();
			senders.forEach( r => {if(r.track.kind == "audio") r.track.stop();}  );
			var receivers = server_connection.getReceivers();
			receivers.forEach( r => {if(r.track.kind == "audio") r.track.stop();} );*/
			while(source_nodes.length > 0)
			{
				source_nodes[0].disconnect();
				source_nodes.shift();
			}
			while(processor_nodes.length > 0)
			{
				processor_nodes[0].disconnect();
				processor_nodes.shift();
			}
			if(audio_mode == "html5")
			{
				audio_element.srcObject = null;
				audio_element = undefined;
			}
			else
			{
				audio_context.close();
				audio_context = undefined;
			}
			server_connection.close();
			server_connection = undefined;
		}
		/*function local_audio_stop()
		{
			log(LOG.DBG, "local_audio_stop() start");
			if(!microphone_on)
				return;
			log(LOG.VERB, "Shutting down microphone");
			var senders = server_connection.getSenders();
			senders.forEach(s => s.track.stop());
			microphone_on = false;
			if(!shutting_down)
			{
				var button = document.getElementById("microphone_button");
				button.innerText = "Turn on microphone";
				button.onclick = local_audio_start;
			}
		}
		function server_video_stop()
		{
			log(LOG.DBG, "server_video_stop() start");
			if(typeof video_element == "undefined")
				return;

			log(LOG.VERB, "Shutting down video");
			var receivers = server_connection.getReceivers();
			receivers.forEach( r => {if(r.track.kind == "video") r.track.stop();} );
			video_element.srcObject = null;
			video_element = undefined;
			if(!shutting_down)
			{
				var button = document.getElementById("video_button");
				button.innerText = "Turn on video";
				button.onclick = server_video_start;
			}
		}*/
		//Reset the document back to it's original state
		function document_cleanup()
		{
			//Connect button
			var thing = document.getElementById("server_connection_button");
			thing.innerText = "Connect";
			thing.onclick = connect_to_server;
			thing.disabled = false;
			//Status light
			set_status_light("off");
			//Page content
			clear_page();
			return;
		}
		function handle_server_crash()
		{
			shutting_down = 1;
			server_audio_stop();
			document_cleanup();
		}
	//END Destruction stuff
	
	
	//Message handling
	/*	Data object structure:
	{
		"status": "ok" | "error",
		"error_code": <int> (not present if status is 'ok'),
		"error_message": <string> (not present if status is 'ok'),
		"stuff": <varies> (not present if there's no data to return)
	}*/
		function handle_plugin_message(message)
		{
			var transaction = message.transaction;
			var data = message.plugindata.data;
			log(LOG.VERB, "\""+transaction+"\" Message from server");
			log(LOG.DBG, data);
			switch(transaction)
			{
				//Client commands
				case "list_rooms":
					if(data.status == "ok")
					{
						//Clear the room list
						var roomsTable = document.getElementById("room_list_body");
						while(roomsTable.firstChild)
							roomsTable.removeChild(roomsTable.firstChild);
						
						//Repopulate room list
						rooms = data.stuff;
						var i = 0;
						log(LOG.DBG, "Populating room list");
						for (var j in rooms)
						{
							log(LOG.DBG, rooms[j]);
							var newRow = document.createElement("tr");
							newRow.className = "room_list_item";
							newRow.innerHTML = "<td class='room_list_property' title='name'>"+rooms[j].name+"</td>"+
							"<td class='room_list_property' title='subject'>"+rooms[j].subject+"</td>"+
							"<td class='room_list_property' title='description'>"+rooms[j].description+"</td>"+
							"<td class='room_list_property' title='audio'><img class=\"room_list_prop_img\" src=\""+(rooms[j].audio_enabled ? "img/green.png" : "img/red.png")+"\"></td>"+
							"<td class='room_list_property' title='video'><img class=\"room_list_prop_img\" src=\""+(rooms[j].video_enabled ? "img/green.png" : "img/red.png")+"\"></td>"+
							"<td class='room_list_property' title='max_clients'>"+rooms[j].max_clients+"</td>"+
							"<td class='room_list_property' title='connected_clients'>"+rooms[j].connected_clients+"</td>";
							if(rooms[j].connected_clients < rooms[j].max_clients)
								newRow.innerHTML += "<td class='room_list_connect' title='"+i+"'>Connect</td>";
							roomsTable.appendChild(newRow);
							i++;
						}
						var buttons = document.getElementsByClassName("room_list_connect");
						for(var b in buttons)
						{
							if(typeof buttons[b].addEventListener != "undefined")
								buttons[b].addEventListener("click", connect_to_room);
						}
					}
					else
					{
						console.error("The server experienced some kind of error. Could not acquire room list");
						//TODO - Change GUI to reflect this (don't just clear the current room list though)
					}
				break;
				
				case "join_room":
					if(data.status == "ok")
					{
						current_room = rooms[waiting_to_join];
						build_page("lobby_page");
						if(current_room.audio_enabled)
							server_audio_start();
					}
					else
					{
						console.error("Error attempting to join lobby");
					}
					waiting_to_join = -1;
				break;

				case "leave_room":
					if(data.status == "ok")
					{
						current_room = undefined;
						if(!shutting_down)
						{
							build_page("lobby_listing");
							send_message("list_rooms");
						}
					}
					else
					{
						console.error("Error attempting to leave lobby");
					}
				break;
				
				case "list_peers":
				break;
				
				/*case 'ice_trickle':
					if(data.status == "ok")
					{
						var candidate = new RTCIceCandidate(msg./*candidate* /);
						server_connection.addIceCandidate(candidate).catch();
					}
					else
					{
						console.error("Error with ICE stuff");
					}
				break;*/
				
				case "sdp_pass":
					if(data.status == "error")
					{
						console.error("Server error processing SDP: "+data.error_code);
					}
					else
					{
						log(LOG.VERB, "Server successfully processed SDP");
					}
				break;

				case "request_sdp_offer":
					if(data.status == "error")
					{
						console.error("Server error creating SDP offer: "+data.error_code);
					}
				break;

				case "sdp_offer":
					log(LOG.VERB, "Server's SDP offer:");
					log(LOG.VERB, message.jsep);
					server_connection.setRemoteDescription(message.jsep).then(function(){
						//FIXME - Need to change the ReceiveVideo properties depending on whether this is the original negotation or a renegotation
						//FIXME - Need a way to know if this is an initial negotation or a renegotation
						var c = {
							"offerToReceiveAudio":true,
							"offerToReceiveVideo":current_room.video_enabled,
							"mandatory": {
								"OfferToReceiveAudio":true,
								"OfferToReceiveVideo":current_room.video_enabled
							}
						}
						return server_connection.createAnswer(c);
					}).then(function(answer) {
						log(LOG.VERB, "our SDP answer:");
						log(LOG.VERB, answer);
						return server_connection.setLocalDescription(answer);
					}).then(function() {
						send_message("sdp_pass", server_connection.localDescription);
					}).catch(function(e){
						console.error("Caught exception establishing PeerConnection with gateway", e);
					});
				break;

				case "sdp_answer":
					log(LOG.VERB, "Server's SDP response:");
					log(LOG.VERB, message.jsep);
					server_connection.setRemoteDescription(message.jsep).catch(function(e){
						console.error("Caught exception setting remote description for outgoing PeerConnection", e);
					});
				break;
				
				
				//Server events
				/*case "video_stream_started":
				break;
				case "video_stream_stopped":
				break;*/
			}
		}
		function handle_janus_message(message)
		{
			switch(message.janus)
			{
				case "success":
					if(message.transaction == "createsession")
					{
						session_id = message.data.id;
						keepaliveTID = setInterval(keepalive_timeout, 45000);
						//Session has been created, now to make a plugin handle
						var msg = {
							"janus":"attach",
							"plugin":"plugin.streamlobby",
							"transaction":"pluginattach",
							"session_id":session_id
						}
						ws_connection.send(JSON.stringify(msg));
					}
					else if(message.transaction == "pluginattach")
					{
						handle_id = message.data.id;
						set_status_light("green", "Connected to server");
						build_page("lobby_listing");
						send_message("list_rooms");
					}
					else if(message.transaction == "keepalive")
					{
						log(LOG.VERB, "received keepalive response");
					}
					else if(message.transaction == "hangup")
					{
						//Server audio has been stopped, now change the GUI
						/*if(!shutting_down)
						{
							var button = document.getElementById("voice_chat_button");
							button.innerText = "Turn on audio";
							button.onclick = server_audio_start;
							button.disabled = false;
							button = document.getElementById("microphone_button");
							if(button)
								button.disabled = true;
							button = document.getElementById("video_button");
							if(button)
								button.disabled = true;
						}*/
					}
					else
					{
						console.error("Unknown success message from gateway\nTransaction: "+message.transaction);
					}
				break;
				
				/*The plugin is the one that sends this message, not the gateway*/
				case "sdp_response":
					if(message.status == "ok")
					{
						var remoteSettings = new RTCSessionDescription(message.sdp);
						//var remoteSettings = new RTCSessionDescription(JSON.parse(message.sdp));
						server_connection.setRemoteDescription(remoteSettings);
					}
					else
					{
						console.error("Error with SDP negotiation");
					}
				break;
				
				case "webrtcup":
					log(LOG.NORMAL, "WebRTC is up and running");
					/*var button = document.getElementById("voice_chat_button");
					button.innerText = "Turn off audio";
					button.onclick = server_audio_stop;
					button = document.getElementById("microphone_button");
					if(button)
						button.disabled = false;*/
				break;
				
				case "media":
					if(message.receiving)
						log(LOG.NORMAL, "The server is now recieving your "+message.type+" stream");
					else
						log(LOG.NORMAL, "The server has lost your "+message.type+" stream");
				break;
				
				case "slowlink":
					console.warn("slowlink notice recieved\nNACK count: "+message.nacks);
				break;
				
				case "hangup":
					log(LOG.NORMAL, "PeerConnection has been closed\nReason:"+message.reason);
					server_connection = undefined;
				break;
				
				case "keepalive":
					log(LOG.NORMAL, "Server sent keepalive");
				break;
				
				case "ack":
					log(LOG.NORMAL, "Recieved ack message from server");
				break;
				
				case "event":
					log(LOG.NORMAL, "Recieved event message from server");
				break;

				case "timeout":
					log(LOG.NORMAL, "Session timed out");
					clearInterval(keepaliveTID);
					keepaliveTID = -1;
					session_id = undefined;
					//FIXME - At this point, the websocket connection is still open
					shutdown();
				break;
				
				case "error":
					if(message.transaction == "createsession")
					{
						console.error("Error creating session", message.error);
						set_status_light("red", "Error creating session");
						if(connection_type == "ws")
							ws_connection.close();
					}
					else if(message.transaction == "pluginattach")
					{
						console.error("Gateway error attaching to plugin", message.error);
						set_status_light("red", "Error attaching to plugin");
						var button = document.getElementById("server_connection_button");
						button.innerText = "Connect";
						button.disabled = false;
						if(connection_type == "ws")
							ws_connection.close();
					}
					else if(message.transaction == "keepalive")
					{
						console.error("Error with keepalive request", message.error);
					}
					else
					{
						console.error("Unknown error\nTransaction: "+message.transaction);
					}
				break;
				
				default:
					console.error("Unknown message from server", message);
				break;
			}
		}
		function send_message(request, stuff)
		{
			if(typeof session_id == "undefined" || typeof handle_id == "undefined")
				return;
			var msg = {
				"janus":"message",
				"transaction":"",
				"body": {
					"request":""
				}
			}
			switch(request)
			{
				case "join_room":
					msg.transaction = request;
					msg.body.room = stuff;
					msg.body.request = user_commands[request];
				break;

				case "leave_room":
					msg.transaction = request;
					msg.body.request = user_commands[request];
				break;
				
				case "sdp_pass":
					msg.transaction = request;
					msg.jsep = stuff;
					msg.body.request = user_commands[request];
				break;
				
				case "request_sdp_offer":
					msg.transaction = request;
					msg.body.request = user_commands[request];
					msg.body.audio = stuff.audio;
					msg.body.video = stuff.video;
				break;
				
				case "ice_trickle":
					msg.janus = "trickle";
					msg.transaction = request;
					msg.candidate = stuff;
				break;
				
				case "ice_finished":
					msg.janus = "trickle";
					msg.transaction = request;
					msg.candidate = {"completed":true}
				break;
				
				case "list_peers":
				case "list_rooms":
					msg.transaction = request;
					msg.body.request = user_commands[request];
				break;
				
				case "destroy_plugin":
					msg.janus = "detach";
					msg.transaction = request;
				break;
				
				case "destroy_session":
					msg.janus = "destroy";
					msg.transaction = request;
				break;

				//Used to shut down the peer connection entirely
				case "hangup":
					msg.janus = request;
					msg.transaction = request;
				break;
				
				default:
					console.error("Unknown request");
					return;
				break;
			}
			if(connection_type == "ws")
			{
				msg.session_id = session_id;
				if(typeof handle_id != "undefined")
					msg.handle_id = handle_id;
				log(LOG.DBG, "Sending message");
				log(LOG.DBG, msg);
				if(typeof ws_connection != "undefined")
					ws_connection.send(JSON.stringify(msg));
			}
			/*else
			{
				var xhr = XMLHttpRequest();
				xhr.open("POST", "https://"+server_domain+":"+http_port+"/janus/"+session_id+"/"+handle_id);
				xhr.send(null);
			}*/
		}
	//END Message handling
	
	
	
	//Callback and Event Handling functions
	//Connect button
	function connect_to_server(e)
	{
		try
		{
			if(typeof WebSocket != "undefined")
				ws_connect(e);
			else
				http_connect(e);
		}
		catch(e)
		{
			set_status_light("red", "Error connecting to server");
			clearInterval(connection_lightTID);
			var button = document.getElementById("server_connection_button");
			button.innerText = "Connect";
			button.disabled = false;
			alert("Unexpected error connecting to server");
			console.error("Unexpected error connecting to server", e);
		}
	}
	//WebSocket connection
	function ws_closed(e)
	{
		clearInterval(keepaliveTID);
		log(LOG.NORMAL, "WebSocket connection closed (Code "+e.code+": "+ws_close_codes[e.code]+")");
		ws_connection = undefined;
		/*Status code 1006 indicates one of the following:
			* Tried connecting to an invalid URl
			* Janus got shut down while the connection was open
		*/
		if(e.code == 1006)
		{
			if(typeof server_connection != "undfeined")
				handle_server_crash();
			else
				document_cleanup();
		}
	}
	function ws_incoming_message(e)
	{
		var message = JSON.parse(e.data);
		log(LOG.VERB, "Got message on WebSocket connection");
		log(LOG.DBG, message);
		if(message.janus == "message" || message.transaction && message.transaction in user_commands)
		{
			log(LOG.DBG, "Handing off to handle_plugin_message");
			handle_plugin_message(message);
		}
		else
		{
			log(LOG.DBG, "Handing off to handle_janus_message");
			handle_janus_message(message);
		}
	}
	//PeerConnection
	/*function start_negotation(e)
	{
		log(LOG.DBG, "Recieved start_negotation event");
	}*/
	function handle_ice_candidate(e)
	{
		log(LOG.DBG, "Recieved ice_candidate event");
		//the endOfCandidates field seems to be specific to Edge
		if(e.candidate == null || typeof e.candidate.candidate != "undefined" && e.candidate.candidate.indexOf("endOfCandidates") > 0)
		{
			send_message("ice_finished");
		}
		else
		{
			//var candidate = new RTCIceCandidate(e.candidate);
			//server_connection.addIceCandidate(candidate).then(function(){
				send_message("ice_trickle", e.candidate);
			//});
		}
	}
	function remote_stream_added(e)
	{
		//TODO - figure out if it's a video or audio stream
		log(LOG.NORMAL, "Remote stream added to PeerConnection object");
		if(audio_mode == "web_audio")
		{
			setTimeout(function(e) {
				try {
				var node = audio_context.createMediaStreamSource(e.stream);
				source_nodes.push(node);
				var pnode = audio_context.createScriptProcessor(0, 1, 1);
				processor_nodes.push(pnode);
				pnode.onaudioprocess = function(e) {
					var inB = e.inputBuffer;
					var outB = e.outputBuffer;
					for(var ch = 0; ch < outB.numberOfChannels; ch++)
					{
						var inData = inB.getChannelData(ch);
						var outData = outB.getChannelData(ch);
						log(LOG.OGODWTF, "Input buffer");
						log(LOG.OGODWTF, inData);
						for(var i = 0; i < inB.length; i++)
						{
							outData[i] = inData[i];
						}
					}
				}
				node.connect(pnode);
				var gnode = audio_context.createGain();
				gnode.gain.value = 1;
				gain_nodes.push(gnode);
				//pnode.connect(audio_context.destination);
				pnode.connect(gnode);
				gnode.connect(audio_context.destination);
				log(LOG.NORMAL, "Remote stream added to WebAudio context");
				} catch(ex) {
					console.error("Caught error adding remote stream to audio context.", ex);
				}
			}, 1500, e);
		}
		else if(audio_mode == "html5")
		{
			audio_element.srcObject = e.stream;
		}
	}
	/*function remote_track_added(e)
	{
		log(LOG.DBG, "Recieved ontrack event");
		//TODO - figure out if it's a video or audio track
		audio_element.srcObject = e.streams[0];
	}*/
	//GUI clicks
	function connect_to_room(e)
	{
		if(waiting_to_join != -1)
			return;
		var offset = e.currentTarget.title;
		waiting_to_join = offset;
		var roomname = rooms[offset].name;
		send_message("join_room", roomname);
	}
	function leave_room(e)
	{
		if(audio_mode == "html5" && typeof audio_element != "undefined")
			server_audio_stop();
		else if(audio_mode == "web_audio" && typeof audio_context != "undefined")
			server_audio_stop();
		build_page("lobby_listing");
		send_message("leave_room");
	}
	
	
	
	function setGain(gain)
	{
		gain_nodes.forEach( gnode => gnode.gain.value = gain);
	}
	
	
	//Misc functions
	function log(target_lv, stuff)
	{
		if(target_lv <= log_level)
			console.log(stuff);
	}
	function set_log_level(new_lv)
	{
		if(new_lv > -1 && new_lv < 4)
			log_level = new_lv;
	}
	function set_status_light(color, div_title)
	{
		if(current_status_light != null)
			current_status_light.style.visibility = "hidden";
		switch(color)
		{
			case "red":
			case "yellow":
			case "green":
				current_status_light = document.getElementById("connection_"+color);
				current_status_light.style.visibility = "visible";
			break;
			case "off":
				current_status_light = null;
			break;
		}
		if(typeof div_title == "string" && div_title != "")
			light.title = div_title;
	}
	function build_page(page)
	{
		clear_page();
		switch(page)
		{
			case "lobby_listing":
				//Change connect button to Disconnect
				var button = document.getElementById("server_connection_button");
				button.innerText = "Disconnect";
				button.disabled = false;
				button.onclick = shutdown;
				//Add lobby printout button
				var newButton = document.createElement("button");
				newButton.onclick = function() {
					send_message("list_rooms");
				}
				newButton.innerText = "Refresh";
				newButton.id = "refresh_button";
				page_content.appendChild(newButton);
				//Add table to house lobby information
				var roomsTable = document.createElement("table");
				roomsTable.id = "room_list";
				roomsTable.innerHTML = "<thead id=\"room_list_header\"><tr>"+
				"<th>Room Name</th>"+
				"<th>Subject</th>"+
				"<th>Description</th>"+
				"<th>Audio</th>"+
				"<th>Video</th>"+
				"<th>Max Clients</th>"+
				"<th>Connected Clients</th>"+
				"</tr></thead><tbody id=\"room_list_body\"></tbody>";
				page_content.appendChild(roomsTable);
			break;

			case "lobby_page":
				//Add button to leave the room
				var newButton = document.createElement("button");
				newButton.id = "leave_room_button";
				newButton.onclick = leave_room;
				newButton.innerText = "Leave Lobby";
				page_content.appendChild(newButton);
				if(current_room.audio_enabled)
				{
					//Create audio element
					if(audio_mode == "html5")
					{
						audio_element = new Audio();
						audio_element.id = "audio";
						audio_element.autoplay = true;
					}
					else if(audio_mode == "web_audio")
					{
						audio_context = new AudioContext();
					}
				}
				if(current_room.video_enabled)
				{
					//Add button for turning on video
					var newButton = document.createElement("button");
					newButton.id = "video_button";
					newButton.innerText = "Turn on video";
					if(current_room.video_active)
						newButton.disabled = false;
					else
						newButton.disabled = true;
					page_content.appendChild(newButton);
				}
			break;

			default:
				console.error("Unknown page name \""+page+"\"");
			break;
		}
	}
	//Remove page specific elements from the document
	function clear_page()
	{
		while(page_content.firstChild)
			page_content.removeChild(page_content.firstChild);
	}
	
	
	
	
	//Timeout/Interval functions
	function keepalive_timeout()
	{
		ws_connection.send(JSON.stringify({"janus":"keepalive","session_id":session_id,"transaction":"keepalive"}));
	}
	function toggleConnectionLight()
	{
		if(light.style.opacity == .01)
			light.style.opacity = 1;
		else
			light.style.opacity = .01;
	}
//})();
