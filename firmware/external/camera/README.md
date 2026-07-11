# Camera Team Reference

Reference source:

```text
OV2640 team reference project
```

Important findings:

- Current default camera format is `PIXFORMAT_GRAYSCALE`.
- Current default frame size is `FRAMESIZE_96X96`.
- The reference code can switch among grayscale, RGB565, YUV422, and JPEG.
- It already contains sample logic for converting JPEG/RGB565/YUV422 into a
  `96 x 96` grayscale buffer.

Current deploy integration:

```text
esp/main/include/camera_capture.hpp
esp/main/src/camera_capture_ov2640.cpp
```

The OV2640 `.ino` pin mapping has been ported into `camera_capture_ov2640.cpp`.
The current default capture format is grayscale QQVGA (`160 x 120`), which is
stored into the `photos` partition and resized into the deploy runtime frame for
inference in `CAMERA_FLASH_MODE`.

Deploy integration target:

```text
camera_fb_t / stored image
  -> grayscale frame
  -> ESP hand crop
  -> 96 x 96 model input
```

Do not edit the original folder directly. Port future camera changes into
`esp/main/include` and `esp/main/src`.
