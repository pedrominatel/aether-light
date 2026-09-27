# DDP component

This component implements the transport and wire-level parts of Distributed
Display Protocol version 1. It intentionally has no dependency on NVS, the web
interface, or a particular LED driver.

Supported behavior:

- UDP and TCP on the standard DDP port (4048 by default)
- 10-byte headers and optional 4-byte timecodes
- data, Push, Query, and Reply packets
- bounded payloads (1440 bytes by default)
- TCP stream fragmentation and multiple sequential packets
- back-to-back duplicate suppression and sequence-gap statistics
- callback-generated query replies

The application supplies callbacks for data, Push, and Query packets. Callback
data is valid only for the duration of the callback and must be copied if the
application needs to retain it.

The component owns no display framebuffer. Frame assembly, address mapping,
packet-loss policy, and persistent configuration belong to the application.

Use `DDP_SERVER_CONFIG_DEFAULT()` to initialize the server configuration, set
the desired transport flags, populate `ddp_callbacks_t`, and call
`ddp_server_start()`.

Callbacks execute in the DDP server task and must not retain packet pointers.
They should do bounded work and must not call `ddp_server_stop()` for the same
server.
