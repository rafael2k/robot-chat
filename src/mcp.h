#ifndef RC_MCP_H
#define RC_MCP_H

#include "conf.h"
#include "session.h"

#include <cJSON.h>

/* Run an MCP server on stdin/stdout until stdin closes. */
int mcp_run(conf_t const *c);

cJSON *event_to_json(event_t const *e);

#endif
