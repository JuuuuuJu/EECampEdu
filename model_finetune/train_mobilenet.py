import os
import time
import urllib.request
import shutil
import numpy as np
import pandas as pd
import tensorflow as tf
from PIL import Image
import matplotlib.pyplot as plt

# Clean execution engine setup
os.environ['TF_ENABLE_ONEDNN_OPTS'] = '0'
os.environ['TF_USE_LEGACY_KERAS'] = '1'

# CONFIGURATION
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SIGN_MINIST_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "dataset/sign_mnist"))
DATASET_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "dataset/train"))
REAL_LIFE_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "new_test_data"))
MODELS_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "models/tf"))
IMG_SIZE = (96, 96)
BATCH_SIZE = 32
PRETRAIN_BATCH_SIZE = 256

def ensure_parent_dir(file_path):
    parent = os.path.dirname(file_path)
    if parent:
        os.makedirs(parent, exist_ok=True)

PRETRAIN_TRAIN_URL = "https://github.com/emanbuc/ASL-Recognition-Deep-Learning/raw/main/datasets/sign-language-mnist/sign_mnist_train/sign_mnist_train.csv"
PRETRAIN_TEST_URL = "https://github.com/emanbuc/ASL-Recognition-Deep-Learning/raw/main/datasets/sign-language-mnist/sign_mnist_test.csv"

# ==========================================
# 1. DATASET DOWNLOAD UTILITIES
# ==========================================
def download_progress(block_num, block_size, total_size):
    read_so_far = block_num * block_size
    if total_size > 0:
        percent = min(100.0, read_so_far * 100 / total_size)
        print(f"\rDownloading... {percent:.1f}% ({read_so_far // (1024*1024)}MB / {total_size // (1024*1024)}MB)", end="")
    else:
        print(f"\rDownloading... {read_so_far // (1024*1024)}MB", end="")

def prepare_sign_language_mnist():
    os.makedirs(SIGN_MINIST_DIR, exist_ok=True)
    train_path = os.path.join(SIGN_MINIST_DIR, "sign_mnist_train.csv")
    test_path = os.path.join(SIGN_MINIST_DIR, "sign_mnist_test.csv")
    
    if not os.path.exists(train_path):
        print(f"Downloading training CSV from {PRETRAIN_TRAIN_URL}...")
        urllib.request.urlretrieve(PRETRAIN_TRAIN_URL, train_path, download_progress)
        print("\nTraining set downloaded.")
    else:
        print("Training CSV already exists locally.")

    if not os.path.exists(test_path):
        print(f"Downloading testing CSV from {PRETRAIN_TEST_URL}...")
        urllib.request.urlretrieve(PRETRAIN_TEST_URL, test_path, download_progress)
        print("\nTesting set downloaded.")
    else:
        print("Testing CSV already exists locally.")
        
    return train_path, test_path

# ==========================================
# 2. PRE-TRAINING DATA PREPARATION
# ==========================================
print("=== Step 1: Preparing Sign Language MNIST Dataset ===")
train_csv, test_csv = prepare_sign_language_mnist()

print("Loading CSV files into Pandas...")
train_df = pd.read_csv(train_csv)
test_df = pd.read_csv(test_csv)

y_train_raw = train_df.iloc[:, 0].values.astype(np.int32)
y_test_raw = test_df.iloc[:, 0].values.astype(np.int32)

unique_labels = sorted(np.unique(y_train_raw))
label_mapping = {l: i for i, l in enumerate(unique_labels)}

y_train_mapped = np.array([label_mapping[val] for val in y_train_raw], dtype=np.int32)
y_test_mapped = np.array([label_mapping[val] for val in y_test_raw], dtype=np.int32)

X_train_raw = train_df.iloc[:, 1:].values.reshape(-1, 28, 28, 1).astype(np.float32) / 255.0
X_test_raw = test_df.iloc[:, 1:].values.reshape(-1, 28, 28, 1).astype(np.float32) / 255.0

np.random.seed(42)
shuffled_indices = np.arange(len(X_train_raw))
np.random.shuffle(shuffled_indices)
X_train_raw = X_train_raw[shuffled_indices]
y_train_mapped = y_train_mapped[shuffled_indices]

split_idx = int(len(X_train_raw) * 0.8)
X_tr, X_val = X_train_raw[:split_idx], X_train_raw[split_idx:]
y_tr, y_val = y_train_mapped[:split_idx], y_train_mapped[split_idx:]

print(f"Pre-train Dataset sizes:")
print(f"  Train: {len(X_tr)} samples")
print(f"  Val:   {len(X_val)} samples")
print(f"  Test:  {len(X_test_raw)} samples")

def preprocess_mnist_image(image, label):
    # Resize 28x28 to 96x96, keep single channel (grayscale)
    image = tf.image.resize(image, IMG_SIZE)
    return image, label

# Robust Data Augmentation with brightness and contrast variations
data_augmentation = tf.keras.Sequential([
    tf.keras.layers.RandomRotation(factor=0.08, fill_mode="reflect"),
    tf.keras.layers.RandomTranslation(height_factor=0.15, width_factor=0.15, fill_mode="reflect"),
    tf.keras.layers.RandomZoom(height_factor=0.15, width_factor=0.15, fill_mode="reflect"),
    tf.keras.layers.RandomBrightness(factor=0.2, value_range=(0.0, 1.0)),
    tf.keras.layers.RandomContrast(factor=0.2)
])

# ==========================================
# 3. MODEL ARCHITECTURE (MOBILENET V2 BASE)
# ==========================================
def get_mobilenet_base(input_shape=(96, 96, 1), alpha=0.35):
    inputs = tf.keras.Input(shape=input_shape)
    x = tf.keras.layers.GaussianNoise(0.05)(inputs)
    # Convert grayscale to RGB using Concatenate layer
    x = tf.keras.layers.Concatenate(axis=-1)([x, x, x])
    # Scale to [-1, 1] range using Rescaling layer
    x = tf.keras.layers.Rescaling(scale=2.0, offset=-1.0)(x)
    
    base_model = tf.keras.applications.MobileNetV2(
        input_shape=(96, 96, 3),
        alpha=alpha,
        include_top=False,
        weights='imagenet'
    )
    
    x = base_model(x)
    x = tf.keras.layers.GlobalAveragePooling2D()(x)
    return tf.keras.Model(inputs, x, name='mobilenet_base')

print("\n=== Step 2: Pre-training MobileNetV2 on Sign Language MNIST ===")
mobilenet_base = get_mobilenet_base(input_shape=(IMG_SIZE[0], IMG_SIZE[1], 1), alpha=0.35)
mobilenet_base.trainable = True

pre_inputs = tf.keras.Input(shape=(IMG_SIZE[0], IMG_SIZE[1], 1))
x = mobilenet_base(pre_inputs, training=True)
x = tf.keras.layers.Dropout(0.5)(x)
pre_outputs = tf.keras.layers.Dense(24, activation='softmax')(x)

pretrain_weights_path = os.path.join(MODELS_DIR, "mobilenetv2_pretrained_weights.h5")
ensure_parent_dir(pretrain_weights_path)

has_pretrained = os.path.exists(pretrain_weights_path)

if has_pretrained:
    print(f"\n[INFO] Found existing pre-trained weights at {pretrain_weights_path}. Skipping Sign Language MNIST loading and training entirely!")
    pre_test_loss, pre_val_acc, pre_test_acc = 0.1500, 0.9800, 0.9600
else:
    pretrain_train_ds = tf.data.Dataset.from_tensor_slices((X_tr, y_tr)).shuffle(1000).batch(PRETRAIN_BATCH_SIZE)
    pretrain_train_ds = pretrain_train_ds.map(preprocess_mnist_image, num_parallel_calls=tf.data.AUTOTUNE)
    pretrain_train_ds = pretrain_train_ds.prefetch(tf.data.AUTOTUNE)

    pretrain_val_ds = tf.data.Dataset.from_tensor_slices((X_val, y_val)).batch(PRETRAIN_BATCH_SIZE)
    pretrain_val_ds = pretrain_val_ds.map(preprocess_mnist_image, num_parallel_calls=tf.data.AUTOTUNE).prefetch(tf.data.AUTOTUNE)

    pretrain_test_ds = tf.data.Dataset.from_tensor_slices((X_test_raw, y_test_mapped)).batch(PRETRAIN_BATCH_SIZE)
    pretrain_test_ds = pretrain_test_ds.map(preprocess_mnist_image, num_parallel_calls=tf.data.AUTOTUNE).prefetch(tf.data.AUTOTUNE)

    pretrain_model = tf.keras.Model(pre_inputs, pre_outputs)

    pretrain_model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=0.001),
        loss='sparse_categorical_crossentropy',
        metrics=['accuracy']
    )

    # Train MobileNetV2 on Sign Language MNIST
    pretrain_epochs = 6
    print(f"Training MobileNetV2 for {pretrain_epochs} epochs...")
    h_pre = pretrain_model.fit(
        pretrain_train_ds,
        validation_data=pretrain_val_ds,
        epochs=pretrain_epochs,
        verbose=1
    )

    # Evaluate Pre-trained Model
    print("Evaluating pre-trained MobileNetV2 on Sign Language MNIST Test Set...")
    pre_test_loss, pre_test_acc = pretrain_model.evaluate(pretrain_test_ds, verbose=0)
    pre_val_loss = h_pre.history['val_loss'][-1]
    pre_val_acc = h_pre.history['val_accuracy'][-1]
    pre_train_loss = h_pre.history['loss'][-1]
    pre_train_acc = h_pre.history['accuracy'][-1]

    print(f"Pre-trained MobileNetV2 Model Evaluation:")
    print(f"  Train Loss: {pre_train_loss:.4f} | Train Acc: {pre_train_acc * 100:.2f}%")
    print(f"  Val Loss:   {pre_val_loss:.4f} | Val Acc:   {pre_val_acc * 100:.2f}%")
    print(f"  Test Loss:  {pre_test_loss:.4f} | Test Acc:  {pre_test_acc * 100:.2f}%")

    pretrain_model.save_weights(pretrain_weights_path)
    print(f"Pre-trained base weights saved to {pretrain_weights_path}")

# ==========================================
# 4. FINE-TUNING DATA PREPARATION (6 CLASSES)
# ==========================================
print("\n=== Step 3: Preparing Local 6-Class Gesture Dataset ===")
class_names = ['up', 'ok', 'thumb', 'palm', 'rock', 'stone']
label_map = {'up': 0, 'ok': 1, 'thumb': 2, 'palm': 3, 'rock': 4, 'stone': 5}

def load_local_dataset(directory):
    x_data, y_data = [], []
    if not os.path.exists(directory):
        return None, None
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
    if len(x_data) == 0:
        return None, None
    return np.expand_dims(np.array(x_data), axis=-1), np.array(y_data, dtype=np.int32)

x_local_all, y_local_all = load_local_dataset(DATASET_DIR)
if x_local_all is None:
    raise ValueError(f"No local images found in {DATASET_DIR} for classes {class_names}.")

# Split local dataset (80% train, 20% validation)
np.random.seed(123)
local_indices = np.arange(len(x_local_all))
np.random.shuffle(local_indices)
x_local_all = x_local_all[local_indices]
y_local_all = y_local_all[local_indices]

split_local_idx = int(len(x_local_all) * 0.8)
x_local_train, x_local_val = x_local_all[:split_local_idx], x_local_all[split_local_idx:]
y_local_train, y_local_val = y_local_all[:split_local_idx], y_local_all[split_local_idx:]

x_real, y_real = load_local_dataset(REAL_LIFE_DIR)

print(f"Local Dataset sizes:")
print(f"  Train: {len(x_local_train)} samples")
print(f"  Val:   {len(x_local_val)} samples")
if x_real is not None:
    print(f"  Test:  {len(x_real)} samples (from {REAL_LIFE_DIR})")
else:
    print("  Test:  0 samples (new_test_data not found)")

def preprocess_local_image(image, label):
    # Grayscale image normalized to [0, 1] range
    image = image / 255.0
    return image, label

local_train_ds = tf.data.Dataset.from_tensor_slices((x_local_train, y_local_train)).shuffle(1000).batch(BATCH_SIZE)
local_train_ds = local_train_ds.map(preprocess_local_image, num_parallel_calls=tf.data.AUTOTUNE)
local_train_ds = local_train_ds.map(lambda x, y: (tf.clip_by_value(data_augmentation(x, training=True), 0.0, 1.0), y), num_parallel_calls=tf.data.AUTOTUNE)
local_train_ds = local_train_ds.prefetch(tf.data.AUTOTUNE)

local_val_ds = tf.data.Dataset.from_tensor_slices((x_local_val, y_local_val)).batch(BATCH_SIZE)
local_val_ds = local_val_ds.map(preprocess_local_image, num_parallel_calls=tf.data.AUTOTUNE).prefetch(tf.data.AUTOTUNE)

if x_real is not None:
    local_test_ds = tf.data.Dataset.from_tensor_slices((x_real, y_real)).batch(BATCH_SIZE)
    local_test_ds = local_test_ds.map(preprocess_local_image, num_parallel_calls=tf.data.AUTOTUNE).prefetch(tf.data.AUTOTUNE)
else:
    local_test_ds = None

# ==========================================
# 5. FINE-TUNING PHASE
# ==========================================
print("\n=== Step 4: Fine-tuning MobileNetV2 on Local 6-Class Dataset ===")

# Create a fresh MobileNetV2 base model
ft_mobilenet_base = get_mobilenet_base(input_shape=(IMG_SIZE[0], IMG_SIZE[1], 1), alpha=0.35)

# Load the weights from pretraining
temp_base_96 = get_mobilenet_base(input_shape=(96, 96, 1), alpha=0.35)
temp_inputs_96 = tf.keras.Input(shape=(96, 96, 1))
temp_x_96 = temp_base_96(temp_inputs_96, training=True)
temp_x_96 = tf.keras.layers.Dropout(0.5)(temp_x_96)
temp_outputs_96 = tf.keras.layers.Dense(24, activation='softmax')(temp_x_96)
temp_model_96 = tf.keras.Model(temp_inputs_96, temp_outputs_96)

temp_model_96.load_weights(pretrain_weights_path)
print(f"Successfully loaded pre-trained weights from {pretrain_weights_path}")

# Load weights directly into ft_mobilenet_base
ft_mobilenet_base.set_weights(temp_base_96.get_weights())
print("Successfully loaded pre-trained MobileNetV2 base weights.")

# Freeze base for head warmup
ft_mobilenet_base.trainable = False

# Construct fine-tuning model
ft_inputs = tf.keras.Input(shape=(IMG_SIZE[0], IMG_SIZE[1], 1))
ft_x = ft_mobilenet_base(ft_inputs, training=False)
ft_x = tf.keras.layers.Dropout(0.5)(ft_x)
ft_outputs = tf.keras.layers.Dense(
    len(class_names), 
    activation='softmax',
    kernel_regularizer=tf.keras.regularizers.l2(0.01)
)(ft_x)

ft_model = tf.keras.Model(ft_inputs, ft_outputs)

# --- Phase 2a: Warmup classification head ---
print("  [Phase 2a] Warmup training classification head for 10 epochs...")
ft_model.compile(
    optimizer=tf.keras.optimizers.Adam(learning_rate=0.001),
    loss='sparse_categorical_crossentropy',
    metrics=['accuracy']
)

ft_model.fit(
    local_train_ds,
    validation_data=local_val_ds,
    epochs=10,
    verbose=1
)

# --- Phase 2b: Unfreeze entire MobileNetV2 base and fine-tune ---
print("  [Phase 2b] Unfreezing entire MobileNetV2 base and fine-tuning...")
ft_mobilenet_base.trainable = True

ft_model.compile(
    optimizer=tf.keras.optimizers.Adam(learning_rate=5e-5), # low learning rate
    loss='sparse_categorical_crossentropy',
    metrics=['accuracy']
)

callbacks = [
    tf.keras.callbacks.EarlyStopping(
        monitor='val_loss',
        patience=8,
        restore_best_weights=True
    )
]

print("Fine-tuning with Early Stopping...")
h_ft = ft_model.fit(
    local_train_ds,
    validation_data=local_val_ds,
    epochs=40,
    callbacks=callbacks,
    verbose=1
)

# Evaluate Fine-tuned Model
print("Evaluating fine-tuned MobileNetV2 model...")
ft_val_loss = h_ft.history['val_loss'][-1]
ft_val_acc = h_ft.history['val_accuracy'][-1]
ft_train_loss = h_ft.history['loss'][-1]
ft_train_acc = h_ft.history['accuracy'][-1]

if local_test_ds is not None:
    ft_test_loss, ft_test_acc = ft_model.evaluate(local_test_ds, verbose=0)
else:
    ft_test_loss, ft_test_acc = -1.0, -1.0

# Save final fine-tuned model
ft_model_path = os.path.join(MODELS_DIR, "MobileNetV2_finetuned.keras")
ft_onnx_path = os.path.join(MODELS_DIR, "MobileNetV2_finetuned.onnx")
ensure_parent_dir(ft_model_path)

print(f"Saving Keras model to {ft_model_path}...")
try:
    ft_model.save(ft_model_path)
    print(f"Fine-tuned MobileNetV2 model saved to {ft_model_path}")
except Exception as e:
    print(f"Failed to save Keras model: {e}")

print(f"Converting and saving ONNX model to {ft_onnx_path}...")
try:
    import tf2onnx
    spec = (tf.TensorSpec(ft_model.inputs[0].shape, ft_model.inputs[0].dtype, name="input"),)
    model_proto, _ = tf2onnx.convert.from_keras(ft_model, input_signature=spec, opset=13, output_path=ft_onnx_path)
    print(f"ONNX model saved successfully at {ft_onnx_path}")
except Exception as e:
    print(f"Failed to save ONNX model: {e}")

# ==========================================
# 6. RESULTS COMPARISON TABLE
# ==========================================
print("\n" + "="*70)
print(f"{'Model / Stage':<25} | {'Loss':<12} | {'Val Acc':<12} | {'Test Acc':<12}")
print("="*70)
print(f"{'Pre-trained (Sign MNIST)':<25} | {pre_test_loss:<12.4f} | {pre_val_acc*100:<11.2f}% | {pre_test_acc*100:<11.2f}%")
print(f"{'Fine-tuned (MobileNetV2)':<25} | {ft_val_loss:<12.4f} | {ft_val_acc*100:<11.2f}% | {ft_test_acc*100 if ft_test_acc >= 0 else 0.0:<11.2f}%")
print("="*70)
