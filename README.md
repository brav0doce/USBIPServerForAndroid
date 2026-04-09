# USB/IP Server for Android

[![Build](https://github.com/brav0doce/USBIPServerForAndroid/actions/workflows/build.yml/badge.svg)](https://github.com/brav0doce/USBIPServerForAndroid/actions/workflows/build.yml)

An Android application that implements a **USB/IP server**, allowing USB devices physically connected to an Android phone or tablet to be shared with other computers over the network using the [USB/IP protocol](https://docs.kernel.org/usb/usbip_protocol.html).

---

## Table of Contents

- [What is USB/IP?](#what-is-usbip)
- [How It Works](#how-it-works)
- [Requirements](#requirements)
- [How to Use](#how-to-use)
  - [Android (Server side)](#android-server-side)
  - [Windows Client (usbip-win2)](#windows-client-usbip-win2)
  - [Linux Client](#linux-client)
- [How to Compile](#how-to-compile)
  - [Prerequisites](#prerequisites)
  - [Build from Android Studio](#build-from-android-studio)
  - [Build from Command Line](#build-from-command-line)
  - [Building a Signed Release APK](#building-a-signed-release-apk)
- [Architecture](#architecture)
- [Compatibility](#compatibility)
- [Troubleshooting](#troubleshooting)
- [License](#license)

---

## What is USB/IP?

USB/IP is an open protocol that allows USB devices to be shared over an IP network as if they were locally attached. It was originally developed for Linux kernels and is documented at [https://docs.kernel.org/usb/usbip_protocol.html](https://docs.kernel.org/usb/usbip_protocol.html).

This app implements the **server (stub) side** of that protocol on Android. You plug a USB device into your Android phone's OTG port, run this app, and then any USB/IP client on another machine (Windows, Linux) can claim and use that USB device remotely.

---

## How It Works

```
 [USB Device] ──OTG──> [Android Phone running this app]
                                │
                           TCP port 3240
                                │
                         [Windows / Linux]
                      running a USB/IP client
```

1. **Device enumeration** – The app uses Android's `UsbManager` API to list all connected USB devices and their descriptors (vendor ID, product ID, class, interfaces, endpoints).
2. **TCP server** – A foreground service starts a TCP server on port **3240** (the standard USB/IP port).
3. **USB/IP handshake** – When a client connects, the server responds to `OP_REQ_DEVLIST` (list available devices) and `OP_REQ_IMPORT` (attach to a device) requests using the standard USB/IP wire protocol.
4. **URB forwarding** – Once a device is claimed, every USB Request Block (URB) sent by the client (`USBIP_CMD_SUBMIT`) is translated into a real USB transfer on the Android side:
   - **Bulk transfers** are performed via the `USBDEVFS_BULK` ioctl through a native JNI library (`libusblib.so`), which bypasses Android's Java USB API to achieve lower latency.
   - **Control transfers** are performed via the `USBDEVFS_CONTROL` ioctl similarly.
   - **Interrupt transfers** are handled through Android's `UsbRequest` API.
   - Results are sent back to the client as `USBIP_RET_SUBMIT` replies.
5. **Unlink** – `USBIP_CMD_UNLINK` (abort a pending URB) is also supported.
6. **Wake locks** – The service holds a CPU wake lock and a high-performance Wi-Fi lock to prevent the connection from being interrupted by the power manager.

### Key source files

| File | Purpose |
|------|---------|
| `UsbIpConfig.java` | Main Activity – start/stop the service |
| `UsbIpService.java` | Foreground Service – owns USB connections and implements `UsbRequestHandler` |
| `UsbIpServer.java` | TCP server accepting client connections on port 3240 |
| `UsbRequestHandler.java` | Interface between the server and the service |
| `protocol/cli/` | Packets for the control channel (device list, import) |
| `protocol/dev/` | Packets for the device channel (submit URB, unlink URB, replies) |
| `usb/XferUtils.java` | USB transfer helpers (bulk, control, interrupt) |
| `jni/usblib/usblib_jni.c` | Native C code performing `USBDEVFS_BULK` / `USBDEVFS_CONTROL` ioctls |

---

## Requirements

### Android (server)

- Android 5.0 (API 21) or newer
- A USB OTG adapter/cable (to connect USB devices to the phone)
- The USB device connected to the phone before or after starting the service
- Network connectivity (Wi-Fi recommended for best throughput)

### Client (Windows)

- [usbip-win2](https://github.com/vadimgrn/usbip-win2) — a modern Windows USB/IP client driver

### Client (Linux)

- Kernel module `usbip-core` + `vhci-hcd` (usually in the `linux-tools` or `usbip` package)

---

## How to Use

### Android (Server side)

1. Connect your USB device to the Android phone via an OTG adapter.
2. Install the APK (from [Releases](../../releases) or built yourself).
3. Open **USB/IP Server** and tap **Start Service**.
4. Grant the notification permission when prompted (required on Android 13+).
5. When the client tries to claim a device, Android will show a permission dialog — tap **OK**.
6. The service runs in the background as a foreground service and shows a persistent notification.
7. Note the phone's IP address (shown in the ready screen, or check your Wi-Fi settings).

### Windows Client ([usbip-win2](https://github.com/vadimgrn/usbip-win2))

1. Install the usbip-win2 driver and tools following its [README](https://github.com/vadimgrn/usbip-win2?tab=readme-ov-file).
2. Open a command prompt and list available devices:
   ```cmd
   usbip list -r <android-ip>
   ```
3. Attach a device (replace `1-1` with the bus ID shown in the list):
   ```cmd
   usbip attach -r <android-ip> -b 1-1
   ```
4. The USB device now appears as locally attached on Windows.
5. To detach:
   ```cmd
   usbip detach -p <port>
   ```

### Linux Client

```bash
# Load kernel modules
sudo modprobe vhci-hcd

# List devices exported by the Android phone
usbip list -r <android-ip>

# Attach a device (replace 1-1 with the correct bus ID)
sudo usbip attach -r <android-ip> -b 1-1

# Detach when done
sudo usbip detach -p 0
```

---

## How to Compile

### Prerequisites

| Tool | Recommended version |
|------|-------------------|
| Android Studio | Ladybug (2024.2) or newer |
| JDK | 17 |
| Android SDK | API 35 (compile & target) |
| Android NDK | 27.0.12077973 |
| Gradle | 8.9 (wrapper included) |

> **NDK note:** The app contains native C code (`usblib_jni.c`) compiled via `ndk-build`. The NDK version is pinned in `app/build.gradle` (`ndkVersion`). Android Studio will prompt you to install it automatically.

### Build from Android Studio

1. Clone the repository:
   ```bash
   git clone https://github.com/brav0doce/USBIPServerForAndroid.git
   cd USBIPServerForAndroid
   ```
2. Open the project in Android Studio (`File → Open`).
3. Let Android Studio sync and download dependencies (Gradle + NDK).
4. Build a debug APK: **Build → Build Bundle(s) / APK(s) → Build APK(s)**.
5. The APK is written to `app/build/outputs/apk/debug/app-debug.apk`.

### Build from Command Line

```bash
# Linux / macOS
./gradlew assembleDebug

# Windows
gradlew.bat assembleDebug
```

The output APK will be at `app/build/outputs/apk/debug/app-debug.apk`.

To build the release variant (unsigned):

```bash
./gradlew assembleRelease
```

Output: `app/build/outputs/apk/release/app-release-unsigned.apk`

### Building a Signed Release APK

To install outside of the Play Store you need a signed APK.

1. **Create a keystore** (only needed once):
   ```bash
   keytool -genkeypair -v \
     -keystore my-release-key.jks \
     -alias my-key-alias \
     -keyalg RSA -keysize 2048 \
     -validity 10000
   ```

2. **Add signing config** to `app/build.gradle`:
   ```groovy
   android {
       signingConfigs {
           release {
               storeFile file("../my-release-key.jks")
               storePassword System.getenv("KEYSTORE_PASSWORD")
               keyAlias "my-key-alias"
               keyPassword System.getenv("KEY_PASSWORD")
           }
       }
       buildTypes {
           release {
               signingConfig signingConfigs.release
               minifyEnabled false
               proguardFiles getDefaultProguardFile('proguard-android.txt'), 'proguard-rules.txt'
           }
       }
   }
   ```

3. **Build and sign**:
   ```bash
   KEYSTORE_PASSWORD=<pass> KEY_PASSWORD=<pass> ./gradlew assembleRelease
   ```

   Output: `app/build/outputs/apk/release/app-release.apk`

> Automated release builds are also produced by the GitHub Actions workflow defined in `.github/workflows/build.yml`. Published APKs are always signed (stable tags with your configured release keystore; pre-releases with a temporary CI keystore), so they are installable on-device.

---

## Architecture

```
┌──────────────────────────────────────────┐
│            UsbIpConfig (Activity)        │  ← Start / Stop button
└────────────────────┬─────────────────────┘
                     │ startService()
┌────────────────────▼─────────────────────┐
│         UsbIpService (Foreground Svc)    │
│  • Holds UsbManager references           │
│  • Manages AttachedDeviceContext map     │
│  • Implements UsbRequestHandler          │
└────────────────────┬─────────────────────┘
                     │
┌────────────────────▼─────────────────────┐
│             UsbIpServer                  │
│  • TCP ServerSocket on port 3240         │
│  • One thread per client connection      │
└────────────────────┬─────────────────────┘
                     │
         ┌───────────┴───────────┐
         │ Control channel       │ Device channel
         │ (OP_REQ_DEVLIST /     │ (USBIP_CMD_SUBMIT /
         │  OP_REQ_IMPORT)       │  USBIP_CMD_UNLINK)
         └───────────────────────┘
                     │
┌────────────────────▼─────────────────────┐
│    XferUtils  +  UsbLib (JNI)            │
│  • Bulk / Control via usbdevfs ioctls    │
│  • Interrupt via Android UsbRequest API  │
└──────────────────────────────────────────┘
```

---

## Compatibility

| Android version | API | Status |
|-----------------|-----|--------|
| 5.0 – 12        | 21–32 | ✅ Supported |
| 13 (Tiramisu)   | 33  | ✅ Supported (POST_NOTIFICATIONS permission) |
| 14 (UpsideDownCake) | 34 | ✅ Supported (FOREGROUND_SERVICE_SPECIAL_USE) |
| 15+             | 35+ | ✅ Supported (16 KB page-size compatible NDK build) |

The NDK build produces shared libraries for all ABIs (`armeabi-v7a`, `arm64-v8a`, `x86`, `x86_64`) and supports 16 KB memory pages (`APP_SUPPORT_FLEXIBLE_PAGE_SIZES := true`) as required by Android 15+ devices.

---

## Troubleshooting

**The client cannot connect**
- Make sure the Android firewall / hotspot does not block port 3240.
- Ensure both devices are on the same network.
- Verify the service is running (notification should be visible).

**Device list is empty**
- Reconnect the USB device and restart the service.
- Check that Android granted USB permission (dialog should have appeared).

**Transfer errors / device not responding**
- Some USB devices use isochronous endpoints which are not currently supported.
- Try a shorter OTG cable; signal quality matters at USB 2.0 speeds.

**`usbip attach` fails on Windows**
- Make sure usbip-win2 driver is installed and loaded correctly.
- Run the command prompt as Administrator.

---

## License

See [LICENSE](LICENSE).
