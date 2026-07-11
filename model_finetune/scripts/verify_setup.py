import tensorflow as tf

print("TensorFlow Version:", tf.__version__)

# Attempt to build your exact target model architecture
try:
    model = tf.keras.applications.MobileNetV2(
        input_shape=(96, 96, 1),  # 96x96 Grayscale
        alpha=0.35,               # Width multiplier for ESP32
        include_top=True,         # Include classification layer
        weights=None,             # No ImageNet weights since we changed channels
        classes=3                 # Change this to your number of gesture classes
    )
    print("\nSUCCESS: MobileNetV2 0.35 successfully initialized for ESP32!")
    print(f"Model Input Shape: {model.input_shape}")
    print(f"Model Output Shape: {model.output_shape}")
except Exception as e:
    print("\nSetup Error:", e)
