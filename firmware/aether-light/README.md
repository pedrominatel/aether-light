# Aether Light

Aether Light runs on the ESP32-P4 and exposes a local web interface for network
and addressable LED configuration. The interface is implemented by the local
`web_interface` component and provides separate Status, Network, LED Channels,
Configuration, and Reboot pages.

The Network page supports DHCP or a fixed IPv4 address shared by the active
Ethernet or fallback WiFi interface. Fixed address, gateway, subnet mask, and
DNS settings are validated, stored in NVS, and applied after restart. It can
also scan for nearby WiFi networks and use a selected SSID in the configuration
form.

## UltraLED configuration

UltraLED hardware settings are stored in NVS under a versioned application
schema. On first boot the LED output is disabled so that no GPIO is driven until
the hardware has been configured. The web page supports one to eight channels
with these settings:

- LED model shared by all channels
- data GPIO, pixel count, color order, and brightness for each channel
- 1 to 960 pixels per channel
- brightness from 0% (off) to 100%

Brightness is an output scaling setting, not an electrical current limiter. The
configured percentage must suit the power supply and wiring available to each
channel; actual current also depends on the number of illuminated pixels and
their colors.

Saving LED settings commits them to NVS and restarts the device. Invalid or
incompatible stored settings leave UltraLED inactive while the network and web
configuration page remain available for recovery. GPIOs used by the configured
Ethernet interface cannot be assigned to an LED channel.
