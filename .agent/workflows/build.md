---
description: 编译 Pixel Caddy ESP32-S3 固件
---

# 编译 Pixel Caddy 固件

使用 Arduino IDE 内置的 arduino-cli 编译 ESP32-S3 固件，并通过 OTA 上传。

// turbo-all

1. 自动递增构建版本号

默认递增第三位（构建版本）。如果用户说"加大版本"则递增第一位并重置后两位，"加小版本"则递增第二位并重置第三位。

```
$file = "f:\pixel caddy\pixelcaddys3\pixelcaddys3.ino"; $content = Get-Content $file -Raw; if ($content -match '#define FIRMWARE_VERSION "v(\d+)\.(\d+)\.(\d+)"') { $major = [int]$Matches[1]; $minor = [int]$Matches[2]; $patch = [int]$Matches[3]; $patch++; $newVer = "v$major.$minor.$patch"; $content = $content -replace '#define FIRMWARE_VERSION "v\d+\.\d+\.\d+"', "#define FIRMWARE_VERSION `"$newVer`""; Set-Content $file $content -NoNewline; Write-Host "Version bumped to $newVer" } else { Write-Host "ERROR: Version not found" }
```

2. 编译固件

```
& "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" compile --fqbn esp32:esp32:XIAO_ESP32S3 "f:\pixel caddy\pixelcaddys3\pixelcaddys3.ino" --config-file "C:\Users\phantasy\AppData\Local\Arduino15\arduino-cli.yaml" --warnings default --output-dir "f:\pixel caddy\pixelcaddys3\build\esp32.esp32.XIAO_ESP32S3"
```

编译成功后会输出固件大小和内存使用情况。如果编译失败，检查输出中的错误信息并修复代码。

3. 上传固件到设备 (OTA)

确保设备已进入 OTA 模式（LED 显示 "IP 36"），然后执行以下命令自动上传固件：

```
$binPath = "f:\pixel caddy\pixelcaddys3\build\esp32.esp32.XIAO_ESP32S3\pixelcaddys3.ino.bin"; $hash = (Get-FileHash -Path $binPath -Algorithm MD5).Hash.ToLower(); Write-Host "Firmware: $binPath"; Write-Host "MD5: $hash"; Write-Host "Starting OTA..."; $startResult = Invoke-WebRequest -Uri "http://10.10.10.36/ota/start?mode=fr&hash=$hash" -Method Get -TimeoutSec 5 -UseBasicParsing; Write-Host "OTA Start: $($startResult.StatusCode)"; Write-Host "Uploading firmware..."; curl.exe -F "file=@$binPath" http://10.10.10.36/ota/upload --progress-bar; Write-Host "OTA upload complete! Device will restart."
```

上传完成后设备会自动重启。
