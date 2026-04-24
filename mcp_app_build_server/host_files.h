#ifndef MCP_HOST_FILES_H
#define MCP_HOST_FILES_H

#include <stdint.h>

void host_files_init(const char *apps_root);
void host_files_out(uint8_t port, uint8_t data);
uint8_t host_files_in(uint8_t port);

#endif
