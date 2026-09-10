# Hotspot Arcade (ESP32-S3 FAP)

This is the ESP32-S3 host UI for [tarikbc/hotspot-arcade](https://github.com/tarikbc/hotspot-arcade). It preserves the Flipper host workflow—session control, game selection, live roster and scores, console, SSID, and host preferences—while using the S3 radio already inside this port.

## Architecture

The FAP is intentionally a thin, dynamically linked controller. It does not contain a second Wi-Fi stack or a board firmware image.

- `hotspot_arcade.fap` owns the GUI and persistent host settings.
- The firmware-resident `hotspot_arcade_service` owns the access point, HTTP/WebSocket server, game engine, clients, and pack loading.
- The service's ESP32-S3 runtime serves the compressed phone client directly from SD.
- The FAP polls a copied service snapshot every 500 ms on the ViewDispatcher thread. It does not register asynchronous callbacks, so service tasks never touch GUI objects.
- Closing the FAP synchronously stops the service before any UI or storage state is released.

There is deliberately no UART transport, external ESP board discovery, GPIO reset, expansion-module setup, ESP flasher, or bundled board firmware in this port.

## SD card layout

Install the tracked `assets/` contents at this exact location:

```text
/ext/apps_assets/hotspot_arcade/
├── web/
│   └── index.html.gz
└── packs/
    ├── trivia/
    ├── draw/
    └── ...
```

The firmware maps `/ext` to `/sdcard`; the service therefore receives these native paths:

```text
/sdcard/apps_assets/hotspot_arcade/web/index.html.gz
/sdcard/apps_assets/hotspot_arcade/packs
/sdcard/apps_data/hotspot_arcade/packs
```

User settings are stored at `/ext/apps_data/hotspot_arcade/config.txt`. Optional user-created packs belong in `/ext/apps_data/hotspot_arcade/packs/`.

The manifest declares `fap_file_assets="assets"` for compatible packaging tools. This port's standalone `buildFap.sh` currently compiles the FAP but does not install `fap_file_assets`, so copy the directory to the SD card separately. The repository's `sdcard/apps_assets/hotspot_arcade/` mirror is ready for the normal SD-card staging workflow.

## Build

First build firmware containing the `hotspot_arcade_service` component and its exported FAP API symbols. Then build the application from the repository root:

```sh
bash ./buildFap.sh applications_user/hotspot_arcade
```

For the T-Embed ESP32-S3 target, the result is written under `build_t_embed/fap/`. Copy the resulting FAP into the SD card's Games application directory and install the assets above.

## Controls

The UI uses only standard Up, Down, OK, and Back events. On T-Embed, rotary clockwise/counter-clockwise maps to Down/Up, rotary click maps to OK, and the side/back input maps to Back.

- **Start/Stop Session** validates the SD card and required web/packs assets before starting the S3 service.
- **Dashboard** shows the hotspot IP and SSID, selected game, player/phone counts, and last event. OK ends the current round.
- **Games** contains all 40 games, including the 20-game expansion documented in
  `docs/HOTSPOT_ARCADE_20_GAMES_PLAN.md`.
- **Leaderboard** sorts joined players by score; OK resets scores.
- **Console** shows a copied service log; OK refreshes it.
- **Set SSID** saves a 1–32 character hotspot name. A running session keeps its current name until restarted.
- **Settings** cycles English, Deutsch, and Portugues BR and toggles sound/vibration. Language changes take effect on the next session; feedback toggles are immediate.

## Service API assumptions

The FAP includes `<hotspot_arcade_service/hotspot_arcade_service.h>` and uses only its synchronous start/stop/control calls plus copied snapshots and console text. The service must keep every returned snapshot and console copy independent of service-owned storage, bump `snapshot.revision` whenever displayable state changes, and make `hotspot_arcade_service_stop()` wait until its workers and network resources are down.

The web client, content packs, and vendored engine originate from upstream revision `95a6af262bb629facbfb603e0f21609517458d21` and are MIT licensed. The upstream license is retained as `assets/LICENSE.hotspot-arcade`.
