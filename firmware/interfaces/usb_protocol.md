# USB / Serial Protocol Draft

The input-interface team can change transport details, but the deploy firmware
should keep command semantics stable.

## Commands

| Command | Direction | Purpose |
| --- | --- | --- |
| `PING` | PC -> ESP | Check device is alive |
| `CAPTURE` | PC -> ESP | Capture one image and store it in flash |
| `INFER_LAST` | PC -> ESP | Run inference on the latest stored image |
| `CAPTURE_INFER` | PC -> ESP | Capture, store, preprocess, infer |
| `GET_RESULT` | PC -> ESP | Return latest prediction and scores |
| `GET_IMAGE` | PC -> ESP | Stream latest stored image to PC |

## Responses

```text
READY
OK,<command>
ERROR,<reason>
RESULT,<pred_idx>,<model_us>,<preprocess_us>,<device_us>,<score0>,<score1>,<score2>,<score3>,<score4>
IMAGE_BEGIN,<filename>,<bytes>,<format>
...binary image payload...
IMAGE_END
```

During benchmark, if ESP reboots and sends `READY` while PC waits for `RESULT`,
the host script should resend the current frame or abort after a retry limit.
