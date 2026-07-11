# External Source References

These folders are staging areas for other teams' code. Do not edit the original
source folders directly. Copy only the parts that are intentionally integrated.

| Team / Source | Original Path | Current Use |
| --- | --- | --- |
| deploy baseline | previous standalone deploy repo | copied into `esp/` and `pc/benchmark` |
| camera | OV2640 reference project | reference for OV2640 capture and format conversion |
| input-interface | CDC/MSC reference project | reference for TinyUSB CDC/MSC protocol |
| model-finetune | `../model_finetune` | source models, datasets, training/quantization code |

## Integration Rule

External folders are references, not the final architecture. Production code
should be moved into `esp/main/include`, `esp/main/src`, `pc/`, or `interfaces/`
with a clear contract.
