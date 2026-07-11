import tensorflow as tf
import numpy as np

# 1. Load your dataset again (same way as training)
val_ds = tf.keras.utils.image_dataset_from_directory(
    "dataset/train", validation_split=0.2, subset="validation", seed=123,
    color_mode="grayscale", image_size=(96, 96), batch_size=1
)

# 2. Load the TFLite Model
interpreter = tf.lite.Interpreter(model_path="models/gesture_model_int8.tflite")
interpreter.allocate_tensors()

# Extract the exact quantization parameters TensorFlow chose
input_details = interpreter.get_input_details()[0]
output_details = interpreter.get_output_details()[0]
scale, zero_point = input_details['quantization']

correct_predictions = 0
total_images = 0

for img, label in val_ds.unbatch():
    # 1. Normalize image to 0.0 - 1.0 (exactly how it was trained)
    img_float = img.numpy() / 255.0  
    
    # 2. Apply the dynamic quantization math
    quantized_img = (img_float / scale) + zero_point
    quantized_img = np.round(quantized_img) # Critical: Round before casting
    quantized_img = np.clip(quantized_img, -128, 127).astype(np.int8)
    
    # 3. Shape and predict
    quantized_img = np.expand_dims(quantized_img, axis=0)
    interpreter.set_tensor(input_details['index'], quantized_img)
    interpreter.invoke()
    
    output_data = interpreter.get_tensor(output_details['index'])
    predicted_label = np.argmax(output_data)
    
    if predicted_label == label.numpy():
        correct_predictions += 1
    total_images += 1

print(f"\nCorrected Accuracy: {(correct_predictions / total_images) * 100:.2f}%")
