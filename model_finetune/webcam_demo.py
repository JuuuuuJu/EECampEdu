import os
import sys
import time
import numpy as np

# Force clean execution engines
os.environ['TF_ENABLE_ONEDNN_OPTS'] = '0'
os.environ['TF_USE_LEGACY_KERAS'] = '1'

import cv2
import tensorflow as tf

# CONFIGURATION
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
IMG_SIZE = (96, 96)
class_names = ['UP', 'OK', 'THUMB', 'PALM', 'ROCK', 'STONE']

# Model mappings (using Keras models)
MODELS = {
    '1': (os.path.normpath(os.path.join(SCRIPT_DIR, 'models/tf/Mini_ResNet_finetuned.keras')), 'Mini ResNet FT'),
    '2': (os.path.normpath(os.path.join(SCRIPT_DIR, 'models/tf/MobileNetV2_finetuned.keras')), 'MobileNetV2 Gesture FT'),
}

current_key = '2'
model_path, model_name = MODELS[current_key]
model = None
error_msg = ""

current_classes = ['UP', 'OK', 'THUMB', 'PALM', 'ROCK', 'STONE']
expected_channels = 1

def load_keras_model(key):
    global model, model_path, model_name, error_msg, current_key, current_classes, expected_channels, IMG_SIZE
    
    path, name = MODELS[key]
    if not os.path.exists(path):
        error_msg = f"Error: {path} not found!"
        print(error_msg)
        return False
        
    try:
        print(f"Loading {name} ({path})...")
        model = tf.keras.models.load_model(path, safe_mode=False)
        
        # Extract metadata from model
        input_shape = model.input_shape
        IMG_SIZE = (input_shape[1], input_shape[2])
        expected_channels = input_shape[3]
        print(f"Model expects input resolution: {IMG_SIZE}")
        print(f"Model expects {expected_channels} input channel(s)")
        
        # Dynamically set class list based on output shape
        num_classes = model.output_shape[-1]
        if num_classes == 4:
            current_classes = ['UP', 'DOWN', 'RIGHT', 'LEFT']
            print(f"Detected 4-class model ({current_classes})")
        elif num_classes == 6:
            current_classes = ['UP', 'OK', 'THUMB', 'PALM', 'ROCK', 'STONE']
            print(f"Detected 6-class model ({current_classes})")
        elif num_classes == 7:
            current_classes = ['UP', 'DOWN', 'RIGHT', 'LEFT', 'PAPER', 'SCISSORS', 'STONE']
            print(f"Detected 7-class model ({current_classes})")
        elif num_classes == 3:
            current_classes = ['PAPER', 'SCISSORS', 'STONE']
            print(f"Detected 3-class model ({current_classes})")
        else:
            current_classes = ['UP', 'OK', 'THUMB', 'PALM', 'ROCK', 'STONE']
            print(f"Detected {num_classes}-class model ({current_classes})")
            
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
for k in ['2',  '1']:
    if load_keras_model(k):
        break
else:
    print("Warning: No pre-trained Keras model files found in workspace. Run train_mini_resnet.py or train_mobilenet.py first.")
    sys.exit(1)

# Initialize webcam
print("Opening Webcam... (Press 'q' to quit, 's' to save current snapshot)")
cap = cv2.VideoCapture(0)

if not cap.isOpened():
    print("Error: Could not open webcam.")
    sys.exit(1)

cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)

cv2.namedWindow("Real-Time Direction Recognition Demo", cv2.WINDOW_AUTOSIZE)

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

    # Preprocessing
    gray_roi = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)
    resized_roi = cv2.resize(gray_roi, IMG_SIZE)
    normalized_roi = resized_roi.astype(np.float32) / 255.0
    
    # Match the model's float32 input range (MobileNet expects [-1, 1], CNNs expect [0, 1])
    # MobileNetV2 finetuned model has internal scaling, so we don't scale it externally
    if 'MobileNet' in model_name and 'finetuned' not in model_path.lower():
        float_roi = (normalized_roi * 2.0) - 1.0
    else:
        float_roi = normalized_roi
        
    # Input Preparation
    if expected_channels == 3:
        rgb_roi = np.stack([float_roi] * 3, axis=-1)
        input_data = np.expand_dims(rgb_roi, axis=0).astype(np.float32)
    else:
        input_data = np.expand_dims(float_roi, axis=0)
        input_data = np.expand_dims(input_data, axis=-1).astype(np.float32)

    # Run Inference
    output_data = model(input_data, training=False)[0].numpy()
    
    # Argmax prediction
    pred_idx = np.argmax(output_data)
    confidence = output_data[pred_idx]

    # Adaptive Confidence Thresholding Filter for stability
    if len(current_classes) > 5:
        CONFIDENCE_THRESHOLD = 0.55 # 7 classes (UP/DOWN/LEFT/RIGHT/PAPER/SCISSORS/STONE)
    elif len(current_classes) == 3:
        CONFIDENCE_THRESHOLD = 0.65 # 3 classes (PAPER/SCISSORS/STONE)
    else:
        CONFIDENCE_THRESHOLD = 0.70 # 4 or 5 classes
        
    if confidence < CONFIDENCE_THRESHOLD:
        predicted_label = 'NULL'
    else:
        predicted_label = current_classes[pred_idx] if pred_idx < len(current_classes) else "UNKNOWN"

    # Render Prediction Output
    text_color = (0, 255, 0) if predicted_label != 'NULL' else (0, 0, 255)
    result_text = f"Pred: {predicted_label} ({confidence * 100:.1f}%)"
    cv2.putText(frame, result_text, (20, 45), cv2.FONT_HERSHEY_SIMPLEX, 1.1, text_color, 3)
    
    # Render Active Model Info
    cv2.putText(frame, f"Model: {model_name} (Keras)", (20, 80), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 0), 2)

    # Render Switch Model Menu at the bottom
    cv2.rectangle(frame, (10, height - 110), (width - 10, height - 10), (50, 50, 50), -1)
    cv2.rectangle(frame, (10, height - 110), (width - 10, height - 10), (150, 150, 150), 1)
    
    cv2.putText(frame, "Switch Model [Press 1-3]:", (20, height - 85), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1, cv2.LINE_AA)
    cv2.putText(frame, "1: Mini ResNet | 2: MobileNetV2 Gesture", (20, height - 60), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (200, 200, 200), 1, cv2.LINE_AA)
    cv2.putText(frame, "[q]: Quit | [s]: Take Snapshot", (20, height - 35), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (200, 200, 200), 1, cv2.LINE_AA)

    # If there is a loading error, show it in red
    if error_msg:
        cv2.putText(frame, error_msg, (20, 115), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 255), 2)

    # Render what the model sees (96x96 grayscale)
    model_view = cv2.resize(resized_roi, (150, 150))
    model_view_color = cv2.cvtColor(model_view, cv2.COLOR_GRAY2BGR)
    frame[10:160, width - 160:width - 10] = model_view_color
    cv2.rectangle(frame, (width - 160, 10), (width - 10, 160), (255, 255, 255), 1)
    cv2.putText(frame, "Model Input", (width - 160, 175), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 255), 1)

    cv2.imshow("Real-Time Direction Recognition Demo", frame)

    key = cv2.waitKey(1) & 0xFF
    if key == ord('q'):
        break
    elif key in [ord('1'), ord('2')]:
        target_key = chr(key)
        load_keras_model(target_key)
    elif key == ord('s'):
        timestamp = int(time.time())
        img_name = f"snapshot_{predicted_label}_{timestamp}.jpg"
        cv2.imwrite(img_name, frame)
        print(f"Snapshot saved: {img_name}")


cap.release()
cv2.destroyAllWindows()
print("Webcam demo closed.")
