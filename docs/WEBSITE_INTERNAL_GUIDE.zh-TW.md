# EECampEdu AI PC Portal 組內操作說明

這份文件給開發與助教組內使用，說明網站功能、操作流程、硬體串接與目前的控制邏輯。學生版請看 `docs/STUDENT_README.md`。

## 1. 系統定位

AI PC Portal 是跑在 AI PC 上的 Flask web server。學生用 Chrome / Edge 開啟網站，透過 HTTPS + Web Serial 操作接在學生電腦上的 ESP32-S3 main board 與 ESP32 control board。

主要流程：

```text
Dataset / OV2640 capture -> train .keras / .pth -> quantize .tflite
-> flash main board firmware -> flash model partition
-> main board OV2640 inference -> browser forwards ACTION to control board
-> control board drives servos
```

## 2. 背景服務

AI PC 上主要有兩個長駐服務：

```bash
systemctl --user status eecamp-portal
systemctl --user restart eecamp-portal
journalctl --user -u eecamp-portal -f

systemctl --user status eecamp-camera-app
systemctl --user restart eecamp-camera-app
journalctl --user -u eecamp-camera-app -f
```

改到 `apps/training_portal/server.py`、`apps/training_portal/templates/index.html`、portal 靜態檔或文件後，至少重啟 `eecamp-portal` 才會在網站上看到新版本。

## 3. 網站頁面功能

### Home

首頁只放營隊歡迎資訊、logo 與頁尾 repo 連結。左上 brand 會回到首頁。

### Model finetune

用途是讓學生建立自己的 gesture dataset 並訓練模型。

功能：

- 上傳 dataset zip。
- 用 OV2640 拍照建立 dataset。
- Dataset 可以存在超過 6 種 gesture class，但一次 active classes 最多 6 個。
- 可以設定 class -> robot action mapping。
- 開始 TensorFlow / PyTorch training。
- 選 `.keras` model 對目前影像做 preview prediction。

注意：training 與 prediction 都應該使用與 firmware 一致的 preprocessing：OV2640 frame 轉成模型 input 所需的 96x96 grayscale 格式。

### Deploy & benchmark

用途是部署模型並量測 ESP32-S3 上的 inference 表現。

功能：

- 選 `.keras` model 做 quantization。
- 產出 deployable `.tflite` 與 quantization report。
- Flash deploy benchmark firmware。
- Flash `.tflite` 到 model partition。
- 在 browser 內透過 Web Serial 跑 benchmark。
- 顯示 accuracy、latency、throughput、PC reference similarity。

Benchmark 用來驗證 deploy。

### Output demo

用途是 output 組教學。學生只改被限定的 teaching block，不直接改整份 firmware。

功能：

- 在網頁內改 C code teaching block。
- AI PC 執行 allowlisted ESP-IDF build。
- Build 成功才允許 flash。
- 可用 serial 指令測 LED / PWM / pattern。

編譯錯誤會完整顯示在 build log，方便學生 debug。

### Main board firmware

用途是完整整合流程：camera、USB CDC/MSC、preprocessing、TFLite Micro inference、prediction forwarding。

操作順序：

1. Flash main board firmware 到 ESP32-S3。
2. Flash `.tflite` model 到 model partition。
3. Flash control board firmware 到 ESP32 control board。
4. Connect camera stream，選 ESP32-S3 main board 的 serial port。
5. Connect control board，選 ESP32 control board 的 serial port。
6. main board 產生 `RESULT,...` 後，browser 會把 prediction action 轉送到 control board。

### Camera + USB demo

用途是單純測 OV2640 與 USB CDC/MSC，不主打模型訓練。

功能：

- 開啟 live JPEG preview。
- 調整 pixel format、resolution、brightness、contrast、exposure 等 camera setting。
- 拍照存到 ESP flash 對應資料夾。
- 透過 CDC download 或 MSC mount 看照片。

### AI PC Drive

用途是組內暫存 code / model / zip / 圖片，不是 ESP flash。

規則：

- `0_shared` 給整組整理共用檔。
- `1` 到 `12` 給小隊員各自放檔。
- 大量檔案請先壓成 zip 再上傳。
- OV2640 實拍照片若是要看 ESP 端儲存，應該走 ESP flash / MSC，不是 AI PC Drive。

## 4. Main board -> control board 動作邏輯

Main board firmware 只負責 camera capture、preprocessing、TFLite Micro inference，並透過 CDC 印出：

```text
RESULT,<pred>,<model_us>,<preprocess_us>,<total_us>,<score0>,...
```

Portal 解析 `RESULT` 後做兩層 mapping：

1. `pred index -> class name`
2. `class name -> robot action`

若 confidence 小於 70%，portal 會視為 `NULL / idle`，送到 control board 的命令是：

```text
ACTION,none
```

若 confidence 達標，portal 會送：

```text
ACTION,up
ACTION,down
ACTION,left
ACTION,right
ACTION,clamp
ACTION,release
```

## 5. Control board servo 實際運作

Control board 是普通 ESP32，接四顆 servo。腳位：

```text
base  GPIO18
arm   GPIO19
pitch GPIO22
claw  GPIO21
```

Control board serial baudrate 是 `115200`。目前 portal 使用 preferred protocol：

```text
ACTION,<name>
```

實際 servo 動作如下：

```text
ACTION,up       pitch +5 degrees
ACTION,down     pitch -5 degrees
ACTION,left     base -5 degrees
ACTION,right    base +5 degrees
ACTION,clamp    claw -> 0 degrees
ACTION,release  claw -> 80 degrees
ACTION,none     no movement
```

Servo PWM：50 Hz，pulse range 約 500 us 到 2500 us。Control board 仍保留 legacy gesture command 與 manual angle command，但 main board 頁面應使用 `ACTION,<name>`。

## 6. 常見問題

- Flash 或 benchmark 卡住：確認沒有 Arduino Serial Monitor / idf.py monitor / PuTTY 佔用同一個 port。
- Web Serial 不出現：必須用 Chrome / Edge，且網站需 HTTPS 或 localhost secure context。
- Main board 有 prediction 但 servo 不動：確認 main board 頁面已按 `Connect control board`，並選到 control board 的 serial port。
- Prediction 一直是 NULL：通常是 confidence 低於 70%，或 class mapping / model 不匹配。
- 改 portal 後沒變化：重啟 `eecamp-portal`，再重新整理 browser。