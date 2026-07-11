import os
import sys
import time
import numpy as np

# Force clean execution engines
os.environ['TF_ENABLE_ONEDNN_OPTS'] = '0'

import cv2
import tensorflow as tf

# CONFIGURATION
IMG_SIZE = (96, 96)
class_names = ['UP', 'DOWN', 'RIGHT', 'LEFT', 'NULL']

# Model mappings
MODELS = {
    '1': ('models/Separable_CNN_int8.tflite', 'Separable CNN'),
    '2': ('models/Mini_ResNet_int8.tflite', 'Mini ResNet'),
    '3': ('models/Baseline_CNN_int8.tflite', 'Baseline CNN'),
    '4': ('models/MobileNetV1_0.25_int8.tflite', 'MobileNetV1 0.25'),
    '5': ('models/MobileNetV2_0.35_int8.tflite', 'MobileNetV2 0.35')
}

current_key = '1'
model_path, model_name = MODELS[current_key]
interpreter = None
input_details = None
output_details = None
input_scale, input_zero_point = 1.0, 0
output_scale, output_zero_point = 1.0, 0
error_msg = ""

def load_tflite_model(key):
    global interpreter, input_details, output_details
    global input_scale, input_zero_point, output_scale, output_zero_point
    global model_path, model_name, error_msg, current_key
    
    path, name = MODELS[key]
    if not os.path.exists(path):
        error_msg = f"Error: {path} not found!"
        print(error_msg)
        return False
        
    try:
        print(f"Loading {name} ({path})...")
        interpreter = tf.lite.Interpreter(model_path=path)
        interpreter.allocate_tensors()
        
        input_details = interpreter.get_input_details()[0]
        output_details = interpreter.get_output_details()[0]
        
        input_scale, input_zero_point = input_details['quantization']
        output_scale, output_zero_point = output_details['quantization']
        
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
for k in ['1', '2', '3', '4', '5']:
    if load_tflite_model(k):
        break
else:
    print("Error: No valid model files found in workspace.")
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

    # Preprocessing (Grayscale -> Resize -> Normalize -> Quantize)
    gray_roi = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)
    resized_roi = cv2.resize(gray_roi, IMG_SIZE)
    normalized_roi = resized_roi.astype(np.float32) / 255.0
    
    # INT8 Quantization
    quantized_roi = np.clip(np.round((normalized_roi / input_scale) + input_zero_point), -128, 127).astype(np.int8)
    
    # Reshape for model input
    input_data = np.expand_dims(quantized_roi, axis=0)
    input_data = np.expand_dims(input_data, axis=-1)

    # Run Inference
    interpreter.set_tensor(input_details['index'], input_data)
    interpreter.invoke()
    
    output_data = interpreter.get_tensor(output_details['index'])[0]
    
    # INT8 Dequantization
    dequantized_output = (output_data.astype(np.float32) - output_zero_point) * output_scale
    
    # Argmax prediction
    pred_idx = np.argmax(dequantized_output)
    predicted_label = class_names[pred_idx] if pred_idx < len(class_names) else "UNKNOWN"
    confidence = dequantized_output[pred_idx]

    # Render Prediction Output
    text_color = (0, 255, 0) if predicted_label != 'NULL' else (0, 0, 255)
    result_text = f"Pred: {predicted_label} ({confidence * 100:.1f}%)"
    cv2.putText(frame, result_text, (20, 45), cv2.FONT_HERSHEY_SIMPLEX, 1.1, text_color, 3)
    
    # Render Active Model Info
    cv2.putText(frame, f"Model: {model_name} (INT8)", (20, 80), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 0), 2)

    # Render Switch Model Menu at the bottom
    cv2.rectangle(frame, (10, height - 110), (width - 10, height - 10), (50, 50, 50), -1)
    cv2.rectangle(frame, (10, height - 110), (width - 10, height - 10), (150, 150, 150), 1)
    
    cv2.putText(frame, "Switch Model [Press 1-5]:", (20, height - 85), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1, cv2.LINE_AA)
    cv2.putText(frame, "1: Separable CNN | 2: Mini ResNet | 3: Baseline CNN", (20, height - 60), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (200, 200, 200), 1, cv2.LINE_AA)
    cv2.putText(frame, "4: MobileNetV1  | 5: MobileNetV2  | [q]: Quit | [s]: Save Frame", (20, height - 35), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (200, 200, 200), 1, cv2.LINE_AA)

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
    elif key in [ord('1'), ord('2'), ord('3'), ord('4'), ord('5')]:
        target_key = chr(key)
        load_tflite_model(target_key)
    elif key == ord('s'):
        timestamp = int(time.time())
        img_name = f"snapshot_{predicted_label}_{timestamp}.jpg"
        cv2.imwrite(img_name, frame)
        print(f"Snapshot saved: {img_name}")

cap.release()
cv2.destroyAllWindows()
print("Webcam demo closed.")
