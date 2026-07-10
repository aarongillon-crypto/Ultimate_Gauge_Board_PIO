// JSON API for the SPA. GET = read, POST = write (query args for small
// mutations, JSON bodies for themes). Handlers run in loopTask context.
#pragma once
#include <WebServer.h>

void api_register(WebServer& server);
