# Vendored Hotspot Arcade engine

The files in `vendor/engine/` are exact copies of:

- `esp32/hotspot-arcade-fw/ha_games.h`
- `esp32/hotspot-arcade-fw/ha_json.h`
- `esp32/hotspot-arcade-fw/ha_proto.h`

Source: <https://github.com/tarikbc/hotspot-arcade>

Upstream revision: `95a6af262bb629facbfb603e0f21609517458d21`

SHA-256:

- `ha_games.h`: `d016915366a2cc2122997eca6a676e0fe987e5b1e52aa0d7136ad043a3e10f8b`
- `ha_json.h`: `09b13a637dde09233dde4d5b4d4507893ee0508aea022523164edda052fb446d`
- `ha_proto.h`: `db92f3486c59ef5b9c0dfe06921a074be86dfaf6cd71198ce5466ea64fdbd4f5`

The engine is MIT licensed. Its unmodified license is in `LICENSE.upstream`.
`compat/Arduino.h` is an ESP-IDF adaptation of upstream's MIT-licensed
`sim/engine/Arduino.h`; it deliberately implements only the API used by the
engine and does not depend on Arduino Core.
