# NVIDIA Video Codec SDK — vendoring note

The NVENC encoder backend needs **one header** from the NVIDIA Video Codec SDK:
`Interface/nvEncodeAPI.h`. It is **not committed** to this repo — the NVIDIA
Video Codec SDK is distributed under NVIDIA's SDK license, so we do not
redistribute its headers. Fetch it yourself (free; requires an NVIDIA developer
account):

1. Download the **NVIDIA Video Codec SDK** from
   <https://developer.nvidia.com/video-codec-sdk> (any recent version works; the
   NVENC API header is backward-compatible).
2. Copy `Interface/nvEncodeAPI.h` from the SDK into:

   ```
   external/VideoCodecSDK/Interface/nvEncodeAPI.h
   ```

   (this `Interface/` directory is gitignored.)
3. Reconfigure CMake. You should see:

   ```
   -- AetherFlow: NVENC backend enabled (include: .../external/VideoCodecSDK/Interface)
   ```

If the header is absent, CMake **auto-disables NVENC** (a status message, not an
error) and the build still succeeds — it falls back to the Intel oneVPL backend
(which needs Intel hardware at runtime). The runtime NVENC implementation itself
comes from your installed NVIDIA GPU driver (`nvEncodeAPI64.dll`); only the
build-time header is needed here.

Alternatively, point CMake at an SDK you already have, without copying:

```bash
cmake -S . -B build -DNVENC_SDK_ROOT="C:/path/to/Video_Codec_SDK"
# CMake looks for ${NVENC_SDK_ROOT}/Interface/nvEncodeAPI.h
```
