import os
import time
import numpy as np
import tensorflow as tf
from PIL import Image

# Force clean execution engines
os.environ['TF_ENABLE_ONEDNN_OPTS'] = '0'
os.environ['TF_USE_LEGACY_KERAS'] = '1'

# CONFIGURATION
DATASET_DIR = "dataset/train"
REAL_LIFE_DIR = "./new_test_data"
IMG_SIZE = (96, 96)
BATCH_SIZE = 32
EPOCHS = 60  
NUM_CLASSES = 5  
class_names = ['up', 'down', 'right', 'left', 'null']
label_map = {'up': 0, 'down': 1, 'right': 2, 'left': 3, 'null': 4}
SAVE_PLOTS = os.environ.get("SAVE_PLOTS", "0") == "1"

if SAVE_PLOTS:
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        plt = None
        print("matplotlib is not installed; training plots will be skipped.")
else:
    plt = None

PHASE_1_EPOCHS = 20
PHASE_2_EPOCHS = 40

# ==========================================
# 1. DATASET PREPARATION & SPLITTING
# ==========================================
print("Preparing datasets (using Unicode-safe custom loader)...")
normalization_layer = tf.keras.layers.Rescaling(1./255)

def load_dataset_from_directory_custom(directory):
    x_data, y_data = [], []
    for label_name in class_names:
        dir_path = os.path.join(directory, label_name)
        if not os.path.isdir(dir_path):
            continue
        for fname in os.listdir(dir_path):
            if fname.lower().endswith(('.png', '.jpg', '.jpeg')):
                img_path = os.path.join(dir_path, fname)
                try:
                    img = Image.open(img_path).convert('L').resize(IMG_SIZE)
                    x_data.append(np.array(img, dtype=np.float32))
                    y_data.append(label_map[label_name])
                except Exception as e:
                    print(f"Error loading image {img_path}: {e}")
    return np.expand_dims(np.array(x_data), axis=-1), np.array(y_data, dtype=np.int32)

x_all, y_all = load_dataset_from_directory_custom(DATASET_DIR)

# Shuffle with a fixed seed
np.random.seed(123)
indices = np.arange(len(x_all))
np.random.shuffle(indices)
x_all = x_all[indices]
y_all = y_all[indices]

# Split (80% train, 20% validation)
split_idx = int(len(x_all) * 0.8)
x_train, x_val = x_all[:split_idx], x_all[split_idx:]
y_train, y_val = y_all[:split_idx], y_all[split_idx:]

print(f"Loaded {len(x_train)} training samples and {len(x_val)} validation samples.")

# Build training dataset pipeline
raw_train_ds = tf.data.Dataset.from_tensor_slices((x_train, y_train)).batch(BATCH_SIZE)
train_ds = raw_train_ds.map(lambda x, y: (normalization_layer(x), y))

# Data augmentation sequential model
data_augmentation = tf.keras.Sequential([
    tf.keras.layers.RandomRotation(factor=0.03, fill_mode="reflect"),
    tf.keras.layers.RandomTranslation(height_factor=0.05, width_factor=0.05, fill_mode="reflect"),
    tf.keras.layers.RandomZoom(height_factor=0.05, width_factor=0.05, fill_mode="reflect"),
])

train_ds = train_ds.cache().shuffle(1000)
train_ds = train_ds.map(lambda x, y: (data_augmentation(x, training=True), y), num_parallel_calls=tf.data.AUTOTUNE)
train_ds = train_ds.prefetch(buffer_size=tf.data.AUTOTUNE)

# Build validation dataset pipeline
raw_val_ds = tf.data.Dataset.from_tensor_slices((x_val, y_val)).batch(BATCH_SIZE)
val_clean_ds = raw_val_ds.map(lambda x, y: (normalization_layer(x), y)).cache().prefetch(buffer_size=tf.data.AUTOTUNE)

def load_real_life_data():
    x_real, y_real = [], []
    if not os.path.exists(REAL_LIFE_DIR):
        return None, None
    for label_name in os.listdir(REAL_LIFE_DIR):
        dir_path = os.path.join(REAL_LIFE_DIR, label_name)
        if not os.path.isdir(dir_path): continue
        for fname in os.listdir(dir_path):
            if fname.lower().endswith(('.png', '.jpg', '.jpeg')):
                img = Image.open(os.path.join(dir_path, fname)).convert('L').resize(IMG_SIZE)
                x_real.append(np.array(img, dtype=np.float32) / 255.0)
                y_real.append(label_map[label_name])
    return np.expand_dims(np.array(x_real), axis=-1), np.array(y_real)

x_real, y_real = load_real_life_data()

# ==========================================
# 2. MODEL ARCHITECTURES
# ==========================================

# --- CATEGORY A: TRAINED FROM SCRATCH ---
def get_baseline_model():
    model = tf.keras.Sequential([
        tf.keras.layers.InputLayer(input_shape=(96, 96, 1)),
        tf.keras.layers.GaussianNoise(0.05),
        tf.keras.layers.Conv2D(16, (3, 3), activation='relu', padding='same'),
        tf.keras.layers.MaxPooling2D(2, 2),
        tf.keras.layers.Conv2D(32, (3, 3), activation='relu', padding='same'),
        tf.keras.layers.MaxPooling2D(2, 2),
        tf.keras.layers.Flatten(),
        tf.keras.layers.Dropout(0.5),
        tf.keras.layers.Dense(NUM_CLASSES, activation='softmax')
    ])
    return model, None

def get_separable_model():
    model = tf.keras.Sequential([
        tf.keras.layers.InputLayer(input_shape=(96, 96, 1)),
        tf.keras.layers.GaussianNoise(0.05),
        tf.keras.layers.SeparableConv2D(16, (3, 3), activation='relu', padding='same'),
        tf.keras.layers.MaxPooling2D(2, 2),
        tf.keras.layers.SeparableConv2D(32, (3, 3), activation='relu', padding='same'),
        tf.keras.layers.MaxPooling2D(2, 2),
        tf.keras.layers.Flatten(),
        tf.keras.layers.Dropout(0.5),
        tf.keras.layers.Dense(NUM_CLASSES, activation='softmax')
    ])
    return model, None

def get_mini_resnet_model():
    inputs = tf.keras.Input(shape=(96, 96, 1))
    x = tf.keras.layers.GaussianNoise(0.05)(inputs)
    
    x = tf.keras.layers.Conv2D(16, (3, 3), padding='same', activation='relu')(x)
    shortcut = x
    
    x = tf.keras.layers.Conv2D(16, (3, 3), padding='same', activation='relu')(x)
    x = tf.keras.layers.Conv2D(16, (3, 3), padding='same')(x)
    x = tf.keras.layers.add([x, shortcut])
    x = tf.keras.layers.Activation('relu')(x)
    x = tf.keras.layers.MaxPooling2D(2, 2)(x)
    
    shortcut = tf.keras.layers.Conv2D(32, (1, 1), padding='same')(x)
    x = tf.keras.layers.Conv2D(32, (3, 3), padding='same', activation='relu')(x)
    x = tf.keras.layers.Conv2D(32, (3, 3), padding='same')(x)
    x = tf.keras.layers.add([x, shortcut])
    x = tf.keras.layers.Activation('relu')(x)
    x = tf.keras.layers.MaxPooling2D(2, 2)(x)
    
    x = tf.keras.layers.Flatten()(x)
    x = tf.keras.layers.Dropout(0.5)(x)
    outputs = tf.keras.layers.Dense(NUM_CLASSES, activation='softmax')(x)
    model = tf.keras.Model(inputs, outputs)
    return model, None


# --- CATEGORY B: TRANSFER LEARNING / FINE-TUNING ---
# These models load pre-trained ImageNet weights, freeze the base network, 
# and train only the new classification head for your 5 specific gesture classes.

def get_mobilenet_v1_model():
    base_model = tf.keras.applications.MobileNet(
        input_shape=(96, 96, 3),
        alpha=0.25,
        include_top=False,
        weights='imagenet'
    )
    base_model.trainable = False  # Start frozen
    
    inputs = tf.keras.Input(shape=(96, 96, 1))
    x = tf.keras.layers.GaussianNoise(0.05)(inputs)
    x = tf.keras.layers.Lambda(lambda img: tf.image.grayscale_to_rgb(img))(x)
    x = tf.keras.layers.Lambda(lambda img: (img * 2.0) - 1.0)(x)
    
    x = base_model(x, training=False)
    x = tf.keras.layers.GlobalAveragePooling2D()(x)
    x = tf.keras.layers.Dropout(0.5)(x)
    outputs = tf.keras.layers.Dense(NUM_CLASSES, activation='softmax')(x)
    
    return tf.keras.Model(inputs, outputs), base_model

def get_mobilenet_v2_model():
    base_model = tf.keras.applications.MobileNetV2(
        input_shape=(96, 96, 3),
        alpha=0.35,
        include_top=False,
        weights='imagenet'
    )
    base_model.trainable = False  # Start frozen
    
    inputs = tf.keras.Input(shape=(96, 96, 1))
    x = tf.keras.layers.GaussianNoise(0.05)(inputs)
    x = tf.keras.layers.Lambda(lambda img: tf.image.grayscale_to_rgb(img))(x)
    x = tf.keras.layers.Lambda(lambda img: (img * 2.0) - 1.0)(x)
    
    x = base_model(x, training=False)
    x = tf.keras.layers.GlobalAveragePooling2D()(x)
    x = tf.keras.layers.Dropout(0.5)(x)
    outputs = tf.keras.layers.Dense(NUM_CLASSES, activation='softmax')(x)
    
    return tf.keras.Model(inputs, outputs), base_model

# ==========================================
# 3. QUANTIZATION & EVALUATION UTILITIES
# ==========================================
def quantize_and_save(keras_model, filename):
    def representative_data_gen():
        for input_value, _ in train_ds.unbatch().take(100):
            yield [tf.expand_dims(input_value, 0)]
    converter = tf.lite.TFLiteConverter.from_keras_model(keras_model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_data_gen
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    tflite_model = converter.convert()
    with open(filename, 'wb') as f:
        f.write(tflite_model)

def evaluate_tflite(model_path, dataset_type, ds_input=None, label_input=None):
    interpreter = tf.lite.Interpreter(model_path=model_path)
    interpreter.allocate_tensors()
    input_det = interpreter.get_input_details()[0]
    output_det = interpreter.get_output_details()[0]
    scale, zero_point = input_det['quantization']
    
    correct, total = 0, 0
    
    if dataset_type == 'tf_ds':
        iterator = ds_input.unbatch()
    else:
        if ds_input is None or len(ds_input) == 0: return "N/A"
        iterator = zip(ds_input, label_input)
        
    for img, label in iterator:
        img_np = img.numpy() if dataset_type == 'tf_ds' else img
        lbl_np = label.numpy() if dataset_type == 'tf_ds' else label
        
        q_img = np.clip(np.round((img_np / scale) + zero_point), -128, 127).astype(np.int8)
        q_img = np.expand_dims(q_img, axis=0)
        
        interpreter.set_tensor(input_det['index'], q_img)
        interpreter.invoke()
        pred = np.argmax(interpreter.get_tensor(output_det['index']))
        
        if pred == lbl_np: correct += 1
        total += 1
        
    return f"{(correct / total) * 100:.2f}%"

def profile_latency(model_path):
    interpreter = tf.lite.Interpreter(model_path=model_path)
    interpreter.allocate_tensors()
    input_det = interpreter.get_input_details()[0]
    dummy_input = np.random.randint(-128, 127, size=input_det['shape'], dtype=np.int8)
    
    interpreter.set_tensor(input_det['index'], dummy_input)
    interpreter.invoke()
    
    runs = 200
    start = time.time()
    for _ in range(runs):
        interpreter.set_tensor(input_det['index'], dummy_input)
        interpreter.invoke()
    return f"{((time.time() - start) / runs) * 1000:.3f} ms"

# ==========================================
# 4. EXECUTION LOOP (TWO-PHASE TRAINING)
# ==========================================
results = {}

# Pass function references, not executed instances
models_to_test = {
    "Baseline_CNN": get_baseline_model,
    "Separable_CNN": get_separable_model,
    "MobileNetV1_0.25": get_mobilenet_v1_model,
    "MobileNetV2_0.35": get_mobilenet_v2_model,
    "Mini_ResNet": get_mini_resnet_model
}

for name, model_fn in models_to_test.items():
    print(f"\n--- Processing Model: {name} ---")
    model, base_model = model_fn()
    
    # PHASE 1: Warmup / Standard Training
    print(f"  [Phase 1] Training for {PHASE_1_EPOCHS} epochs...")
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=0.0005),
        loss='sparse_categorical_crossentropy',
        metrics=['accuracy']
    )
    h1 = model.fit(train_ds, validation_data=val_clean_ds, epochs=PHASE_1_EPOCHS, verbose=0)
    
    # PHASE 2: True Fine-Tuning or Continuation
    if base_model is not None:
        print(f"  [Phase 2] Unfreezing top layers of {name} and lowering learning rate...")
        base_model.trainable = True
        
        # Keep early layers frozen, unfreeze the top ~30 layers
        fine_tune_at = len(base_model.layers) - 30
        for layer in base_model.layers[:fine_tune_at]:
            layer.trainable = False
            
        # Recompile is required to apply trainable changes, use low LR
        model.compile(
            optimizer=tf.keras.optimizers.Adam(learning_rate=1e-5),
            loss='sparse_categorical_crossentropy',
            metrics=['accuracy']
        )
        print(f"  [Phase 2] Fine-tuning for remaining {PHASE_2_EPOCHS} epochs...")
        h2 = model.fit(train_ds, validation_data=val_clean_ds, epochs=PHASE_2_EPOCHS, verbose=0)
    else:
        # Custom models do not need recompiling, just continue training seamlessly
        print(f"  [Phase 2] Continuing standard training for remaining {PHASE_2_EPOCHS} epochs...")
        h2 = model.fit(train_ds, validation_data=val_clean_ds, epochs=PHASE_2_EPOCHS, verbose=0)
        
    # Ensure models directory exists
    os.makedirs("models", exist_ok=True)
    tflite_name = f"models/{name}_int8.tflite"
    tflite_float_name = f"models/{name}_float32.tflite"
    keras_source_name = f"models/{name}.keras"
    saved_model_name = f"models/{name}_saved_model"
    
    print(f"  Saving Keras source model for deploy calibration: {keras_source_name} ...")
    try:
        model.save(keras_source_name)
        print(f"  Keras source model saved at {keras_source_name}")
    except Exception as e:
        print(f"  Failed to save Keras source model for {name}: {e}")
        print(f"  Trying SavedModel export instead: {saved_model_name} ...")
        try:
            if hasattr(model, "export"):
                model.export(saved_model_name)
            else:
                tf.saved_model.save(model, saved_model_name)
            print(f"  SavedModel source model saved at {saved_model_name}")
        except Exception as e2:
            print(f"  Failed to save SavedModel source model for {name}: {e2}")
    
    print(f"  Saving float32 TFLite model for {name}...")
    try:
        converter_float = tf.lite.TFLiteConverter.from_keras_model(model)
        tflite_float_model = converter_float.convert()
        with open(tflite_float_name, 'wb') as f:
            f.write(tflite_float_model)
        print(f"  Float32 TFLite model saved at {tflite_float_name}")
    except Exception as e:
        print(f"  Failed to save float32 model for {name}: {e}")
        
    print(f"  Quantizing {name}...")
    try:
        quantize_and_save(model, tflite_name)
        acc_clean = evaluate_tflite(tflite_name, 'tf_ds', ds_input=val_clean_ds)
        acc_real = evaluate_tflite(tflite_name, 'numpy', ds_input=x_real, label_input=y_real)
        latency = profile_latency(tflite_name)
        size = f"{os.path.getsize(tflite_name) / 1024:.1f} KB"
        print("  Evaluation Complete.")
    except Exception as e:
        acc_clean, acc_real, latency, size = "ERR", "ERR", "ERR", "ERR"
        print(f"  Failed to process {name}: {e}")
        
    # Optional plotting. Disabled by default so model export does not require
    # matplotlib in the deploy/model handoff environment.
    if SAVE_PLOTS and plt is not None:
        try:
            loss = h1.history['loss'] + h2.history['loss']
            val_loss = h1.history['val_loss'] + h2.history['val_loss']
            acc = h1.history['accuracy'] + h2.history['accuracy']
            val_acc = h1.history['val_accuracy'] + h2.history['val_accuracy']
            
            os.makedirs("plots", exist_ok=True)
            fig, ax1 = plt.subplots(figsize=(8, 5))
            
            # Plot Loss (Left Axis)
            color_loss = 'tab:red'
            ax1.set_xlabel('Epoch')
            ax1.set_ylabel('Loss', color=color_loss)
            line1 = ax1.plot(loss, color='tab:red', linestyle='-', label='Train Loss')
            line2 = ax1.plot(val_loss, color='coral', linestyle='--', label='Val Loss')
            ax1.tick_params(axis='y', labelcolor=color_loss)
            ax1.grid(True)
            
            # Plot Accuracy (Right Axis)
            ax2 = ax1.twinx()
            color_acc = 'tab:blue'
            ax2.set_ylabel('Accuracy', color=color_acc)
            line3 = ax2.plot(acc, color='tab:blue', linestyle='-', label='Train Acc')
            line4 = ax2.plot(val_acc, color='skyblue', linestyle='--', label='Val Acc')
            ax2.tick_params(axis='y', labelcolor=color_acc)
            
            # Phase 2 start vertical line
            v_line = ax1.axvline(x=PHASE_1_EPOCHS - 1, color='gray', linestyle=':', label='Phase 2 Start')
            
            # Combined Legends
            lines = line1 + line2 + line3 + line4 + [v_line]
            labels = [l.get_label() for l in lines]
            ax1.legend(lines, labels, loc='upper left')
            
            plt.title(f'{name} - Training Curves')
            plt.tight_layout()
            
            plot_path = f"plots/{name}_training_curves.png"
            plt.savefig(plot_path)
            plt.close()
            print(f"  Saved dual-axis training curves to {plot_path}")
        except Exception as e:
            print(f"  Failed to plot training curves for {name}: {e}")
    else:
        print(f"  Skipping training curve plot for {name}. Set SAVE_PLOTS=1 to enable.")
    
    results[name] = {
        "Clean Acc": acc_clean,
        "Real Acc": acc_real,
        "PC Latency": latency,
        "Size": size
    }

# ==========================================
# 5. PRINT RESULTS COMPARISON
# ==========================================
print("\n" + "="*82)
print(f"{'Model Architecture':<22} | {'Clean Val Acc':<15} | {'Real Acc':<10} | {'PC Latency':<12} | {'File Size'}")
print("="*82)
for name, metrics in results.items():
    print(f"{name:<22} | {metrics['Clean Acc']:<15} | {metrics['Real Acc']:<10} | {metrics['PC Latency']:<12} | {metrics['Size']}")
print("="*82)
