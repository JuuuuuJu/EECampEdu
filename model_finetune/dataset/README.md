# Dataset Folder

This folder is intentionally kept as a placeholder. Real images should not be committed to git.

## Expected Layout

```text
dataset/
  train/
    class_a/
      image001.jpg
    class_b/
      image001.jpg
  validation/
    class_a/
      image101.jpg
    class_b/
      image101.jpg
```

The portal can store many gesture class folders, but one training/deploy run uses at most 6 active classes.

## Image Formats

Accepted formats:

- `.jpg`
- `.jpeg`
- `.png`
- `.bmp`

Training and inference preprocessing should be consistent:

- crop/resize to `96x96`
- grayscale
- scale by `/255.0`

## Classroom Flow

Students normally collect images from the Model finetune page using the OV2640 camera. If a class is selected, physical shutter capture can also save the current frame into that dataset class.

Large imported datasets should be uploaded as a zip.
