# Aether Light

Aether Light runs on the ESP32-P4-Function-EV-Board and the
ESP32-S31-Function-CoreBoard-1. It exposes a local web interface for network
and addressable LED configuration. The interface is implemented by the local
`web_interface` component and provides separate Status, Network, LED Channels,
DDP, Files, Configuration, and Reboot pages.

On ESP32-P4, the Files page mounts the board's microSD slot using SDMMC
and provides streamed file upload, download, and deletion. Files are stored in
the card root for later use by sequence playback. A missing or unreadable card
does not prevent the rest of the firmware from starting. The ESP32-S31
Function Coreboard has no onboard card socket, so the Files page reports that
storage is unsupported on that target. Its GPIO20-GPIO25 SDMMC header remains
available for future external-card support.

## Building a target

ESP-IDF 6.1 or newer is required. Keep separate build directories so target
configuration and generated artifacts cannot leak between builds:

```sh
idf.py -B build-esp32p4 -DIDF_TARGET=esp32p4 build
idf.py -B build-esp32s31 -DIDF_TARGET=esp32s31 build
```

Shared settings live in `sdkconfig.defaults`; ESP-IDF automatically adds the
matching `sdkconfig.defaults.<target>` file. The generated `sdkconfig` remains
untracked.

The Network page supports DHCP or a fixed IPv4 address shared by the active
Ethernet or fallback WiFi interface. Fixed address, gateway, subnet mask, and
DNS settings are validated, stored in NVS, and applied after restart. It can
also scan for nearby WiFi networks and use a selected SSID in the configuration
form.

## Firmware versioning

The authoritative firmware version is stored in `version.txt` using
`MAJOR.MINOR.PATCH` format. CMake rejects invalid versions and embeds the value
in the ESP-IDF application descriptor. Increment this file before producing a
release image; do not derive release versions from local Git state.

The running version, build metadata, and image SHA-256 are shown on the Status
page and are available as JSON from `GET /api/firmware`. The
`firmware_version` component also provides strict parsing and comparison helpers
for future OTA update eligibility checks. `secureVersion` is exposed separately
for future ESP32 anti-rollback policy and is not the semantic release version.

## UltraLED configuration

UltraLED hardware settings are stored in NVS under a versioned application
schema. Existing eight-channel settings are migrated when read. On first boot
the LED output is disabled so that no GPIO is driven until the hardware has
been configured. The web page supports one to sixteen channels
with these settings:

- LED model shared by all channels
- data GPIO, pixel count, color order, and brightness for each channel
- 1 to 960 pixels per channel
- brightness from 0% (off) to 100%, defaulting to 50% for new channel configurations

Data GPIOs are selected from a target-specific Kconfig allowlist. The P4
defaults are GPIO 2, 3, 4, 5, 6, 21, 22, 32, 14, 15, 16, 17, 18, 24, 25, and
33. The S31 defaults are GPIO 2, 3, 4, 35, 36, 37, 38, 39, 26, 27, 28, 30,
31, 32, 33, and 34. Each GPIO can be assigned to only one active channel.

Brightness is an output scaling setting, not an electrical current limiter. The
configured percentage must suit the power supply and wiring available to each
channel; actual current also depends on the number of illuminated pixels and
their colors.

Saving LED settings commits them to NVS and restarts the device. Invalid or
incompatible stored settings leave UltraLED inactive while the network and web
configuration page remain available for recovery. GPIOs used by the configured
Ethernet interface cannot be assigned to an LED channel.
