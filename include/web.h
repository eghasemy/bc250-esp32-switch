#pragma once

// Web UI and its JSON API, served from SPIFFS (app/index.html). Runs in both
// modes: in setup mode on the setup AP (the only place a first password can be
// created), in normal mode on the router network or the fallback AP.
// Every endpoint except /api/info, /api/login and first-password creation
// needs a session token (X-Auth-Token header).
void webBegin(bool setupMode);
void webLoop(unsigned long now);
