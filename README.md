# fyewebcam

Use an Android phone as a webcam for a Windows PC.

- **android/**: the phone app. It streams the camera as H.264 over Wi-Fi or USB, and keeps
  streaming with the screen off.
- **windows/**: the PC client. It connects to the phone and shows the video, with mirror,
  rotate and full screen. `windows/vcam/` is the virtual webcam (Windows 11), so Zoom, Teams,
  OBS, browsers and the Camera app can pick "fyewebcam" as a camera.
- **tools/**: `fake_phone.py` streams a test pattern so you can try the client without a phone,
  and `test_client.py` saves the phone's stream to a file.
- **docs/PROTOCOL.md**: the wire protocol between the two apps.

## Get the apps

Every push builds both apps in GitHub Actions. Open the latest run under **Actions**, then
download `fyewebcam-android` (the APK) and `fyewebcam-windows` (the exe with `adb` for USB mode).

## Use it

1. Install the APK on the phone, open fyewebcam and tap **Start streaming**.
2. Run `fyewebcam.exe` on the PC.
   - **Wi-Fi:** type the IP shown on the phone and click **Connect**. Both must be on the same network.
   - **USB:** turn on USB debugging on the phone (Settings > Developer options), plug it in,
     pick **USB** and click **Connect**. Allow the debugging prompt on the phone the first time.
3. For the best picture, hold the phone in landscape. Use **Rotate** if the picture is sideways.
4. **Virtual webcam (Windows 11):** click **Install virtual webcam** once and approve the Windows
   prompt. Then pick "fyewebcam" as the camera in any app. It shows the phone while the PC app is
   connected, and a dark picture otherwise. **Remove virtual webcam** takes it away again.

## Build it yourself

**Android** (Android Studio, or JDK 17 and the Android SDK):

```
cd android
./gradlew assembleDebug        # app/build/outputs/apk/debug/app-debug.apk
./gradlew testDebugUnitTest
```

**Windows** (Visual Studio 2022 with the C++ workload, or CMake with MSVC):

```
cmake -S windows -B windows/build -A x64
cmake --build windows/build --config Release   # windows/build/Release/fyewebcam.exe
ctest --test-dir windows/build -C Release
```

**Try the client without a phone** (needs Python 3 and ffmpeg):

```
python tools/fake_phone.py
```

Then connect the client to `127.0.0.1`.
