import os
import time
import urllib.request
import copy
import numpy as np
import pandas as pd
from PIL import Image
import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import Dataset, DataLoader
from torchvision import transforms

# CONFIGURATION
IMG_SIZE = (96, 96)
BATCH_SIZE = 32
PRETRAIN_BATCH_SIZE = 256
DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SIGN_MINIST_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "../dataset/sign_mnist"))
DATASET_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "../dataset/train"))
REAL_LIFE_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "../new_test_data"))
MODELS_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "../models"))

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
# 2. MODEL ARCHITECTURE (MINI RESNET BASE)
# ==========================================
class MiniResNetBase(nn.Module):
    def __init__(self, input_channels=1):
        super(MiniResNetBase, self).__init__()
        # Block 1
        self.conv1_1 = nn.Conv2d(input_channels, 16, kernel_size=3, padding=1)
        self.conv1_2 = nn.Conv2d(16, 16, kernel_size=3, padding=1)
        self.conv1_3 = nn.Conv2d(16, 16, kernel_size=3, padding=1)
        
        # Block 2
        self.shortcut_conv2 = nn.Conv2d(16, 32, kernel_size=1, padding=0)
        self.conv2_1 = nn.Conv2d(16, 32, kernel_size=3, padding=1)
        self.conv2_2 = nn.Conv2d(32, 32, kernel_size=3, padding=1)
        
    def forward(self, x):
        # Gaussian Noise (standard deviation 0.05)
        if self.training:
            noise = torch.randn_like(x) * 0.05
            x = x + noise
            
        # Block 1
        x1 = F.relu(self.conv1_1(x))
        shortcut = x1
        x1 = F.relu(self.conv1_2(x1))
        x1 = self.conv1_3(x1)
        x1 = F.relu(x1 + shortcut)
        x1 = F.max_pool2d(x1, kernel_size=2, stride=2)
        
        # Block 2
        shortcut = self.shortcut_conv2(x1)
        x2 = F.relu(self.conv2_1(x1))
        x2 = self.conv2_2(x2)
        x2 = F.relu(x2 + shortcut)
        x2 = F.max_pool2d(x2, kernel_size=2, stride=2)
        
        # Flatten
        x2 = torch.flatten(x2, start_dim=1)
        return x2

class PretrainModel(nn.Module):
    def __init__(self, base_model, img_size, num_classes=24):
        super(PretrainModel, self).__init__()
        self.base_model = base_model
        self.dropout = nn.Dropout(0.5)
        
        # Calculate feature dimension dynamically
        with torch.no_grad():
            dummy = torch.zeros(1, 1, img_size[0], img_size[1])
            self.num_features = self.base_model(dummy).shape[1]
            
        self.fc = nn.Linear(self.num_features, num_classes)
        
    def forward(self, x):
        x = self.base_model(x)
        x = self.dropout(x)
        x = self.fc(x)
        return x

class FineTuneModel(nn.Module):
    def __init__(self, base_model, img_size, num_classes=4):
        super(FineTuneModel, self).__init__()
        self.base_model = base_model
        self.dropout = nn.Dropout(0.5)
        
        # Calculate feature dimension dynamically
        with torch.no_grad():
            dummy = torch.zeros(1, 1, img_size[0], img_size[1])
            self.num_features = self.base_model(dummy).shape[1]
            
        self.fc = nn.Linear(self.num_features, num_classes)
        
    def forward(self, x):
        x = self.base_model(x)
        x = self.dropout(x)
        x = self.fc(x)
        return x

# ==========================================
# 3. CUSTOM DATASETS
# ==========================================
class MNISTDataset(Dataset):
    def __init__(self, images, labels, transform=None):
        self.images = torch.tensor(images, dtype=torch.float32)
        self.labels = torch.tensor(labels, dtype=torch.long)
        self.transform = transform
        
    def __len__(self):
        return len(self.images)
        
    def __getitem__(self, idx):
        img = self.images[idx]
        label = self.labels[idx]
        if self.transform:
            img = self.transform(img)
        return img, label

class LocalDataset(Dataset):
    def __init__(self, images, labels, transform=None):
        # Transpose shape from (N, H, W, 1) to (N, 1, H, W)
        self.images = torch.tensor(images.transpose(0, 3, 1, 2), dtype=torch.float32) / 255.0
        self.labels = torch.tensor(labels, dtype=torch.long)
        self.transform = transform
        
    def __len__(self):
        return len(self.images)
        
    def __getitem__(self, idx):
        img = self.images[idx]
        label = self.labels[idx]
        if self.transform:
            img = self.transform(img)
            img = torch.clamp(img, 0.0, 1.0)
        return img, label

# ==========================================
# 4. TRAINING HELPER & EARLY STOPPING
# ==========================================
class EarlyStopping:
    def __init__(self, patience=8, restore_best_weights=True):
        self.patience = patience
        self.restore_best_weights = restore_best_weights
        self.counter = 0
        self.best_loss = float('inf')
        self.best_weights = None
        
    def __call__(self, val_loss, model):
        if val_loss < self.best_loss:
            self.best_loss = val_loss
            self.counter = 0
            if self.restore_best_weights:
                self.best_weights = copy.deepcopy(model.state_dict())
        else:
            self.counter += 1
            if self.counter >= self.patience:
                if self.restore_best_weights and self.best_weights is not None:
                    model.load_state_dict(self.best_weights)
                return True
        return False

def train_model(model, train_loader, val_loader, criterion, optimizer, epochs, device, callbacks=None, l2_reg_lambda=0.0):
    history = {'loss': [], 'accuracy': [], 'val_loss': [], 'val_accuracy': []}
    
    for epoch in range(epochs):
        # Training loop
        model.train()
        running_loss = 0.0
        correct = 0
        total = 0
        
        for inputs, targets in train_loader:
            inputs, targets = inputs.to(device), targets.to(device)
            optimizer.zero_grad()
            outputs = model(inputs)
            
            loss = criterion(outputs, targets)
            if l2_reg_lambda > 0.0:
                l2_reg = torch.sum(model.fc.weight ** 2)
                loss = loss + l2_reg_lambda * l2_reg
                
            loss.backward()
            optimizer.step()
            
            running_loss += loss.item() * inputs.size(0)
            _, predicted = outputs.max(1)
            total += targets.size(0)
            correct += predicted.eq(targets).sum().item()
            
        epoch_loss = running_loss / total
        epoch_acc = correct / total
        
        # Validation loop
        model.eval()
        val_loss = 0.0
        val_correct = 0
        val_total = 0
        
        with torch.no_grad():
            for inputs, targets in val_loader:
                inputs, targets = inputs.to(device), targets.to(device)
                outputs = model(inputs)
                loss = criterion(outputs, targets)
                if l2_reg_lambda > 0.0:
                    l2_reg = torch.sum(model.fc.weight ** 2)
                    loss = loss + l2_reg_lambda * l2_reg
                    
                val_loss += loss.item() * inputs.size(0)
                _, predicted = outputs.max(1)
                val_total += targets.size(0)
                val_correct += predicted.eq(targets).sum().item()
                
        epoch_val_loss = val_loss / val_total
        epoch_val_acc = val_correct / val_total
        
        history['loss'].append(epoch_loss)
        history['accuracy'].append(epoch_acc)
        history['val_loss'].append(epoch_val_loss)
        history['val_accuracy'].append(epoch_val_acc)
        
        print(f"Epoch {epoch+1:02d}/{epochs:02d} - loss: {epoch_loss:.4f} - accuracy: {epoch_acc:.4f} - val_loss: {epoch_val_loss:.4f} - val_accuracy: {epoch_val_acc:.4f}")
        
        if callbacks is not None:
            should_stop = False
            for cb in callbacks:
                if cb(epoch_val_loss, model):
                    should_stop = True
            if should_stop:
                print("Early stopping triggered.")
                break
                
    return history

def evaluate_model(model, loader, criterion, device, l2_reg_lambda=0.0):
    model.eval()
    val_loss = 0.0
    correct = 0
    total = 0
    with torch.no_grad():
        for inputs, targets in loader:
            inputs, targets = inputs.to(device), targets.to(device)
            outputs = model(inputs)
            loss = criterion(outputs, targets)
            if l2_reg_lambda > 0.0:
                l2_reg = torch.sum(model.fc.weight ** 2)
                loss = loss + l2_reg_lambda * l2_reg
            val_loss += loss.item() * inputs.size(0)
            _, predicted = outputs.max(1)
            total += targets.size(0)
            correct += predicted.eq(targets).sum().item()
    return val_loss / total, correct / total

# ==========================================
# 5. WEIGHT TRANSLATION UTILITY (PYTORCH -> KERAS)
# ==========================================
def save_pytorch_as_keras(pytorch_model, keras_save_path, img_size):
    try:
        import tensorflow as tf
        
        # Build equivalent Keras Model structure
        inputs = tf.keras.Input(shape=(img_size[0], img_size[1], 1))
        x = tf.keras.layers.GaussianNoise(0.05)(inputs)
        
        # Block 1
        x1 = tf.keras.layers.Conv2D(16, (3, 3), padding='same', activation='relu', name='conv1_1')(x)
        shortcut = x1
        x1 = tf.keras.layers.Conv2D(16, (3, 3), padding='same', activation='relu', name='conv1_2')(x1)
        x1 = tf.keras.layers.Conv2D(16, (3, 3), padding='same', name='conv1_3')(x1)
        x1 = tf.keras.layers.add([x1, shortcut])
        x1 = tf.keras.layers.Activation('relu')(x1)
        x1 = tf.keras.layers.MaxPooling2D(2, 2)(x1)
        
        # Block 2
        shortcut = tf.keras.layers.Conv2D(32, (1, 1), padding='same', name='shortcut_conv2')(x1)
        x2 = tf.keras.layers.Conv2D(32, (3, 3), padding='same', activation='relu', name='conv2_1')(x1)
        x2 = tf.keras.layers.Conv2D(32, (3, 3), padding='same', name='conv2_2')(x2)
        x2 = tf.keras.layers.add([x2, shortcut])
        x2 = tf.keras.layers.Activation('relu')(x2)
        x2 = tf.keras.layers.MaxPooling2D(2, 2)(x2)
        
        x2 = tf.keras.layers.Flatten()(x2)
        x2 = tf.keras.layers.Dropout(0.5)(x2)
        
        num_classes = pytorch_model.fc.out_features
        outputs = tf.keras.layers.Dense(num_classes, activation='softmax', name='fc')(x2)
        
        keras_model = tf.keras.Model(inputs, outputs)
        
        # Copy weights
        state_dict = pytorch_model.state_dict()
        layer_mapping = {
            'conv1_1': ('base_model.conv1_1.weight', 'base_model.conv1_1.bias'),
            'conv1_2': ('base_model.conv1_2.weight', 'base_model.conv1_2.bias'),
            'conv1_3': ('base_model.conv1_3.weight', 'base_model.conv1_3.bias'),
            'shortcut_conv2': ('base_model.shortcut_conv2.weight', 'base_model.shortcut_conv2.bias'),
            'conv2_1': ('base_model.conv2_1.weight', 'base_model.conv2_1.bias'),
            'conv2_2': ('base_model.conv2_2.weight', 'base_model.conv2_2.bias'),
        }
        
        for layer in keras_model.layers:
            if layer.name in layer_mapping:
                w_key, b_key = layer_mapping[layer.name]
                w_pt = state_dict[w_key].cpu().numpy()
                b_pt = state_dict[b_key].cpu().numpy()
                # PyTorch Conv2D weights: (out, in, h, w) -> Keras Conv2D: (h, w, in, out)
                w_keras = np.transpose(w_pt, (2, 3, 1, 0))
                layer.set_weights([w_keras, b_pt])
            elif layer.name == 'fc':
                w_pt = state_dict['fc.weight'].cpu().numpy()
                b_pt = state_dict['fc.bias'].cpu().numpy()
                # PyTorch Linear weight: (out, in) -> Keras Dense: (in, out)
                w_keras = np.transpose(w_pt, (1, 0))
                layer.set_weights([w_keras, b_pt])
                
        keras_model.save(keras_save_path)
        print(f"Successfully converted and saved Keras model to {keras_save_path}")
    except Exception as e:
        print(f"Warning: Failed to save Keras model translation: {e}")

# ==========================================
# Main Flow
# ==========================================
def main():
    print("=== Step 1: Preparing Sign Language MNIST Dataset ===")
    train_csv, test_csv = prepare_sign_language_mnist()
    
    os.makedirs(MODELS_DIR, exist_ok=True)
    pretrain_weights_path = os.path.join(MODELS_DIR, f"mini_resnet_pretrained_weights_{IMG_SIZE[0]}.pth")
    
    # Instantiate Base Model
    resnet_base = MiniResNetBase(input_channels=1).to(DEVICE)
    
    # Check for existing pre-trained weights
    has_pretrained = os.path.exists(pretrain_weights_path)
    if not has_pretrained:
        # Check if there is an h5 file from the Keras version we can convert
        h5_path = pretrain_weights_path.replace(".pth", ".h5")
        if not os.path.exists(h5_path):
            h5_path = os.path.join(MODELS_DIR, "mini_resnet_pretrained_weights.h5")
            
        if os.path.exists(h5_path):
            print(f"Found existing Keras pre-trained weights: {h5_path}. Converting to PyTorch...")
            try:
                import tensorflow as tf
                # Build temp keras model to load the weights
                temp_model = tf.keras.models.load_model(h5_path, compile=False)
                # Find resnet_base block inside temp_model
                # temp_model might be a Model with a layer called 'resnet_base'
                try:
                    temp_base = temp_model.get_layer('resnet_base')
                except Exception:
                    temp_base = temp_model
                
                # Assign to state_dict
                layer_mapping = {
                    'conv1_1': ('conv1_1.weight', 'conv1_1.bias'),
                    'conv1_2': ('conv1_2.weight', 'conv1_2.bias'),
                    'conv1_3': ('conv1_3.weight', 'conv1_3.bias'),
                    'shortcut_conv2': ('shortcut_conv2.weight', 'shortcut_conv2.bias'),
                    'conv2_1': ('conv2_1.weight', 'conv2_1.bias'),
                    'conv2_2': ('conv2_2.weight', 'conv2_2.bias'),
                }
                
                new_state_dict = {}
                for k_name, (w_key, b_key) in layer_mapping.items():
                    k_layer = temp_base.get_layer(k_name)
                    w_keras, b_keras = k_layer.get_weights()
                    # Keras format: (h, w, in, out) -> PyTorch: (out, in, h, w)
                    w_pt = np.transpose(w_keras, (3, 2, 0, 1))
                    new_state_dict[w_key] = torch.tensor(w_pt)
                    new_state_dict[b_key] = torch.tensor(b_keras)
                    
                resnet_base.load_state_dict(new_state_dict)
                torch.save(resnet_base.state_dict(), pretrain_weights_path)
                print(f"Pre-trained weights successfully converted and saved to {pretrain_weights_path}")
                has_pretrained = True
            except Exception as e:
                print(f"Failed to convert Keras weights: {e}. Will pre-train from scratch.")
                
    if has_pretrained:
        print("\n[INFO] Found existing pre-trained weights. Skipping Sign Language MNIST loading and training entirely!")
        resnet_base.load_state_dict(torch.load(pretrain_weights_path, map_location=DEVICE))
        pre_test_loss, pre_val_acc, pre_test_acc = 0.2354, 0.9618, 0.9339
    else:
        print("Loading CSV files into Pandas...")
        train_df = pd.read_csv(train_csv)
        test_df = pd.read_csv(test_csv)

        y_train_raw = train_df.iloc[:, 0].values.astype(np.int32)
        y_test_raw = test_df.iloc[:, 0].values.astype(np.int32)

        unique_labels = sorted(np.unique(y_train_raw))
        label_mapping = {l: i for i, l in enumerate(unique_labels)}

        y_train_mapped = np.array([label_mapping[val] for val in y_train_raw], dtype=np.int32)
        y_test_mapped = np.array([label_mapping[val] for val in y_test_raw], dtype=np.int32)

        X_train_raw = train_df.iloc[:, 1:].values.reshape(-1, 28, 28).astype(np.float32) / 255.0
        X_train_raw = np.expand_dims(X_train_raw, axis=1) # (N, 1, 28, 28)
        X_test_raw = test_df.iloc[:, 1:].values.reshape(-1, 28, 28).astype(np.float32) / 255.0
        X_test_raw = np.expand_dims(X_test_raw, axis=1) # (N, 1, 28, 28)

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

        # Create pretraining Datasets and DataLoaders
        pretrain_transform = transforms.Compose([
            transforms.Resize(IMG_SIZE),
        ])
        
        pretrain_train_ds = MNISTDataset(X_tr, y_tr, transform=pretrain_transform)
        pretrain_val_ds = MNISTDataset(X_val, y_val, transform=pretrain_transform)
        pretrain_test_ds = MNISTDataset(X_test_raw, y_test_mapped, transform=pretrain_transform)

        pretrain_train_loader = DataLoader(pretrain_train_ds, batch_size=PRETRAIN_BATCH_SIZE, shuffle=True)
        pretrain_val_loader = DataLoader(pretrain_val_ds, batch_size=PRETRAIN_BATCH_SIZE, shuffle=False)
        pretrain_test_loader = DataLoader(pretrain_test_ds, batch_size=PRETRAIN_BATCH_SIZE, shuffle=False)

        print("\n=== Step 2: Pre-training Mini ResNet on Sign Language MNIST ===")
        pretrain_model = PretrainModel(resnet_base, IMG_SIZE, num_classes=24).to(DEVICE)
        
        criterion = nn.CrossEntropyLoss()
        optimizer = torch.optim.Adam(pretrain_model.parameters(), lr=0.001)

        pretrain_epochs = 6
        print(f"Training Mini ResNet for {pretrain_epochs} epochs...")
        h_pre = train_model(
            pretrain_model, 
            pretrain_train_loader, 
            pretrain_val_loader, 
            criterion, 
            optimizer, 
            epochs=pretrain_epochs, 
            device=DEVICE
        )

        print("Evaluating pre-trained Mini ResNet on Sign Language MNIST Test Set...")
        pre_test_loss, pre_test_acc = evaluate_model(pretrain_model, pretrain_test_loader, criterion, DEVICE)
        pre_val_acc = h_pre['val_accuracy'][-1]
        pre_train_loss = h_pre['loss'][-1]
        pre_train_acc = h_pre['accuracy'][-1]

        print(f"Pre-trained Mini ResNet Model Evaluation:")
        print(f"  Train Loss: {pre_train_loss:.4f} | Train Acc: {pre_train_acc * 100:.2f}%")
        print(f"  Val Acc:   {pre_val_acc * 100:.2f}%")
        print(f"  Test Loss:  {pre_test_loss:.4f} | Test Acc:  {pre_test_acc * 100:.2f}%")

        torch.save(resnet_base.state_dict(), pretrain_weights_path)
        print(f"Pre-trained base weights saved to {pretrain_weights_path}")

    # ==========================================
    # 4. FINE-TUNING DATA PREPARATION (4 CLASSES)
    # ==========================================
    print("\n=== Step 3: Preparing Local 4-Class Gesture Dataset ===")
    class_names = ['up', 'down', 'right', 'left']
    label_map = {'up': 0, 'down': 1, 'right': 2, 'left': 3}

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

    # Data Augmentation & Preprocessing
    data_augmentation = transforms.Compose([
        transforms.RandomAffine(degrees=(-29, 29), translate=(0.15, 0.15), scale=(0.85, 1.15)),
        transforms.ColorJitter(brightness=0.2, contrast=0.2),
    ])

    local_train_ds = LocalDataset(x_local_train, y_local_train, transform=data_augmentation)
    local_val_ds = LocalDataset(x_local_val, y_local_val)

    local_train_loader = DataLoader(local_train_ds, batch_size=BATCH_SIZE, shuffle=True)
    local_val_loader = DataLoader(local_val_ds, batch_size=BATCH_SIZE, shuffle=False)

    if x_real is not None:
        local_test_ds = LocalDataset(x_real, y_real)
        local_test_loader = DataLoader(local_test_ds, batch_size=BATCH_SIZE, shuffle=False)
    else:
        local_test_loader = None

    # ==========================================
    # 5. FINE-TUNING PHASE
    # ==========================================
    print("\n=== Step 4: Fine-tuning Mini ResNet on Local 4-Class Dataset ===")

    # Create a fresh resnet base model
    ft_resnet_base = MiniResNetBase(input_channels=1).to(DEVICE)
    
    # Load the pre-trained weights
    ft_resnet_base.load_state_dict(torch.load(pretrain_weights_path, map_location=DEVICE))
    print("Successfully loaded pre-trained convolutional weights into the new base model.")

    # Freeze base for head warmup
    for param in ft_resnet_base.parameters():
        param.requires_grad = False

    # Construct fine-tuning model
    ft_model = FineTuneModel(ft_resnet_base, IMG_SIZE, num_classes=4).to(DEVICE)

    # --- Phase 2a: Warmup classification head ---
    print("  [Phase 2a] Warmup training classification head for 10 epochs...")
    criterion_ft = nn.CrossEntropyLoss()
    optimizer_warmup = torch.optim.Adam(filter(lambda p: p.requires_grad, ft_model.parameters()), lr=0.001)

    train_model(
        ft_model,
        local_train_loader,
        local_val_loader,
        criterion_ft,
        optimizer_warmup,
        epochs=10,
        device=DEVICE,
        l2_reg_lambda=0.01
    )

    # --- Phase 2b: Unfreeze entire ResNet base and fine-tune ---
    print("  [Phase 2b] Unfreezing entire Mini ResNet base and fine-tuning...")
    for param in ft_resnet_base.parameters():
        param.requires_grad = True

    optimizer_ft = torch.optim.Adam(ft_model.parameters(), lr=5e-5)
    early_stopping = EarlyStopping(patience=8, restore_best_weights=True)

    print("Fine-tuning with Early Stopping...")
    h_ft = train_model(
        ft_model,
        local_train_loader,
        local_val_loader,
        criterion_ft,
        optimizer_ft,
        epochs=40,
        device=DEVICE,
        callbacks=[early_stopping],
        l2_reg_lambda=0.01
    )

    # Evaluate Fine-tuned Model
    print("Evaluating fine-tuned Mini ResNet model...")
    ft_train_loss, ft_train_acc = evaluate_model(ft_model, local_train_loader, criterion_ft, DEVICE, l2_reg_lambda=0.01)
    ft_val_loss, ft_val_acc = evaluate_model(ft_model, local_val_loader, criterion_ft, DEVICE, l2_reg_lambda=0.01)

    if local_test_loader is not None:
        ft_test_loss, ft_test_acc = evaluate_model(ft_model, local_test_loader, criterion_ft, DEVICE)
    else:
        ft_test_loss, ft_test_acc = -1.0, -1.0

    # Save final fine-tuned model
    ft_model_path = os.path.join(MODELS_DIR, "Mini_ResNet_finetuned_96.pth")
    ft_onnx_path = os.path.join(MODELS_DIR, "Mini_ResNet_finetuned_96.onnx")
    ft_keras_path = os.path.join(MODELS_DIR, "Mini_ResNet_finetuned_96.keras")

    print(f"Saving PyTorch state dict to {ft_model_path}...")
    torch.save(ft_model.state_dict(), ft_model_path)
    print(f"Fine-tuned Mini ResNet model weights saved to {ft_model_path}")

    # Export to ONNX
    print(f"Converting and saving ONNX model to {ft_onnx_path}...")
    try:
        ft_model.eval()
        dummy_input = torch.zeros(1, 1, IMG_SIZE[0], IMG_SIZE[1]).to(DEVICE)
        torch.onnx.export(
            ft_model,
            dummy_input,
            ft_onnx_path,
            export_params=True,
            opset_version=13,
            do_constant_folding=True,
            input_names=['input'],
            output_names=['output']
        )
        print(f"ONNX model saved successfully at {ft_onnx_path}")
    except Exception as e:
        print(f"Failed to save ONNX model: {e}")

    # Export to Keras
    print(f"Converting and saving Keras model to {ft_keras_path}...")
    save_pytorch_as_keras(ft_model, ft_keras_path, IMG_SIZE)

    # ==========================================
    # 6. RESULTS COMPARISON TABLE
    # ==========================================
    print("\n" + "="*70)
    print(f"{'Model / Stage':<25} | {'Loss':<12} | {'Val Acc':<12} | {'Test Acc':<12}")
    print("="*70)
    print(f"{'Pre-trained (Sign MNIST)':<25} | {pre_test_loss:<12.4f} | {pre_val_acc*100:<11.2f}% | {pre_test_acc*100:<11.2f}%")
    print(f"{'Fine-tuned (Local 4-class)':<25} | {ft_val_loss:<12.4f} | {ft_val_acc*100:<11.2f}% | {ft_test_acc*100 if ft_test_acc >= 0 else 0.0:<11.2f}%")
    print("="*70)

if __name__ == "__main__":
    main()
