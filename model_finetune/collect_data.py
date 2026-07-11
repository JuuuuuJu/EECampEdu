import os
import time
import tkinter as tk
from tkinter import ttk, messagebox
import cv2
from PIL import Image, ImageTk

# CONFIGURATION
BASE_DATASET_DIR = "dataset/train"
BOX_SIZE = 250  # ROI square size

class DatasetCollectorApp:
    def __init__(self, root):
        self.root = root
        self.root.title("手勢資料收集工具 (Gesture Dataset Collector)")
        self.root.geometry("900x680")
        self.root.resizable(False, False)

        # Style configurations
        self.style = ttk.Style()
        self.style.theme_use("clam")
        
        # Initialize camera
        self.cap = cv2.VideoCapture(0)
        if not self.cap.isOpened():
            messagebox.showerror("錯誤", "無法開啟視訊鏡頭，請檢查相機連接與權限。")
            self.root.destroy()
            return
            
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)

        # Main Layout Frames
        left_frame = ttk.Frame(self.root, padding=10)
        left_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        right_frame = ttk.Frame(self.root, padding=10, width=250)
        right_frame.pack(side=tk.RIGHT, fill=tk.Y)
        right_frame.pack_propagate(False)

        # Left Panel: Camera Feed
        self.cam_label = ttk.Label(left_frame)
        self.cam_label.pack(fill=tk.BOTH, expand=True)

        # Right Panel: Controls & Ingestion
        title_label = ttk.Label(right_frame, text="資料集收集控制台", font=("Microsoft JhengHei", 14, "bold"))
        title_label.pack(pady=15)

        # Text input for Class Label
        label_frame = ttk.LabelFrame(right_frame, text=" 類別標籤 (Label) ", padding=10)
        label_frame.pack(fill=tk.X, pady=10)

        self.label_entry = ttk.Entry(label_frame, font=("Microsoft JhengHei", 11))
        self.label_entry.insert(0, "up")  # Default class
        self.label_entry.pack(fill=tk.X, pady=5)
        
        # Instruction
        inst_label = ttk.Label(label_frame, text="例如: up, down, left, right\n(若輸入新類別會自動建立資料夾)", 
                               font=("Microsoft JhengHei", 9), foreground="gray")
        inst_label.pack(anchor=tk.W)

        # Image counter and status
        status_frame = ttk.LabelFrame(right_frame, text=" 當前狀態 ", padding=10)
        status_frame.pack(fill=tk.X, pady=10)

        self.counter_label = ttk.Label(status_frame, text="當前類別已收集: 0 張", font=("Microsoft JhengHei", 10))
        self.counter_label.pack(anchor=tk.W, pady=2)

        self.total_counter_label = ttk.Label(status_frame, text="資料集總計: 0 張", font=("Microsoft JhengHei", 10))
        self.total_counter_label.pack(anchor=tk.W, pady=2)

        self.log_label = ttk.Label(status_frame, text="等待拍照...", font=("Microsoft JhengHei", 9), foreground="green")
        self.log_label.pack(anchor=tk.W, pady=10)

        # Action Buttons
        self.capture_btn = ttk.Button(right_frame, text="📸 拍照存檔 (Space)", command=self.capture_image)
        self.capture_btn.pack(fill=tk.X, pady=15, ipady=8)

        # Keyboard shortcuts
        self.root.bind("<space>", lambda event: self.capture_image())
        self.label_entry.bind("<KeyRelease>", lambda event: self.update_counters())

        # Start camera updates and count initial images
        self.update_counters()
        self.update_feed()

        # Window closing handler
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)

    def update_feed(self):
        ret, frame = self.cap.read()
        if ret:
            # Flip frame for natural mirror effect
            frame = cv2.flip(frame, 1)
            self.current_frame = frame.copy() # Store clean copy without overlays
            
            # Draw green ROI bounding box on GUI representation
            height, width, _ = frame.shape
            x1 = (width - BOX_SIZE) // 2
            y1 = (height - BOX_SIZE) // 2
            x2 = x1 + BOX_SIZE
            y2 = y1 + BOX_SIZE
            
            cv2.rectangle(frame, (x1, y1), (x2, y2), (0, 255, 0), 2)
            cv2.putText(frame, "Align Hand inside this box", (x1, y1 - 10),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1, cv2.LINE_AA)

            # Convert BGR frame to RGB for Tkinter
            cv2_image = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            img = Image.fromarray(cv2_image)
            imgtk = ImageTk.PhotoImage(image=img)
            self.cam_label.imgtk = imgtk
            self.cam_label.configure(image=imgtk)

        self.root.after(15, self.update_feed)

    def capture_image(self):
        label = self.label_entry.get().strip().lower()
        if not label:
            messagebox.showwarning("警告", "請輸入類別名稱！")
            return

        # Ensure destination directory exists
        target_dir = os.path.join(BASE_DATASET_DIR, label)
        os.makedirs(target_dir, exist_ok=True)

        # Crop the center ROI from the clean frame
        height, width, _ = self.current_frame.shape
        x1 = (width - BOX_SIZE) // 2
        y1 = (height - BOX_SIZE) // 2
        x2 = x1 + BOX_SIZE
        y2 = y1 + BOX_SIZE
        
        roi = self.current_frame[y1:y2, x1:x2]

        # Save image
        timestamp = int(time.time() * 1000)
        filename = f"{label}_{timestamp}.jpg"
        filepath = os.path.join(target_dir, filename)
        
        cv2.imwrite(filepath, roi)
        
        # Log status
        relative_path = os.path.join("dataset", "train", label, filename)
        self.log_label.config(text=f"已儲存: {relative_path}", foreground="blue")
        
        # Update image counts
        self.update_counters()

    def count_images_in_dir(self, directory):
        if not os.path.exists(directory):
            return 0
        count = 0
        for fname in os.listdir(directory):
            if fname.lower().endswith(('.png', '.jpg', '.jpeg', '.bmp')):
                count += 1
        return count

    def update_counters(self):
        label = self.label_entry.get().strip().lower()
        
        # Count for current class
        if label:
            class_dir = os.path.join(BASE_DATASET_DIR, label)
            class_count = self.count_images_in_dir(class_dir)
            self.counter_label.config(text=f"當前類別已收集: {class_count} 張")
        else:
            self.counter_label.config(text="當前類別已收集: 0 張")

        # Count total dataset size
        total_count = 0
        if os.path.exists(BASE_DATASET_DIR):
            for sub in os.listdir(BASE_DATASET_DIR):
                sub_path = os.path.join(BASE_DATASET_DIR, sub)
                if os.path.isdir(sub_path):
                    total_count += self.count_images_in_dir(sub_path)
        self.total_counter_label.config(text=f"資料集總計: {total_count} 張")

    def on_closing(self):
        self.cap.release()
        self.root.destroy()

if __name__ == "__main__":
    root = tk.Tk()
    app = DatasetCollectorApp(root)
    root.mainloop()
