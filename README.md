Stream Lobby
=============
Basic VoIP plugin for [Janus MCU Gateway](https://janus.conf.meetecho.com/index.html) based on the sample audiobridge plugin.

Only voice chat is supported for now. Support for text chat and admin controlled video feeds will be added later. Before that however, testing needs to be done from a hosted server environment. Nearly all testing I have done so far has been on a local network with a minimal amount of testing using my residential internet connection. Audio was corrupted when clients accessed the server from outside networks, but this is possibly due to the poor upload speed/stability that comes with non-fiber US internet connections.

## Dependencies
* Janus and all its dependencies
* libuuid
* libopus
* libogg
