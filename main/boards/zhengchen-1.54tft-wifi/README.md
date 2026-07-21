# Product Reference

```text
https://e.tb.cn/h.6Gl2LC7rsrswQZp?tk=qFuaV9hzh0k CZ356
```

# Build Configuration

Set the target to ESP32-S3:

```bash
idf.py set-target esp32s3
```

Open menuconfig:

```bash
idf.py menuconfig
```

Select the board:

```text
Xiaozhi Assistant -> Board Type -> zhengchen-1.54tft-wifi
```

Build:

```bash
idf.py build
```

Build, flash, and monitor:

```bash
idf.py build flash monitor
```

Generate a merged firmware image:

```bash
idf.py merge-bin
```
