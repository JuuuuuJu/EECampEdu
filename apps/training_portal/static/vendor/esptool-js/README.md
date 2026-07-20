# Vendored dependency — esptool-js

`bundle.js` is Espressif's official browser flasher **esptool-js**, vendored so the
portal can flash the ESP32-S3 over the Web Serial API with **no CDN at runtime**
(the classroom AI PC is offline for students).

| | |
|---|---|
| Package | [`esptool-js`](https://github.com/espressif/esptool-js) |
| Version | **0.5.4** (pinned) |
| Source | `https://cdn.jsdelivr.net/npm/esptool-js@0.5.4/bundle.js` |
| File | `bundle.js` (ES module; bundles `pako` for flash compression) |
| SHA-256 | `2a896d5e520ea9b6ea9900223c2460df797c3b2287f441daa4e50168a50b17e6` |
| Exports | `ESPLoader`, `Transport` (used by the portal) |
| License | Apache-2.0 (Espressif) |

The portal loads it same-origin via a dynamic `import("/static/vendor/esptool-js/bundle.js")`
from `templates/index.html`. It is a committed dependency (not generated), so it
is intentionally **not** git-ignored.

## Updating

```bash
curl -L "https://cdn.jsdelivr.net/npm/esptool-js@<version>/bundle.js" \
  -o apps/training_portal/static/vendor/esptool-js/bundle.js
sha256sum apps/training_portal/static/vendor/esptool-js/bundle.js   # update the table above
```

Verify the exports still include `ESPLoader` and `Transport`, and that the
`writeFlash({fileArray, flashSize, flashMode, flashFreq, eraseAll, compress, reportProgress})`
option shape is unchanged.
