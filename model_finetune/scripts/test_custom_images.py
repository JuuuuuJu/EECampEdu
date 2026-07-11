import os
# Disable oneDNN and force Keras 2 engine
os.environ['TF_ENABLE_ONEDNN_OPTS'] = '0'
os.environ['TF_USE_LEGACY_KERAS'] = '1'

import tensorflow as tf
import numpy as np
import glob
from PIL import Image

# 1. Configuration
MODEL_PATH = "models/gesture_model_int8.tflite"
# You can put your test images in a folder named 'test' or 'dataset/test'
TEST_DIR = "dataset/test"  # If this doesn't exist, we will also check a root 'test' folder
CLASS_NAMES = ["paper", "rock", "scissors"] # Sorted alphabetically (standard Keras folder order)

# 2. Check if model exists
if not os.path.exists(MODEL_PATH):
    print(f"Error: Quantized model '{MODEL_PATH}' not found. Please run 'train_and_quantize.py' first.")
    exit(1)

# 3. Load TFLite Model
interpreter = tf.lite.Interpreter(model_path=MODEL_PATH)
interpreter.allocate_tensors()

# Extract quantization parameters
input_details = interpreter.get_input_details()[0]
output_details = interpreter.get_output_details()[0]
scale, zero_point = input_details['quantization']

# 4. Resolve Test Directory
if not os.path.exists(TEST_DIR) and os.path.exists("test"):
    TEST_DIR = "test"

if not os.path.exists(TEST_DIR):
    print(f"Error: Test directory '{TEST_DIR}' not found.")
    print("Please create a folder named 'dataset/test' or 'test' and place your test images inside.")
    exit(1)

# 5. Find all images (jpg, jpeg, png)
image_extensions = ["*.jpg", "*.jpeg", "*.png", "*.JPG", "*.JPEG", "*.PNG"]
image_paths = []
for ext in image_extensions:
    image_paths.extend(glob.glob(os.path.join(TEST_DIR, "**", ext), recursive=True))

if not image_paths:
    print(f"No images found in '{TEST_DIR}'. Please add some image files (.jpg, .png).")
    exit(1)

print(f"Found {len(image_paths)} images in '{TEST_DIR}'. Starting inference...")
print("-" * 50)

correct_count = 0
total_count = 0

for img_path in image_paths:
    try:
        # Load image, convert to grayscale and resize to 96x96
        img = Image.open(img_path).convert('L')
        img_resized = img.resize((96, 96))
        
        # Convert to numpy array and normalize to 0.0 - 1.0
        img_array = np.array(img_resized, dtype=np.float32) / 255.0
        
        # Apply quantization to match the INT8 model requirements
        quantized_img = (img_array / scale) + zero_point
        quantized_img = np.round(quantized_img)
        quantized_img = np.clip(quantized_img, -128, 127).astype(np.int8)
        
        # Add batch dimension (1, 96, 96, 1)
        quantized_img = np.expand_dims(quantized_img, axis=0)
        quantized_img = np.expand_dims(quantized_img, axis=-1)
        
        # Set input tensor
        interpreter.set_tensor(input_details['index'], quantized_img)
        # Run inference
        interpreter.invoke()
        
        # Get output scores
        output_data = interpreter.get_tensor(output_details['index'])
        predicted_idx = np.argmax(output_data)
        predicted_class = CLASS_NAMES[predicted_idx]
        
        # Extract ground truth label from folder name if organized in subfolders
        parent_folder = os.path.basename(os.path.dirname(img_path)).lower()
        if parent_folder in CLASS_NAMES:
            actual_class = parent_folder
            is_correct = (predicted_class == actual_class)
            result_str = f"PRED: {predicted_class:<10} | ACTUAL: {actual_class:<10} | {'SUCCESS' if is_correct else 'FAILED'}"
            if is_correct:
                correct_count += 1
            total_count += 1
        else:
            result_str = f"PRED: {predicted_class:<10}"
            
        print(f"File: {os.path.basename(img_path):<25} | {result_str}")
        
    except Exception as e:
        print(f"Failed to process {os.path.basename(img_path)}: {e}")

print("-" * 50)
if total_count > 0:
    accuracy = (correct_count / total_count) * 100
    print(f"Summary: Classified {correct_count}/{total_count} correctly ({accuracy:.2f}% accuracy)")
else:
    print(f"Processed {len(image_paths)} images successfully.")
