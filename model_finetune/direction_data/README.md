# 📂 原始匯出資料目錄 (Direction Data Export Directory)

本資料夾包含了從 Edge Impulse 匯出的原始方向手勢資料夾，供專案初始化與資料庫整理使用。

---

## 📄 內容清單
* **`training/`**：包含所有尚未分類（或已用腳本分類）的原始訓練影像檔案。
* **`testing/`**：包含所有尚未分類的原始測試影像檔案。
* **`info.labels`**：Edge Impulse 用於識別各影像對應標籤（Label）的索引設定檔。
* **`上下左右-export.zip`**：從 Edge Impulse 下載的原始資料備份壓縮檔。
* **`README.txt`**：關於如何將本資料庫重新上傳至 Edge Impulse 專案的指引。

---

## 🛠️ 資料分類整理流程
這些原始影像通常是全部混在同一個目錄（例如 `training/`）中。本專案提供了一個分類腳本 `scripts/shuf.py`，它會：
1. 讀取 `direction_data/training/` 資料夾。
2. 自動解析每個圖片的檔名（例如含有 `up`、`down`、`left`、`right`、`null` 的關鍵字）。
3. 建立對應的類別子資料夾，並將圖片搬移進去，以建立標準的分類資料集。
