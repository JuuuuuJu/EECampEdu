# Input Interface Reference

Reference source:

```text
CDC/MSC input-interface team reference project
```

Important findings:

- The reference uses TinyUSB composite CDC + MSC.
- CDC receives text commands.
- MSC exposes flash-backed storage to the host.
- The reference command is currently centered around `capture` image transfer.

Deploy integration target:

```text
PC command
  -> USB CDC parser
  -> CAPTURE / INFER_LAST / GET_RESULT / GET_IMAGE
  -> structured ESP response
```

The protocol draft is in `interfaces/usb_protocol.md`.
