# External Source References

These folders are staging areas for other teams' code. Do not edit the original
source folders directly. Copy only the parts that are intentionally integrated.

| Team / Source | Original Path | Current Use |
| --- | --- | --- |
| deploy baseline | previous standalone deploy repo | copied into `esp/` and `pc/benchmark` |
| camera | OV2640 reference project | reference for OV2640 capture and format conversion |
| camera/USB | `D:\0711_integration\camera_usb\EECampEdu` | source of truth for TinyUSB CDC image streaming and MSC storage behavior |
| input-interface UI | `D:\0711_integration\esp32Cam_app` | integrated into top-level `apps/esp32_cam_input_app` as a Dear ImGui + SDL3 desktop UI with CDC command controls |
| model-finetune | `../model_finetune` | source models, datasets, and training code |
| output | `output/robotic_arm.ino` | reference for robotic-arm servo movement |

## Integration Rule

External folders are references, not the final architecture. Production code
should be moved into `esp/main/include`, `esp/main/src`, `pc/`, or `interfaces/`
with a clear contract.


