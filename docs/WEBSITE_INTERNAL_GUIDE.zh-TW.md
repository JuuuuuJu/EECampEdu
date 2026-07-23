# AI PC 網站操作說明

這份給組內工作人員看。學生主要只需要開瀏覽器，不需要自己打 CLI。

## 入口

每組一台 AI PC：

- Team N 網站：`https://140.112.194.42:4430+N`
- Team N SSH：`ssh -p 220+N eecamp@140.112.194.42`

例：Team 1 是 `https://140.112.194.42:4431`，SSH 是 `ssh -p 221 eecamp@140.112.194.42`。

## 背景服務

網站服務：

```bash
systemctl --user restart eecamp-portal
systemctl --user status eecamp-portal
journalctl --user -u eecamp-portal -f
```

如果有使用 camera helper：

```bash
systemctl --user restart eecamp-camera-app
systemctl --user status eecamp-camera-app
journalctl --user -u eecamp-camera-app -f
```

改 `server.py`、`templates/index.html`、portal static assets、`deploy/eecamp-portal.env` 後要重開 `eecamp-portal`。

## 網站功能

| 頁面 | 用途 |
|---|---|
| Home | 首頁、repo 連結、營隊入口。 |
| Model finetune | 用 OV2640 拍 dataset、選 active class、train、quantize、preview model。 |
| Deploy & benchmark | 燒 benchmark firmware 和 `.tflite`，在瀏覽器跑 benchmark。 |
| Output demo | 讓學生改限定範圍內的 C code，AI PC build，成功才可以 flash。 |
| Main board firmware | 燒完整 main board firmware，連 camera，看 prediction，並連 control board 控制 servo。 |
| Camera + USB demo | 測 OV2640 preview、拍照、ESP flash、MSC。 |
| AI PC Drive | 這台 AI PC 上的小型檔案區。大量檔案請先 zip。 |

## Model 與 Firmware 的差異

- `.tflite` 是模型，直接寫進 model partition，不會轉成 `.bin`。
- `TFLiteGesture_esp.bin` 是 ESP-IDF 編出來的 main board app firmware，不是模型。
- Firmware flash 和 model flash 是兩件事。

## Confidence 與 Output

所有 prediction 若 confidence 低於 70%，一律視為 `null` / idle，不送有效動作給 control board。

Main board 頁面需要同時連：

1. ESP32-S3 main board
2. ESP32 control board

瀏覽器收到 main board prediction 後，依照 mapping 把 action 送到 control board，control board 再驅動 servo。

## Camera 快速排查

如果三個 camera firmware 都突然不能用，而且 log 出現 `ESP_ERR_NOT_SUPPORTED` 或 `camera_fb_null`：

1. 先完整斷電重插。
2. 重插 OV2640 FPC。
3. 確認 3.3V/2.8V/1.2V 與 GND。
4. 量 GPIO15 XCLK、GPIO4/5 SCCB、GPIO13 PCLK。
5. 先用 Camera + USB demo 測，不要用 Deploy benchmark 測 camera。

`deploy_benchmark` 沒有實體相機流程。
