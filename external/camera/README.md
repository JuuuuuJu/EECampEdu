# Camera Team Reference

Original source:

```text
C:\Users\user\OneDrive\桌面\OV2640-main
```

Important findings:

- Current default camera format is `PIXFORMAT_GRAYSCALE`.
- Current default frame size is `FRAMESIZE_96X96`.
- The reference code can switch among grayscale, RGB565, YUV422, and JPEG.
- It already contains sample logic for converting JPEG/RGB565/YUV422 into a
  `96 x 96` grayscale buffer.

Deploy integration target:

```text
camera_fb_t / stored image
  -> grayscale frame
  -> ESP hand crop
  -> 96 x 96 model input
```

Do not edit the original folder directly. Port the required camera code into
`esp/main/include` and `esp/main/src`.
