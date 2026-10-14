# karute-ble

把原「Wi-Fi 热点 + 手机网页」的打字机魔改成 **BLE 蓝牙直连 + Android App** 的版本。
硬件是 ESP32-S3 / ESP32-C3（自带 BLE），通过 USB 伪装成键盘，把手机里粘贴的文字敲进目标电脑。

## 它是怎么工作的

```
Android App  ──BLE NUS (GATT)──►  ESP32-S3/C3  ──USB HID──►  目标电脑
   (选模式/速度/贴文字)              (打字引擎不变)          (当成真实键盘输入)
```

- **没有 Wi-Fi、没有网页、没有热点**。设备上电后广播 BLE，名字固定 `karute-ble`。
- App 扫描到 `karute-ble-*` 后连接，走 Nordic UART Service（NUS，标准 BLE 串口服务）。
- 打字引擎、NVS 设置、LittleFS 草稿、RGB LED、按键全部沿用原工程。
- 支持三种输入模式：**GBK**（Alt+小键盘，打中文）、**ENG**（直敲 ASCII）、**VUC**（vuc+十六进制）。

## 目录结构

```
karute-ble/
├─ platformio.ini              # 三个烧录目标：esp32s3 / esp32s3-devkit / esp32c3
├─ src/
│  ├─ main.cpp                 # BLE 服务 + 状态机 + HID 打字引擎
│  ├─ ble_protocol.h           # App↔固件命令行协议（纯逻辑，可本机单测）
│  ├─ typing_settings.h        # NVS 设置结构（沿用原工程）
│  ├─ alt_encoding.h            # UTF-8 解码 + GBK 查表 + Alt+numpad 时序
│  └─ led_policy.h              # LED 采样策略
├─ scripts/generate_gbk_table.py   # 构建时生成 Unicode→GBK 查表
├─ tests/                      # 本机 g++/node 可跑的纯逻辑测试
└─ android/                    # Android Studio 工程（Kotlin）
   ├─ settings.gradle
   ├─ build.gradle
   └─ app/src/main/java/com/karute/ble/
      ├─ MainActivity.kt        # 主界面
      ├─ BleManager.kt          # BLE 扫描/连接/收发队列
      └─ Protocol.kt            # 与固件对齐的命令协议
```

## 一、固件编译与烧录

需要 [PlatformIO Core](https://platformio.org/install/cli)。

```bash
cd karute-ble
pio run -e esp32s3           # 默认：M5Stack AtomS3U（GPIO41 按键 / GPIO35 LED）
pio run -e esp32s3-devkit    # 通用 S3 DevKit
pio run -e esp32c3           # ESP32-C3-DevKitM-1
```

烧录：

```bash
pio run -e esp32s3 -t upload
```

板级切换只需改 `platformio.ini` 里的环境名；换板子时若引脚不同，用 `-DKARUTE_BUTTON_PIN=x -DKARUTE_LED_PIN=y` 覆盖（各环境里已写好示例）。

> USB 策略保持原样：`ARDUINO_USB_MODE=0`，只暴露 HID 键盘，不开 CDC/MSC/DFU。改了会让电脑认不出键盘。

## 二、App 编译成 APK

用 Android Studio（Hedgehog 或更新）打开 `android/` 目录，等 Gradle 同步完，然后：

- 直接 Run：装到插着数据线的安卓手机。
- 出 release APK：菜单 `Build → Build App Bundle(s) / APK(s) → Build APK(s)`，产物在
  `android/app/build/outputs/apk/debug/app-debug.apk`（或 release 目录）。

最低 Android 8.0（API 24）。Android 12+ 运行时会弹一次「附近设备/蓝牙」权限，允许即可。

## 三、命令协议（改代码时两端必须同步）

App 往 NUS RX 特征值写一行一行的命令，行尾 `\n`。

| 方向 | 行 | 含义 |
|---|---|---|
| App→设备 | `T <base64>` | 追加 base64 解码后的 UTF-8 文本（可分多行，按到达顺序拼接） |
| App→设备 | `C RESET` | 清空草稿 |
| App→设备 | `C START / PAUSE / RESUME / STOP` | 打字控制 |
| App→设备 | `S k=v k=v ...` | 设置：mode(1/2/4)、vucDownMs、vucIntervalMs、engDownMs、engIntervalMs、altBeforeMs、altDownMs、altIntervalMs、altReleaseMs、altAfterMs、startDelayMs、inputLed、standbyLed |
| 设备→App | `OK` | 命令已受理 |
| 设备→App | `ERR <中文原因>` | 拒绝（如 Num Lock 未开、正文超 32KB） |
| 设备→App | `STATE <state>\|<bytes>\|<chars>\|<pct>\|<remainingMs>\|<error>` | 状态/进度，打字中每 300ms 推一次 |

## 四、本机已跑过的自检

没有 PlatformIO / Android SDK 的环境里，我对纯逻辑做了 g++ 实测：

```bash
python3 scripts/generate_gbk_table.py /tmp/gen        # 生成 21791 条 GBK 映射
g++ -std=c++17 -Isrc tests/ble_protocol_test.cpp      # 协议解析 + base64
g++ -std=c++17 -Isrc tests/typing_settings_test.cpp   # NVS 设置校验/迁移
g++ -std=c++17 -Isrc -I/tmp/gen tests/alt_encoding_test.cpp  # Alt 编码
```

这三组都通过。固件整包和 APK 必须在装好 PlatformIO / Android Studio 的机器上跑一次构建才算通过。

## 三、推到 GitHub 自动出 APK

仓库自带 `.github/workflows/build-android-apk.yml`：

- push 到 `main`，或在仓库 Actions 页手动点 Run → 自动编译 debug APK，打成 `karute-ble-apk.zip`，在该次运行的 **Artifacts** 区下载。
- 打 tag（`git tag v1.0.0 && git push origin v1.0.0`）→ 自动把 zip 挂到 GitHub **Releases** 页，手机直接下载装。

不用在本地装 Android Studio 也能拿到 APK。

```bash
cd karute-ble
git init
git add .
git commit -m "karute-ble: BLE version"

# 在 GitHub 建一个空仓库后：
git remote add origin git@github.com:<你的用户名>/karute-ble.git
git branch -M main
git push -u origin main

# 等 Actions 跑完，在仓库 Actions 标签页下载 artifact；
# 打 tag 走 Release：
git tag v1.0.0
git push origin v1.0.0
```

## 四、已知坑

1. **GBK 模式必须 Num Lock 开着**。固件会读主机 LED 状态，Num Lock 没亮直接拒绝开始。
2. **GBK 模式是全篇预检**：正文里只要有一个字不在 GBK 里，一个字都不打，并报「第 N 个字符 U+XXXX 不支持」。这是刻意的，避免打一半留半截。
3. **VUC 模式依赖目标机的输入法**支持 vuc 功能（微信/搜狗等），没有就别用。
4. BLE 写队列在 App 端是串行的，一次只发一个未完成写；不要绕过 `BleManager.enqueue()` 直接写特征值。
5. 设备名、NUS UUID、按键/LED 引脚都在 `src/main.cpp` 顶部的常量里，集中改即可。
