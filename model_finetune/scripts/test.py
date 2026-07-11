import numpy as np
import tensorflow as tf
import os
from PIL import Image
from rembg import remove

MODEL_PATH = "models/gesture_model_int8.tflite"
TEST_DATA_DIR = "./new_test_data"
IMG_SIZE = (96, 96)

interpreter = tf.lite.Interpreter(model_path=MODEL_PATH)
interpreter.allocate_tensors()
input_details = interpreter.get_input_details()
output_details = interpreter.get_output_details()

import cv2

def load_and_preprocess(img_path, details):
    # 1. Load the original messy photo
    input_img = Image.open(img_path)
    
    # 2. Remove the messy background
    removed_bg = remove(input_img)
    
    # 3. Paste onto a flat Green background to match training domain
    # Your model trained on RGB Green Screen -> Grayscale, 
    # so we replicate that process precisely.
    green_bg = Image.new("RGBA", removed_bg.size, (0, 255, 0, 255))
    green_bg.paste(removed_bg, (0, 0), removed_bg)
    
    # 4. Convert to Grayscale & Resize (matches training)
    img = green_bg.convert('L').resize(IMG_SIZE)
    img_array = np.array(img, dtype=np.float32) / 255.0
    img_array = img_array.reshape(1, 96, 96, 1)
    
    # 5. Quantize using model parameters
    params = details.get('quantization_parameters')
    if params and 'scales' in params and len(params['scales']) > 0:
        scale = params['scales'][0]
        zero_point = params['zero_points'][0]
        quantized = (img_array / scale) + zero_point
        return np.clip(np.round(quantized), -128, 127).astype(np.int8)
    
    return img_array.astype(np.float32)

label_map = {'rock': 0, 'paper': 1, 'scissors': 2}
correct = 0
total = 0

for label in os.listdir(TEST_DATA_DIR):
    label_path = os.path.join(TEST_DATA_DIR, label)
    if not os.path.isdir(label_path): continue
    
    for filename in os.listdir(label_path):
        img_path = os.path.join(label_path, filename)
        # PASSING THE DICTIONARY NOW
        input_data = load_and_preprocess(img_path, input_details[0])
        
        interpreter.set_tensor(input_details[0]['index'], input_data)
        interpreter.invoke()
        
        pred = np.argmax(interpreter.get_tensor(output_details[0]['index']))
        print(f"File: {filename} | Label: {label} (Idx: {label_map.get(label)}) | Pred: {pred}")
        
        if label_map.get(label) == pred:
            correct += 1
        total += 1

print(f"Accuracy: {correct / total:.2%}")