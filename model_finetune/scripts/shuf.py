import os
import shutil

# Target directory containing the unorganized training images
TARGET_DIR = "./direction_data/training"

# The specific classes/folders to create and sort into
CLASSES = ["null", "left", "right", "up", "down"]

def organize_direction_dataset(base_dir, classes):
    if not os.path.exists(base_dir):
        print(f"Error: The directory '{base_dir}' does not exist.")
        return

    # Create the target subfolders if they don't exist yet
    for class_name in classes:
        class_folder = os.path.join(base_dir, class_name)
        if not os.path.exists(class_folder):
            os.makedirs(class_folder)
            print(f"Created folder: {class_folder}")

    # Track moving statistics
    moved_counts = {c: 0 for c in classes}
    skipped_count = 0

    # Iterate through all files in the base directory
    for filename in os.listdir(base_dir):
        file_path = os.path.join(base_dir, filename)

        # Skip directories (like the target folders we just created)
        if os.path.isdir(file_path):
            continue

        # Check if the file is an image format
        if not filename.lower().endswith(('.png', '.jpg', '.jpeg', '.bmp')):
            continue

        # Match filename keywords to target classes
        matched = False
        lower_filename = filename.lower()
        
        for class_name in classes:
            if class_name in lower_filename:
                dest_path = os.path.join(base_dir, class_name, filename)
                
                # Move the file securely
                shutil.move(file_path, dest_path)
                moved_counts[class_name] += 1
                matched = True
                break # Stop checking other classes once matched

        if not matched:
            skipped_count += 1

    # Print clean execution logs
    print("\n" + "="*40)
    print("Training Directory Sorting Complete. Summary:")
    print("="*40)
    for class_name, count in moved_counts.items():
        print(f"Moved to '{class_name}/': {count} files")
    print(f"Skipped (No class keyword matched): {skipped_count} files")
    print("="*40)

if __name__ == "__main__":
    organize_direction_dataset(TARGET_DIR, CLASSES)