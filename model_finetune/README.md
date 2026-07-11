# 上下左右手勢識別與邊緣運算模型專案 (Direction & Gesture Recognition TFLite Project)

本專案主要用於手勢/方向（上、下、左、右、背景）識別模型的訓練、量化（Quantization）與電腦/邊緣運算硬體（如 ESP32）上的即時預測評估。

---

## 📂 目錄結構說明

```text
model_finetune/
├── dataset/                  # 手勢訓練與驗證資料集 (已分割為 80% 訓練, 20% 驗證)
├── debug_injected_images/    # 用於分析訓練增強注入的調試影像
├── direction_data/           # Edge Impulse 格式之原始方向資料與匯出檔
├── models/                   # 已轉換且量化為 INT8 的 TFLite 模型庫 (*_int8.tflite)
├── new_test_data/            # 用於真實場景測試的獨立測試影像集 (Real-world Testset)
├── scripts/                  # 舊版手勢評估、資料整理與環境驗證的輔助腳本
├── collect_data.py           # 核心收集腳本：提供圖形介面以拍照並自動擴充資料集
├── README.md                 # 專案主說明文件 (本檔案)
├── train_and_quantize.py     # 核心訓練腳本：支援兩階段訓練與全整數量化 (INT8)
└── webcam_demo.py            # 核心展示腳本：支援即時視訊預測與多模型鍵盤即時切換 (1-5 鍵)
```

各子目錄內皆附有獨立的 `README.md`，提供更詳細的該單元操作指引。

---

## 📊 模型效能對比 (Benchmarks)

以下是使用 `dataset/train` 與 `new_test_data`（實測集）測試的各架構全整數量化（INT8）模型數據：

| 模型名稱 | 驗證集準確度 (Val Acc) | 實測集準確度 (Real Acc) | 推理延遲 (PC Latency) | 模型大小 (File Size) | 邊緣運算部署推薦 |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Separable_CNN_int8.tflite** | 97.67% | **90.70%** | **0.478 ms** | **97.5 KB** | ⭐⭐⭐⭐⭐ (首選) |
| **Mini_ResNet_int8.tflite** | **100.00%** | **95.35%** | 3.389 ms | 118.6 KB | ⭐⭐⭐⭐ (精準但略慢) |
| **Baseline_CNN_int8.tflite** | 90.70% | 76.74% | 0.892 ms | 99.5 KB | ⭐⭐⭐ (普通) |
| **MobileNetV2_0.35_int8.tflite**| 79.07% | 74.42% | 0.921 ms | 615.8 KB | ⭐ (體積過大且準度低) |
| **MobileNetV1_0.25_int8.tflite**| 74.42% | 53.49% | 0.575 ms | 301.4 KB | ⭐ (不推薦) |

---

## 🛠️ 快速開始指南

### 1. 環境準備
請使用支持 TensorFlow 2.x 的 Python 3.10 環境（本專案已在 `teaching_monster` Conda 環境中配置完成）：
```bash
conda activate teaching_monster
```

### 2. 運行即時鏡頭 Demo (包含鍵盤模型即時切換)
執行 `webcam_demo.py` 可以開啟電腦鏡頭進行手勢預測：
```bash
python webcam_demo.py
```
* **鍵盤操作指令**：
  * 按 **`1`**：切換為 `Separable CNN`
  * 按 **`2`**：切換為 `Mini ResNet`
  * 按 **`3`**：切換為 `Baseline CNN`
  * 按 **`4`**：切換為 `MobileNetV1`
  * 按 **`5`**：切換為 `MobileNetV2`
  * 按 **`s`**：保存當前畫面快照 (Save Snapshot)
  * 按 **`q`**：退出 (Quit)

### 3. 收集手勢圖片加入資料集
執行 `collect_data.py` 開啟圖形化介面收集手勢圖片：
```bash
python collect_data.py
```
* **操作指引**：
  1. 在右側文字框中輸入要收集的類別（例如 `up`、`down`、`new_label`）。
  2. 將手放至綠色對齊框內。
  3. 按下 **「拍照存檔 (Space)」** 按鈕（或按鍵盤 **空白鍵**）進行拍攝。
  4. 裁切好的影像將自動儲存於 `dataset/train/<label_name>/` 底下，並自動建立該分類目錄。

### 4. 模型重新訓練與量化
若需調整模型或重新訓練，請運行：
```bash
python train_and_quantize.py
```
訓練完成後，各架構的 `int8.tflite` 模型會自動儲存至 `models/` 資料夾下，並輸出效能對比表。
