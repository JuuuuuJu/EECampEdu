import serial
import base64
import os
import threading
import sys
import time

try:
    from PIL import Image
    HAS_PIL = True
except ImportError:
    HAS_PIL = False

try:
    import cv2
    import numpy as np
    HAS_CV2 = True
except ImportError:
    HAS_CV2 = False

# Configure the serial port parameters
COM_PORT = os.environ.get('ESP1_PORT', os.environ.get('CAMERA_CONTROLLER_PORT', 'COM11'))
BAUD_RATE = int(os.environ.get('ESP1_BAUD_RATE', os.environ.get('CAMERA_CONTROLLER_BAUD_RATE', '115200')))
ESP1_ASSERT_CONTROL_LINES = os.environ.get('ESP1_ASSERT_CONTROL_LINES', '0').lower() in ('1', 'true', 'yes', 'on')
ESP1_STARTUP_CONFIG = os.environ.get('ESP1_STARTUP_CONFIG', '0').lower() in ('1', 'true', 'yes', 'on')
ESP2_PORT = os.environ.get('OUTPUT_ESP2_PORT', os.environ.get('ESP2_PORT', ''))
ESP2_BAUD_RATE = int(os.environ.get('OUTPUT_ESP2_BAUD_RATE', os.environ.get('ESP2_BAUD_RATE', '115200')))
CLASS_NAMES = ['up', 'down', 'right', 'left', 'null']
OUTPUT_DIR = './captured_images'
os.makedirs(OUTPUT_DIR, exist_ok=True)
receiving_file = False

print(f"Connecting to ESP32-S3 on {COM_PORT} at {BAUD_RATE} baud...")
try:
    # Use a timeout of 2.0s to allow robust transfer of high-resolution images
    ser = serial.Serial(COM_PORT, BAUD_RATE, timeout=2.0, write_timeout=1.0, rtscts=False, dsrdtr=False, xonxoff=False)
    try:
        ser.set_buffer_size(rx_size=1048576, tx_size=65536)
    except Exception as buf_ex:
        print(f"[Warning] Failed to set serial buffer size: {buf_ex}")
    # Keep auto-download/reset control lines deasserted by default. On CH34x/CP210x
    # ESP32 boards, forcing DTR/RTS active can hold EN/GPIO0 in reset/boot mode,
    # which makes even tiny command writes time out on Windows.
    try:
        ser.dtr = bool(ESP1_ASSERT_CONTROL_LINES)
        ser.rts = bool(ESP1_ASSERT_CONTROL_LINES)
        print(f"[Python UI] ESP1 DTR/RTS asserted: {ESP1_ASSERT_CONTROL_LINES}")
    except Exception as line_ex:
        print(f"[Python UI] WARNING: Could not configure ESP1 DTR/RTS: {line_ex}")

    # Save the original write method and override it with a timeout-safe wrapper.
    # Without write_timeout, Windows COM writes can block forever if the ESP side
    # is rebooting, streaming garbage, or not consuming input.
    _orig_write = ser.write
    def robust_write(data):
        for attempt in range(2):
            try:
                written = _orig_write(data)
                try:
                    ser.flush()
                except Exception:
                    pass
                return written
            except Exception as e:
                print(f"\n[Python UI] Write failed ({attempt+1}/2): {e}")
                try:
                    ser.reset_output_buffer()
                except Exception:
                    pass
                time.sleep(0.2)
        return 0

    ser.write = robust_write
except Exception as e:
    print(f"Error opening serial port: {e}")
    print("Please make sure the COM port is correct and not occupied by another serial monitor.")
    exit(1)

esp2_ser = None
if ESP2_PORT:
    print(f"Connecting to ESP2 output controller on {ESP2_PORT} at {ESP2_BAUD_RATE} baud...")
    try:
        esp2_ser = serial.Serial(ESP2_PORT, ESP2_BAUD_RATE, timeout=0.3, write_timeout=0.3)
        time.sleep(2.0)
        esp2_ser.reset_input_buffer()
        print("[ESP2] Output bridge enabled.")
    except Exception as e:
        print(f"[ESP2] Warning: failed to open output port {ESP2_PORT}: {e}")
        print("[ESP2] Camera controller will continue without servo forwarding.")
        esp2_ser = None
else:
    print("[ESP2] Output bridge disabled. Set OUTPUT_ESP2_PORT=COM7 to enable servo forwarding.")
def send_esp1_command(command, label=None):
    if isinstance(command, str):
        command = command.encode("utf-8")
    shown = label or command.decode("utf-8", errors="ignore").strip()
    print(f"[Python UI] -> ESP1 {shown}")
    try:
        written = ser.write(command)
        if written != len(command):
            print(f"[Python UI] WARNING: partial write for {shown}: {written}/{len(command)} bytes")
            return False
        return True
    except Exception as e:
        print(f"[Python UI] WARNING: failed to send {shown}: {e}")
        return False


def label_name(label):
    if 0 <= label < len(CLASS_NAMES):
        return CLASS_NAMES[label]
    return "unknown"


def forward_result_to_esp2(line):
    if esp2_ser is None or not line.startswith("RESULT,"):
        return
    try:
        parts = line.split(",")
        pred_idx = int(parts[1])
        pred_name = label_name(pred_idx)
        command = f"GESTURE,{pred_idx},{pred_name}\n"
        esp2_ser.write(command.encode("ascii"))
        esp2_ser.flush()
        ack = esp2_ser.readline().decode("utf-8", errors="ignore").strip()
        if ack:
            print(f"[ESP2] {ack}")
        else:
            print(f"[ESP2] sent {command.strip()} (no ack)")
    except Exception as e:
        print(f"[ESP2] Forwarding failed: {e}")

buffer = []
receiving = False
metadata = {}
file_index_cache = []
pending_downloads = []
pending_inferences = []

# Globally shared variables for live preview and capture synchronization
latest_frame = None
latest_frame_lock = threading.Lock()

saving_next_frame = False
saving_next_frame_lock = threading.Lock()

# Camera settings state tracking (local representation of ESP32 state)
camera_state = {
    "format": "JPEG",
    "resolution": "QQVGA (160x120)",
    "aec": 1,         # 1 = Auto, 0 = Manual
    "exposure": 0,    # 0 to 1200
    "agc": 1,         # 1 = Auto, 0 = Manual
    "gain": 0,        # 0 to 30
    "brightness": 0,  # -2 to 2
    "contrast": 0,    # -2 to 2
    "saturation": 0,  # -2 to 2
    "mirror": 0,      # 0 = Disabled, 1 = Enabled
    "flip": 0,        # 0 = Disabled, 1 = Enabled
    "streaming": False # True = Continuous stream active
}

def save_frame(img_bytes, metadata, fmt_id):
    # Decide extension based on firmware CameraFrameFormat enum:
    # Firmware CameraFrameFormat: 0=GRAYSCALE, 1=RGB565, 2=YUV422, 3=JPEG
    if fmt_id == 4:
        ext = "jpg"
    elif fmt_id == 3:
        ext = "gray"
    elif fmt_id == 0:
        ext = "rgb565"
    elif fmt_id == 1:
        ext = "yuv422"
    else:
        ext = "bin"
        
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    filename = f"image_{timestamp}_{metadata.get('width','96')}x{metadata.get('height','96')}_{len(img_bytes)}.{ext}"
    filepath = os.path.join(OUTPUT_DIR, filename)
    
    try:
        with open(filepath, "wb") as f:
            f.write(img_bytes)
            
        print(f"Image decoded and saved successfully to: {filepath}")
        
        if fmt_id == 4:
            print("File is a valid JPEG, you can open it directly in any viewer.")
    except Exception as ex:
        print(f"❌ Failed to save image: {ex}")

class BlockLineReader:
    def __init__(self, serial_port):
        self.ser = serial_port
        self.buffer = bytearray()
        
    def readline(self):
        while b'\n' not in self.buffer:
            waiting = self.ser.in_waiting
            if waiting > 0:
                chunk = self.ser.read(waiting)
                self.buffer.extend(chunk)
            else:
                time.sleep(0.001)
        line_bytes, self.buffer = self.buffer.split(b'\n', 1)
        return line_bytes

# Background thread to continuously read from the serial port
def read_serial_thread():
    global receiving, buffer, metadata, latest_frame, saving_next_frame, pending_downloads, pending_inferences, receiving_file
    file_buffer = []
    file_metadata = {}
    
    line_reader = BlockLineReader(ser)
    while True:
        try:
            line_bytes = line_reader.readline()
            line = line_bytes.decode('utf-8', errors='ignore').strip()
            if not line:
                continue
                
            if "Rebooting device" in line:
                print("[Python UI] ESP32 is rebooting. Shutting down controller...")
                os._exit(0)
                
            if line.startswith("---START_FILE:"):
                # Header format: ---START_FILE:format:width:height:size:filename---
                parts = line.split(":")
                if len(parts) >= 6:
                    file_metadata = {
                        "format": int(parts[1]),
                        "width": int(parts[2]),
                        "height": int(parts[3]),
                        "size": int(parts[4]),
                        "filename": parts[5].rstrip("-")
                    }
                    receiving_file = True
                    file_buffer = []
                    print(f"\n📥 Downloading stored file '{file_metadata['filename']}': {file_metadata['width']}x{file_metadata['height']}, Size: {file_metadata['size']} bytes...")
                    
            elif line.startswith("---END_FILE---"):
                if receiving_file:
                    receiving_file = False
                    base64_data = "".join(file_buffer)
                    try:
                        file_bytes = base64.b64decode(base64_data)
                        fmt_id = file_metadata.get("format", 4)
                        clean_name = file_metadata["filename"].replace("/usb/", "").lstrip("/")
                        
                        # Decide extension based on format
                        orig_ext = file_metadata["filename"].split(".")[-1].lower()
                        if orig_ext == "bmp":
                            ext = "bmp"
                        elif fmt_id == 4:
                            ext = "jpg"
                        elif fmt_id == 3:
                            ext = "gray"
                        elif fmt_id == 0:
                            ext = "rgb565"
                        elif fmt_id == 1:
                            ext = "yuv422"
                        else:
                            ext = "bin"
                        
                        # Enforce correct extension in PC file name
                        if not clean_name.lower().endswith(f".{ext}"):
                            clean_name = clean_name.rsplit(".", 1)[0] + f".{ext}"
                            
                        save_path = os.path.join(OUTPUT_DIR, clean_name)
                        with open(save_path, "wb") as f:
                            f.write(file_bytes)
                        print(f"File retrieved and saved successfully to: {save_path}")
                        
                        # PNG conversion disabled per user request
                        pass
                                
                        # Handle batch download sequencing
                        if pending_downloads:
                            def request_next():
                                time.sleep(0.5)
                                if pending_downloads:
                                    next_file = pending_downloads.pop(0)
                                    print(f"[Python UI] Batch download: Requesting next file '{next_file}' ({len(pending_downloads)} remaining)...")
                                    ser.write(f"r{next_file}\n".encode('utf-8'))
                            threading.Thread(target=request_next, daemon=True).start()
                    except Exception as ex:
                        print(f"❌ Failed to decode and save retrieved file: {ex}")
                        
            elif line.startswith("---START_IMAGE:"):
                # Header format: ---START_IMAGE:format:width:height:size---
                parts = line.split(":")
                if len(parts) >= 5:
                    metadata = {
                        "format": int(parts[1]),
                        "width": int(parts[2]),
                        "height": int(parts[3]),
                        "size": int(parts[4].rstrip("-"))
                    }
                    receiving = True
                    buffer = []
                    
                    # Check if this frame is explicitly requested for saving
                    is_saving = False
                    with saving_next_frame_lock:
                        is_saving = saving_next_frame
                    
                    if is_saving:
                        print(f"\n📥 Receiving new image frame: {metadata['width']}x{metadata['height']}, Size: {metadata['size']} bytes...")
                
            elif line.startswith("---END_IMAGE---"):
                if receiving:
                    receiving = False
                    base64_data = "".join(buffer)
                    try:
                        # Decode base64
                        img_bytes = base64.b64decode(base64_data)
                        fmt_id = metadata.get("format", 4)
                        
                        # Decide if we save it to disk
                        is_saving = False
                        with saving_next_frame_lock:
                            if saving_next_frame:
                                is_saving = True
                                saving_next_frame = False # Reset flag after consumption
                                
                        if is_saving:
                            save_frame(img_bytes, metadata, fmt_id)
                            
                        # If streaming is enabled, update latest frame for GUI view
                        if camera_state.get("streaming", False):
                            with latest_frame_lock:
                                latest_frame = {
                                    "bytes": img_bytes,
                                    "metadata": metadata,
                                    "format": fmt_id
                                }
                    except Exception as ex:
                        print(f"❌ Failed to decode Base64 image: {ex}")
                
            elif receiving_file:
                file_buffer.append(line)
            elif receiving:
                buffer.append(line)
            else:
                # Intercept file list logs to populate the index cache
                if line.startswith("--- LOCAL FLASH FILE LIST ---"):
                    file_index_cache.clear()
                    print(f"[ESP32] {line}")
                    forward_result_to_esp2(line)
                elif line.startswith("- ") and ("bytes)" in line):
                    parts = line.split()
                    if len(parts) >= 2:
                        filename = parts[1]
                        if not filename.startswith("/"):
                            filename = "/" + filename
                        file_index_cache.append(filename)
                        idx = len(file_index_cache)
                        # Print with index prefix in Python!
                        print(f"[ESP32]  [{idx}] {line}")
                    else:
                        print(f"[ESP32] {line}")
                    forward_result_to_esp2(line)
                else:
                    # Print standard output lines from ESP32 (logs, statistics, ascii art)
                    print(f"[ESP32] {line}")
                    forward_result_to_esp2(line)
                    
                    if "Inference started" in line:
                        print("[Python UI] ⏳ Running on-device TFLite model inference... Please wait (this can take several seconds for larger models)...")
                    
                    # Handle batch download and inference recoveries
                    if "ERROR: File " in line and "not found" in line:
                        if pending_downloads:
                            def request_next():
                                time.sleep(0.5)
                                if pending_downloads:
                                    next_file = pending_downloads.pop(0)
                                    print(f"[Python UI] Batch download (recovery): Requesting next file '{next_file}' ({len(pending_downloads)} remaining)...")
                                    ser.write(f"r{next_file}\n".encode('utf-8'))
                            threading.Thread(target=request_next, daemon=True).start()
                        if pending_inferences:
                            def request_next_inf():
                                time.sleep(0.5)
                                if pending_inferences:
                                    next_file = pending_inferences.pop(0)
                                    print(f"[Python UI] Batch inference (recovery): Requesting inference on '{next_file}' ({len(pending_inferences)} remaining)...")
                                    ser.write(f"i{next_file}\n".encode('utf-8'))
                            threading.Thread(target=request_next_inf, daemon=True).start()
                    elif "--- FILE INFERENCE COMPLETED ---" in line:
                        if pending_inferences:
                            def request_next_inf():
                                time.sleep(0.5)
                                if pending_inferences:
                                    next_file = pending_inferences.pop(0)
                                    print(f"[Python UI] Batch inference: Requesting inference on '{next_file}' ({len(pending_inferences)} remaining)...")
                                    ser.write(f"i{next_file}\n".encode('utf-8'))
                            threading.Thread(target=request_next_inf, daemon=True).start()
        except Exception as e:
            # If serial connection is closed/broken, terminate thread
            break

def print_help_menu():
    print("\n" + "="*60)
    print("💻 INTERACTIVE CONTROL INTERFACE (Python Host)")
    print("="*60)
    print("Type commands directly here and press Enter.")
    print("Commands are validated on the PC before sending to ESP32.")
    print("-"*60)
    print("📁 General and Storage commands:")
    print("  h                   - Display this Help menu")
    print("  c                   - Trigger image acquisition (saved to PC & CV processed on ESP32)")
    print("  d1 / d0             - Start/Stop Continuous live camera stream (GUI preview)")
    print("  w                   - Capture image and save locally to ESP32 Flash storage")
    print("  l                   - List all files stored in ESP32 Flash storage (displays index numbers)")
    print("  r <idx/file> [cv]   - Retrieve file from ESP32 Flash (r 0 to retrieve all files). Append 'cv' for CV copy")
    print("  k <idx/file>        - Delete/Kill a file from ESP32 Flash (k 0 to delete all files)")
    print("  i <idx/file>        - Feed a stored file to local CV model task (i 0 to infer on all files)")
    print("  format              - Format ESP32 Flash partition")
    print("  usb                 - Expose ESP32 storage to PC as USB drive")
    print("\n🎨 Pixel Format (f <0-3>):")
    print("  f0       - GRAYSCALE (1 byte per pixel)")
    print("  f1       - RGB565    (2 bytes per pixel)")
    print("  f2       - YUV422    (2 bytes per pixel)")
    print("  f3       - JPEG      (Compressed, variable size)")
    print("\n📐 Resolution (s <0-5>):")
    print("  s0       - 96x96")
    print("  s1       - QQVGA (160x120)")
    print("  s2       - QVGA  (320x240)")
    print("  s3       - VGA   (640x480)")
    print("  s4       - SVGA  (800x600)")
    print("  s5       - UXGA  (1600x1200) (Use JPEG format for RAM safety)")
    print("\nExposure & Sensor Controls:")
    print("  e1 / e0  - Enable/Disable Auto Exposure Control (AEC)")
    print("  v <0-1200>- Manual Exposure index (Only active when AEC is disabled)")
    print("  g1 / g0  - Enable/Disable Auto Gain Control (AGC)")
    print("  a <0-30> - Manual Gain index (Only active when AGC is disabled)")
    print("  b <value>- Set Brightness (-2 to 2)")
    print("  t <value>- Set Contrast (-2 to 2)")
    print("  x <value>- Set Saturation (-2 to 2)")
    print("  m1 / m0  - Enable/Disable Horizontal Mirror")
    print("  p1 / p0  - Enable/Disable Vertical Flip")
    print("-"*60)
    print(f"Current State: Format = {camera_state['format']} | Resolution = {camera_state['resolution']}")
    print(f"               AEC = {'AUTO' if camera_state['aec'] else 'MANUAL'} | Exposure = {camera_state['exposure']}")
    print(f"               AGC = {'AUTO' if camera_state['agc'] else 'MANUAL'} | Gain = {camera_state['gain']}")
    print(f"               Brightness = {camera_state['brightness']} | Contrast = {camera_state['contrast']} | Saturation = {camera_state['saturation']}")
    print(f"               Mirror = {'ENABLED' if camera_state['mirror'] else 'DISABLED'} | Flip = {'ENABLED' if camera_state['flip'] else 'DISABLED'}")
    print(f"               Streaming = {'ENABLED' if camera_state['streaming'] else 'DISABLED'}")
    print("="*60 + "\n")

def handle_command(cmd_str):
    global camera_state, latest_frame, saving_next_frame, pending_downloads, pending_inferences, receiving_file
    cmd_str = cmd_str.strip()
    if not cmd_str:
        return False
        
    if receiving_file:
        print("⏳ Warning: A file download is currently in progress. Please wait for it to complete.")
        return False
        
    if cmd_str.lower() == "format":
        print("[Python UI] Sending full format command to ESP32 Flash partition...")
        ser.write(b"format\n")
        return True
        
    if cmd_str.lower() == "usb":
        print("[Python UI] Exposing ESP32 storage partition to PC as USB drive...")
        camera_state["streaming"] = False
        ser.write(b"usb\n")
        return True
        
    action = cmd_str[0].lower()
    arg_str = cmd_str[1:].strip()
    
    if action == 'h':
        print_help_menu()
        return False
        
    if action == 'c':
        ts = time.strftime("%Y%m%d_%H%M%S")
        print(f"[Python UI] Triggering capture workflow with timestamp {ts}...")
        send_esp1_command(f"c{ts}\n", "capture")
        return True
        
    if action == 'w':
        ts = time.strftime("%Y%m%d_%H%M%S")
        print(f"[Python UI] Triggering local capture to ESP32 flash with timestamp {ts}...")
        ser.write(f"w{ts}\n".encode('utf-8'))
        return True
        
    if action == 'l':
        print("[Python UI] Requesting local flash file list from ESP32...")
        ser.write(b"l\n")
        return True
        
    if action in ['r', 'k', 'i']:
        if not arg_str:
            print(f"❌ Error: Command '{action}' requires a filename or index argument (e.g. '{action} 1' or '{action} /img_name.jpg')")
            return False
            
        parts = arg_str.split(None, 1)
        target = parts[0]
        
        # Check if target is a numeric index
        if target.isdigit():
            val = int(target)
            if val == 0:
                if action == 'k':
                    # Special shortcut: k 0 means delete all files
                    filename = "0"
                elif action == 'r':
                    # Special shortcut: r 0 means download all files
                    filename = "0"
                elif action == 'i':
                    # Special shortcut: i 0 means run inference on all files
                    filename = "0"
                else:
                    print(f"❌ Error: Index '0' is only valid for 'k', 'r', and 'i' commands.")
                    return False
            else:
                idx = val - 1
                if 0 <= idx < len(file_index_cache):
                    filename = file_index_cache[idx]
                else:
                    print(f"❌ Error: Invalid index '{target}'. Type 'l' to list available files and indices.")
                    return False
        else:
            filename = target
            if not filename.startswith("/"):
                filename = "/" + filename
                
        normalized_args = filename
        if len(parts) > 1:
            normalized_args += " " + parts[1]
            
        if action == 'r':
            if normalized_args == "/0" or normalized_args == "0":
                if not file_index_cache:
                    print("❌ Error: No files listed in cache. Please type 'l' first to list and index files.")
                    return False
                pending_downloads = list(file_index_cache)
                next_file = pending_downloads.pop(0)
                print(f"[Python UI] Batch download started. Requesting file '{next_file}' ({len(pending_downloads)} remaining)...")
                ser.write(f"r{next_file}\n".encode('utf-8'))
            else:
                print(f"[Python UI] Requesting retrieval of file '{normalized_args}' from ESP32...")
                ser.write(f"r{normalized_args}\n".encode('utf-8'))
        elif action == 'k':
            if normalized_args == "/0" or normalized_args == "0":
                print("[Python UI] Requesting deletion of ALL files from ESP32 storage...")
                ser.write(b"k0\n")
            else:
                print(f"[Python UI] Requesting deletion of file '{normalized_args}' from ESP32...")
                ser.write(f"k{normalized_args}\n".encode('utf-8'))
        elif action == 'i':
            if normalized_args == "/0" or normalized_args == "0":
                if not file_index_cache:
                    print("❌ Error: No files listed in cache. Please type 'l' first to list and index files.")
                    return False
                pending_inferences = list(file_index_cache)
                next_file = pending_inferences.pop(0)
                print(f"[Python UI] Batch inference started. Requesting inference on '{next_file}' ({len(pending_inferences)} remaining)...")
                ser.write(f"i{next_file}\n".encode('utf-8'))
            else:
                print(f"[Python UI] Requesting local CV model inference on file '{normalized_args}'...")
                ser.write(f"i{normalized_args}\n".encode('utf-8'))
        return True
        
    # All other commands expect an integer argument
    try:
        val = int(arg_str)
    except ValueError:
        print(f"❌ Error: Command '{action}' requires an integer argument.")
        return False
        
    if action == 'f':
        formats = {0: "GRAYSCALE", 1: "RGB565", 2: "YUV422", 3: "JPEG"}
        if val not in formats:
            print("❌ Error: Invalid format (0=Grayscale, 1=RGB565, 2=YUV422, 3=JPEG)")
            return False
            
        # Check if they are trying to select JPEG with 96x96 resolution
        # Check if they are trying to select JPEG with 96x96 resolution
        if val == 3 and camera_state["resolution"] == "96x96":
            print("[Python UI] JPEG format is not supported at 96x96 by the OV2640 sensor hardware.")
            print("[Python UI] Automatically switching resolution to QQVGA (160x120) for compatibility.")
            ser.write(b"s1\n")
            time.sleep(1.2)
            camera_state["resolution"] = "QQVGA (160x120)"
            
        # Check if they are trying to select a raw format while at VGA or higher resolution
        is_high_res = camera_state["resolution"] in ["VGA (640x480)", "SVGA (800x600)", "UXGA (1600x1200)"]
        if val != 3 and is_high_res:
            print("❌ Error: Raw formats (Grayscale/RGB565/YUV422) are only supported at QVGA (320x240) and lower resolutions.")
            print("Please change resolution (s0, s1, s2) first before switching format.")
            return False
            
        camera_state["format"] = formats[val]
        print(f"[Python UI] Changing pixel format to {formats[val]}...")
        ser.write(f"f{val}\n".encode('utf-8'))
        return True
        
    elif action == 's':
        resolutions = {
            0: "96x96",
            1: "QQVGA (160x120)",
            2: "QVGA (320x240)",
            3: "VGA (640x480)",
            4: "SVGA (800x600)",
            5: "UXGA (1600x1200)"
        }
        if val not in resolutions:
            print("❌ Error: Invalid resolution (0=96x96, 1=QQVGA, 2=QVGA, 3=VGA, 4=SVGA, 5=UXGA)")
            return False
            
        # Check if they are trying to select 96x96 resolution while in JPEG format
        if val == 0 and camera_state["format"] == "JPEG":
            print("❌ Error: OV2640 sensor hardware does not support JPEG compression at 96x96 resolution.")
            print("Please change format to Grayscale (f0), RGB565 (f1), or YUV422 (f2) first.")
            return False
            
        # Check if they are trying to select VGA or higher while in a raw format
        if val >= 3 and camera_state["format"] != "JPEG":
            print(f"[Python UI] Raw formats (Grayscale/RGB565/YUV422) at {resolutions[val]} require too much RAM and will crash the ESP32.")
            print("[Python UI] Automatically switching pixel format to JPEG for safety.")
            ser.write(b"f3\n")
            time.sleep(1.2)
            camera_state["format"] = "JPEG"
            
        camera_state["resolution"] = resolutions[val]
        print(f"[Python UI] Changing resolution to {resolutions[val]}...")
        ser.write(f"s{val}\n".encode('utf-8'))
        return True
        
    elif action == 'e':
        if val not in [0, 1]:
            print("❌ Error: AEC value must be 0 (Disable) or 1 (Enable)")
            return False
        camera_state["aec"] = val
        print(f"[Python UI] Auto Exposure Control (AEC) set to {'AUTO' if val else 'MANUAL'}")
        ser.write(f"e{val}\n".encode('utf-8'))
        return True
        
    elif action == 'v':
        if not (0 <= val <= 1200):
            print("❌ Error: Manual Exposure index must be between 0 and 1200")
            return False
        camera_state["exposure"] = val
        print(f"[Python UI] Manual Exposure set to {val} (AEC must be disabled for this to take effect)")
        ser.write(f"v{val}\n".encode('utf-8'))
        return True
        
    elif action == 'g':
        if val not in [0, 1]:
            print("❌ Error: AGC value must be 0 (Disable) or 1 (Enable)")
            return False
        camera_state["agc"] = val
        print(f"[Python UI] Auto Gain Control (AGC) set to {'AUTO' if val else 'MANUAL'}")
        ser.write(f"g{val}\n".encode('utf-8'))
        return True
        
    elif action == 'a':
        if not (0 <= val <= 30):
            print("❌ Error: Manual Gain index must be between 0 and 30")
            return False
        camera_state["gain"] = val
        print(f"[Python UI] Manual Gain set to {val} (AGC must be disabled for this to take effect)")
        ser.write(f"a{val}\n".encode('utf-8'))
        return True
        
    elif action == 'b':
        if not (-2 <= val <= 2):
            print("❌ Error: Brightness offset must be between -2 and 2")
            return False
        camera_state["brightness"] = val
        print(f"[Python UI] Brightness set to {val}")
        ser.write(f"b{val}\n".encode('utf-8'))
        return True
        
    elif action == 't':
        if not (-2 <= val <= 2):
            print("❌ Error: Contrast offset must be between -2 and 2")
            return False
        camera_state["contrast"] = val
        print(f"[Python UI] Contrast set to {val}")
        ser.write(f"t{val}\n".encode('utf-8'))
        return True
        
    elif action == 'x':
        if not (-2 <= val <= 2):
            print("❌ Error: Saturation offset must be between -2 and 2")
            return False
        camera_state["saturation"] = val
        print(f"[Python UI] Saturation set to {val}")
        ser.write(f"x{val}\n".encode('utf-8'))
        return True
        
    elif action == 'm':
        if val not in [0, 1]:
            print("❌ Error: Mirror value must be 0 (Disable) or 1 (Enable)")
            return False
        camera_state["mirror"] = val
        print(f"[Python UI] Horizontal Mirror set to {'ENABLED' if val else 'DISABLED'}")
        ser.write(f"m{val}\n".encode('utf-8'))
        return True
        
    elif action == 'p':
        if val not in [0, 1]:
            print("❌ Error: Flip value must be 0 (Disable) or 1 (Enable)")
            return False
        camera_state["flip"] = val
        print(f"[Python UI] Vertical Flip set to {'ENABLED' if val else 'DISABLED'}")
        ser.write(f"p{val}\n".encode('utf-8'))
        return True
        
    elif action == 'd':
        if val not in [0, 1]:
            print("❌ Error: Streaming mode must be 0 (Stop) or 1 (Start)")
            return False
        camera_state["streaming"] = (val == 1)
        print(f"[Python UI] Continuous streaming mode: {'ENABLED' if val else 'DISABLED'}")
        ser.write(f"d{val}\n".encode('utf-8'))
        return True
        
    else:
        print("❌ Unknown Command. Type 'h' to show the help menu.")
        return False

# Command loop running in a background daemon thread
def command_loop():
    try:
        while True:
            cmd = sys.stdin.readline().strip()
            if cmd:
                handle_command(cmd)
    except KeyboardInterrupt:
        pass

# GUI thread function to display preview
def run_gui():
    global latest_frame, saving_next_frame
    if not HAS_CV2:
        print("\n⚠️ OpenCV or NumPy is not installed. Live preview window is disabled.")
        print("Install them using: pip install opencv-python numpy")
        print("Running in CLI-only mode.")
        while True:
            time.sleep(1)
        return
        
    print("\n-------------------------------------------------------------")
    print("🎥 LIVE GUI PREVIEW WINDOW ACTIVATED")
    print("  - Focus the window and press 'c' or 'Space' to capture a frame.")
    print("  - Press 'q' or 'Esc' in the GUI window to exit preview.")
    print("-------------------------------------------------------------")
    
    try:
        cv2.namedWindow("ESP32-S3 OV2640 Live View", cv2.WINDOW_NORMAL)
    except Exception as e:
        print(f"⚠️ Failed to create GUI window (running in headless mode?): {e}")
        while True:
            time.sleep(1)
        return

    # Automatically start continuous streaming from ESP32 on launch.
    # In unit-test mode, force QQVGA before d1 so 115200 baud is not flooded by VGA JPEG frames.
    if not ESP1_STARTUP_CONFIG:
        send_esp1_command(b"s1\n", "s1 QQVGA before preview")
        camera_state["resolution"] = "QQVGA (160x120)"
        time.sleep(0.2)
    camera_state["streaming"] = True
    send_esp1_command(b"d1\n", "d1 start preview")
    
    last_displayed_frame = None
    first_frame = True
    
    while True:
        frame_to_show = None
        with latest_frame_lock:
            if latest_frame is not None and latest_frame != last_displayed_frame:
                frame_to_show = latest_frame
                last_displayed_frame = latest_frame
                
        if frame_to_show is not None:
            img_bytes = frame_to_show["bytes"]
            metadata = frame_to_show["metadata"]
            fmt_id = frame_to_show["format"]
            
            img_bgr = None
            try:
                if fmt_id == 4:  # JPEG
                    nparr = np.frombuffer(img_bytes, np.uint8)
                    img_bgr = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
                elif fmt_id == 3:  # Grayscale
                    nparr = np.frombuffer(img_bytes, np.uint8)
                    img_gray = nparr.reshape((metadata['height'], metadata['width']))
                    img_bgr = cv2.cvtColor(img_gray, cv2.COLOR_GRAY2BGR)
                elif fmt_id == 0:  # RGB565
                    nparr = np.frombuffer(img_bytes, np.uint8)
                    pixels = nparr.view(dtype=np.uint16).byteswap().reshape((metadata['height'], metadata['width']))
                    
                    r = (((pixels >> 11) & 0x1F) << 3).astype(np.uint8)
                    g = (((pixels >> 5) & 0x3F) << 2).astype(np.uint8)
                    b = ((pixels & 0x1F) << 3).astype(np.uint8)
                    
                    img_bgr = np.zeros((metadata['height'], metadata['width'], 3), dtype=np.uint8)
                    img_bgr[..., 2] = r  # OpenCV uses BGR
                    img_bgr[..., 1] = g
                    img_bgr[..., 0] = b
                elif fmt_id == 1:  # YUV422
                    nparr = np.frombuffer(img_bytes, np.uint8)
                    yuv = nparr.reshape((metadata['height'], metadata['width'], 2))
                    img_bgr = cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR_YUY2)
            except Exception as e:
                print(f"Error decoding frame for preview: {e}")
                
            if img_bgr is not None:
                img_h, img_w = img_bgr.shape[:2]
                
                # Set default window size on first frame
                if first_frame:
                    init_w = img_w * 4 if img_w < 300 else img_w
                    init_h = img_h * 4 if img_w < 300 else img_h
                    if init_w > 800:
                        init_h = int(init_h * (800 / init_w))
                        init_w = 800
                    cv2.resizeWindow("ESP32-S3 OV2640 Live View", init_w, init_h)
                    first_frame = False
                
                # Fetch actual window size
                rect = cv2.getWindowImageRect("ESP32-S3 OV2640 Live View")
                if rect is not None and len(rect) >= 4:
                    win_w, win_h = rect[2], rect[3]
                else:
                    win_w, win_h = 0, 0
                
                if win_w > 10 and win_h > 10:
                    scale = min(win_w / img_w, win_h / img_h)
                    new_w = max(1, int(img_w * scale))
                    new_h = max(1, int(img_h * scale))
                    
                    if scale > 1:
                        resized = cv2.resize(img_bgr, (new_w, new_h), interpolation=cv2.INTER_NEAREST)
                    else:
                        resized = cv2.resize(img_bgr, (new_w, new_h), interpolation=cv2.INTER_AREA)
                    
                    canvas = np.zeros((win_h, win_w, 3), dtype=np.uint8)
                    dx = (win_w - new_w) // 2
                    dy = (win_h - new_h) // 2
                    canvas[dy:dy+new_h, dx:dx+new_w] = resized
                    img_to_show = canvas
                else:
                    img_to_show = img_bgr
                
                cv2.imshow("ESP32-S3 OV2640 Live View", img_to_show)
                
        key = cv2.waitKey(30) & 0xFF
        if key == ord('q') or key == 27:  # 'q' or Esc to quit
            break
        elif key == ord('c') or key == ord(' '):  # 'c' or Space to capture
            ts = time.strftime("%Y%m%d_%H%M%S")
            print(f"[Python UI] GUI requested frame capture with timestamp {ts}...")
            send_esp1_command(f"c{ts}\n", "capture")
                    
    # Clean up stream
    print("[Python UI] Stopping live stream...")
    camera_state["streaming"] = False
    ser.write(b"d0\n")
    cv2.destroyAllWindows()

# Reset state and clear buffer BEFORE starting the serial reader thread
try:
    if ESP1_STARTUP_CONFIG:
        print("[Python UI] Resetting ESP32 camera stream state...")
        send_esp1_command(b"d0\n", "d0 stop stream")
        send_esp1_command(b"f3\n", "f3 JPEG")
        send_esp1_command(b"s3\n", "s3 VGA")
        time.sleep(0.3)            # Wait for ESP32 to receive and process
    else:
        print("[Python UI] Startup camera config skipped. Unit-test firmware defaults to JPEG VGA. For unit test, set s1 manually if serial preview is slow.")
    try:
        ser.reset_input_buffer()   # Clear all leftover garbage bytes in serial FIFO
    except Exception as reset_ex:
        print(f"[Python UI] WARNING: Could not clear serial input buffer: {reset_ex}")
except Exception as e:
    print(f"[Python UI] WARNING: Could not reset camera state: {e}")

# Start background serial reader thread AFTER the buffer is clean!
t = threading.Thread(target=read_serial_thread, daemon=True)
t.start()

# Start background terminal command prompt thread
cmd_thread = threading.Thread(target=command_loop, daemon=True)
cmd_thread.start()

print("Listening for incoming image data blocks...")
print_help_menu()

try:
    run_gui()
except KeyboardInterrupt:
    print("\nExiting script.")
finally:
    # Ensure camera stops streaming and serial connection closes
    ser.write(b"d0\n")
    ser.close()














