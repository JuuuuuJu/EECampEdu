# 📂 輔助與舊版腳本目錄 (Scripts Directory)

本資料夾存放專案的輔助工具、環境驗證腳本以及先前手勢識別（剪刀石頭布）的測試程式。

> [!IMPORTANT]
> **運行指引**：為了確保檔案相對路徑正確，請統一在 `model_finetune/` 目錄下執行這些腳本，而不是進入 `scripts/` 資料夾中執行。

---

## 📄 腳本清單與執行方式

### 1. `verify_setup.py` (環境驗證)
* **用途**：驗證當前的 TensorFlow 軟硬體環境，並嘗試初始化一個適用於 ESP32 的 MobileNetV2 0.35 模型，以檢查邊緣硬體相容性。
* **執行**：
  ```bash
  python scripts/verify_setup.py
  ```

### 2. `shuf.py` (原始資料整理)
* **用途**：讀取 `direction_data/training/` 底下的未分類影像，並依檔名關鍵字（如 `up`、`down`）將它們歸類移入子資料夾，以供訓練使用。
* **執行**：
  ```bash
  python scripts/shuf.py
  ```

### 3. `check_tflite.py` (量化準確度檢測)
* **用途**：載入特定的 `.tflite` 模型，並以 `dataset/train` 分割出的 20% 驗證資料，進行量化後的準確度（INT8 Accuracy）檢算。
* **執行**：
  ```bash
  python scripts/check_tflite.py
  ```

### 4. `test.py` (舊版手勢實測 - 背景去除)
* **用途**：針對舊版手勢模型 `models/gesture_model_int8.tflite` (剪刀石頭布) 進行評估。腳本中使用了 `rembg` 去除雜亂背景，並將手勢貼到**綠幕背景**上以符合訓練集特徵分佈，再進行辨識。
* **執行**：
  ```bash
  python scripts/test.py
  ```

### 5. `test_custom_images.py` (自訂影像資料夾預測)
* **用途**：指定一個資料夾（預設 `dataset/test`），載入手勢模型進行批量影像的預測與混淆矩陣準確度統計。
* **執行**：
  ```bash
  python scripts/test_custom_images.py
  ```
