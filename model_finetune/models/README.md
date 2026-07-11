# 📂 量化模型庫 (Models Directory)

本資料夾存放所有經 TensorFlow Lite 轉換並進行 **INT8 全整數量化 (Post-Training Quantization, PTQ)** 的 `.tflite` 模型檔案。這些模型均已被強制將輸入與輸出指定為 `int8` 格式，以完美相容於 ESP32 等邊緣運算裝置。

---

## 📄 模型檔案清單

### 1. 方向/箭頭識別模型 (5 類：UP, DOWN, RIGHT, LEFT, NULL)
* **`Separable_CNN_int8.tflite`** (約 97.5 KB)
  * **架構**：使用深度可分離卷積（`SeparableConv2D`）。
  * **特點**：參數量極少，運算速度極快（PC 推理時間約 0.48 ms）。實測準確度高達 **90.70%**。最推薦部署於 ESP32 上！
* **`Mini_ResNet_int8.tflite`** (約 118.6 KB)
  * **架構**：自訂小型殘差網路（ResNet）。
  * **特點**：預測精度最高（實測準確度 **95.35%**），但推理延遲稍長（PC 端約 3.39 ms）。
* **`Baseline_CNN_int8.tflite`** (約 99.5 KB)
  * **架構**：標準 2 層卷積網路。
  * **特點**：基礎對照組模型，實測準確度約 76.74%。
* **`MobileNetV2_0.35_int8.tflite`** (約 615.8 KB)
  * **架構**：以 Width Multiplier 0.35 的預訓練 MobileNetV2 進行微調。
  * **特點**：體積較大且推理速度較慢（0.92 ms），實測準確度為 74.42%。
* **`MobileNetV1_0.25_int8.tflite`** (約 301.4 KB)
  * **架構**：以 Width Multiplier 0.25 的預訓練 MobileNetV1 進行微調。
  * **特點**：實測準確度僅 53.49%，效果不佳。
* **`MobileNetV2_int8.tflite`** & **`MobileNetV3_Small_int8.tflite`**
  * 更大型的預訓練量化對照模型。

### 2. 剪刀石頭布手勢識別模型 (3 類：rock, paper, scissors)
* **`gesture_model_int8.tflite`** (約 57.1 KB)
  * **特點**：舊版或先前訓練的手勢識別模型。類別為 `paper` (0), `rock` (1), `scissors` (2)。
  * **相關腳本**：可以使用 `scripts/test.py` 與 `scripts/test_custom_images.py` 載入此模型進行驗證。

---

## ⚙️ 模型規格資訊 (以 96x96 灰階輸入為例)
* **輸入形狀 (Input Shape)**：`[1, 96, 96, 1]`
* **輸入類型 (Input Type)**：`int8` (數值範圍：`-128` 至 `127`)
* **輸入量化參數 (Input Quantization)**：`scale` 與 `zero_point` 用於將原始浮點灰階矩陣 `[0.0, 1.0]` 轉換成 `int8` 整數。
* **輸出類型 (Output Type)**：`int8`
