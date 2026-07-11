# Input Interface Reference

Reference sources:

```text
D:\0711_integration\esp32Cam_app
CDC/MSC input-interface team reference project
```

Integrated code:

```text
apps/esp32_cam_input_app
esp/main/src/usb_composite.cpp
esp/main/src/app_main.cpp
```

Important findings:

- `esp32Cam_app` is integrated as a Dear ImGui + SDL3 desktop UI under top-level `apps/`.
- Current UI includes the ImGui demo window, camera/input controls, and a Windows COM-port CDC backend for ESP commands.
- It does not yet send TinyUSB CDC commands to ESP32-S3.
- Firmware already supports TinyUSB CDC + MSC, CDC base64 frame streaming, and camera commands.

Deploy integration target:

```text
PC Dear ImGui control
  -> TinyUSB CDC command
  -> ESP camera / USB / inference action
  -> CDC response or frame stream
```

The protocol draft is in `interfaces/usb_protocol.md`.

