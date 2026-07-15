import os
import sys
import time
import numpy as np
import cv2

# CONFIGURATION
class_names = ['UP', 'OK', 'THUMB', 'PALM', 'ROCK', 'STONE']
expected_channels = 1

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
MODELS_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "../models/pytorch"))

# Model mappings (using ONNX models)
MODELS = {
    '1': (os.path.join(MODELS_DIR, 'Mini_ResNet_finetuned.onnx'), 'Mini ResNet FT ONNX')
}

current_key = '1'
model_path, model_name = MODELS[current_key]
net = None
error_msg = ""
IMG_SIZE = (96, 96)

def load_onnx_model(key):
    global net, model_path, model_name, error_msg, current_key, IMG_SIZE
    
    path, name = MODELS[key]
    if not os.path.exists(path):
        error_msg = f"Error: {path} not found!"
        print(error_msg)
        return False
        
    try:
        print(f"Loading {name} ({path})...")
        net = cv2.dnn.readNetFromONNX(path)
        
        # Parse image size from path/key
        img_sz = 128 if "128" in path else 96
        IMG_SIZE = (img_sz, img_sz)
        
        print(f"Model expects input resolution: {IMG_SIZE}")
        
        model_path = path
        model_name = name
        current_key = key
        error_msg = ""
        return True
    except Exception as e:
        error_msg = f"Failed to load: {e}"
        print(error_msg)
        return False

# Load default model
for k in ['1']:
    if load_onnx_model(k):
        break
else:
    print("Warning: No pre-trained ONNX model files found in models directory. Run train_mini_resnet.py first.")
    sys.exit(1)

# Initialize webcam
print("Opening Webcam... (Press 'q' to quit, 's' to save current snapshot)")
cap = cv2.VideoCapture(0)

if not cap.isOpened():
    print("Error: Could not open webcam.")
    sys.exit(1)

cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)

cv2.namedWindow("ONNX Real-Time Direction Recognition Demo", cv2.WINDOW_AUTOSIZE)

while True:
    ret, frame = cap.read()
    if not ret:
        print("Error: Failed to grab frame.")
        break

    frame = cv2.flip(frame, 1)
    height, width, _ = frame.shape

    # Define ROI box in the center
    box_size = 250
    x1 = (width - box_size) // 2
    y1 = (height - box_size) // 2
    x2 = x1 + box_size
    y2 = y1 + box_size

    # Draw the bounding box for ROI
    cv2.rectangle(frame, (x1, y1), (x2, y2), (0, 255, 0), 2)
    cv2.putText(frame, "Place Gesture Here", (x1, y1 - 10),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)

    # Extract ROI from frame
    roi = frame[y1:y2, x1:x2]

    # Preprocessing (Grayscale -> Resize -> Normalization to [0.0, 1.0])
    gray_roi = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)
    resized_roi = cv2.resize(gray_roi, IMG_SIZE)
    normalized_roi = resized_roi.astype(np.float32) / 255.0

    # Build blob for OpenCV DNN: shape (1, 1, H, W)
    blob = cv2.dnn.blobFromImage(normalized_roi, scalefactor=1.0, size=IMG_SIZE, mean=0, swapRB=False, crop=False)
    
    # Run Inference
    net.setInput(blob)
    output = net.forward() # Logits output (1, 4)
    
    # Apply softmax on numpy array
    exp_output = np.exp(output - np.max(output, axis=1, keepdims=True))
    output_data = (exp_output / np.sum(exp_output, axis=1, keepdims=True))[0]
    
    # Argmax prediction
    pred_idx = np.argmax(output_data)
    confidence = output_data[pred_idx]

    # Adaptive Confidence Thresholding Filter for stability
    CONFIDENCE_THRESHOLD = 0.70
        
    if confidence < CONFIDENCE_THRESHOLD:
        predicted_label = 'NULL'
    else:
        predicted_label = class_names[pred_idx] if pred_idx < len(class_names) else "UNKNOWN"

    # Render Prediction Output
    text_color = (0, 255, 0) if predicted_label != 'NULL' else (0, 0, 255)
    result_text = f"Pred: {predicted_label} ({confidence * 100:.1f}%)"
    cv2.putText(frame, result_text, (20, 45), cv2.FONT_HERSHEY_SIMPLEX, 1.1, text_color, 3)
    
    # Render Active Model Info
    cv2.putText(frame, f"Model: {model_name}", (20, 80), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 0), 2)

    # Render menu at the bottom
    cv2.rectangle(frame, (10, height - 60), (width - 10, height - 10), (50, 50, 50), -1)
    cv2.rectangle(frame, (10, height - 60), (width - 10, height - 10), (150, 150, 150), 1)
    
    cv2.putText(frame, "[q]: Quit | [s]: Take Snapshot", (20, height - 30), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (200, 200, 200), 1, cv2.LINE_AA)

    # If there is a loading error, show it in red
    if error_msg:
        cv2.putText(frame, error_msg, (20, 115), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 255), 2)

    # Render what the model sees (resized grayscale)
    model_view = cv2.resize(resized_roi, (150, 150))
    model_view_color = cv2.cvtColor(model_view, cv2.COLOR_GRAY2BGR)
    frame[10:160, width - 160:width - 10] = model_view_color
    cv2.rectangle(frame, (width - 160, 10), (width - 10, 160), (255, 255, 255), 1)
    cv2.putText(frame, "Model Input", (width - 160, 175), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 255), 1)

    cv2.imshow("ONNX Real-Time Direction Recognition Demo", frame)

    key = cv2.waitKey(1) & 0xFF
    if key == ord('q'):
        break
    elif key == ord('s'):
        timestamp = int(time.time())
        img_name = f"snapshot_{predicted_label}_{timestamp}.jpg"
        cv2.imwrite(img_name, frame)
        print(f"Snapshot saved: {img_name}")

cap.release()
cv2.destroyAllWindows()
print("Webcam demo closed.")
