#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Starts a monitor task that serves the UI whenever a network interface is up. */
void web_interface_start_when_network_ready(void);

#ifdef __cplusplus
}
#endif
