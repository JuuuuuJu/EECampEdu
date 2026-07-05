# External Source References

These folders are staging areas for other teams' code. Do not edit the original
source folders directly. Copy only the parts that are intentionally integrated.

| Team / Source | Original Path | Current Use |
| --- | --- | --- |
| deploy baseline | `D:\0703\EECampEdu_deploy` | copied into `esp/` and `pc/benchmark` |
| camera | `C:\Users\user\OneDrive\桌面\OV2640-main` | reference for OV2640 capture and format conversion |
| input-interface | `C:\Users\user\OneDrive\桌面\esp32_cdc_msc-main` | reference for TinyUSB CDC/MSC protocol |
| model-finetune | `D:\tensorshit` | reference models, datasets, training/quantization code |

## Integration Rule

External folders are references, not the final architecture. Production code
should be moved into `esp/main/include`, `esp/main/src`, `pc/`, or `interfaces/`
with a clear contract.
