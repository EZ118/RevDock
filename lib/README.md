# Vendored dependencies

所有第三方源码都在 `lib/` 下，便于统一管理、审计和离线构建。

| 目录 | 版本 | 许可证 | 保留内容 |
|---|---|---|---|
| `lib/SDL3` | 3.4.20 | zlib | `CMakeLists.txt`、`cmake/`、`include/`、`src/`、`wayland-protocols/`、`LICENSE.txt`、`README.md`、`CREDITS.md` |
| `lib/SDL3_ttf` | 3.2.2 | zlib | `CMakeLists.txt`、`cmake/`、`include/`、`src/`、`LICENSE.txt`、`README.md` |
| `lib/SDL3_image` | 3.4.8 | zlib | `CMakeLists.txt`、`cmake/`、`include/`、`src/`、`LICENSE.txt`、`README.md` |
| `lib/freetype` | 2.14.3 | FTL | `CMakeLists.txt`、`builds/`、`include/`、`src/`、`LICENSE.TXT`、`README` |
| `lib/nlohmann` | 3.12.0 | MIT | `single_include/nlohmann/json.hpp`、`LICENSE.MIT`、`README.md` |
| `lib/doctest` | 2.5.3 | MIT | `doctest.h`、`LICENSE.txt`、`README.md` |

## 裁剪规则

- 删除 `docs/`、`test/`、`tests/`、`examples/`、`demos/`、IDE 工程
  （`VisualC/`、`Xcode/`）、Android 工程、`.github/`、`.git*`。
- 保留 `LICENSE`、`LICENSE.txt`、`LICENSE.MIT`、`README`、`README.md`、
  `CREDITS.md` 等合规文件。
- 不保留任何 Apple 字体、图片或系统资源。
- FreeType 需要 `builds/` 下的 CMake/平台配置，因此保留 `builds/`。
- SDL3 保留 `build-scripts/` 供可选 wiki 头文件目标使用，不影响构建。

## 重新拉取

```powershell
pwsh tools/fetch_deps.ps1
python tools/trim_deps.py
python tools/verify_deps.py
```

拉取使用 `https://cdn.gh-proxy.org/https://github.com/...` 加速前缀；
如果网络可以直连 GitHub，也可以修改脚本中的 `$proxy`。

## 自定义选项

- SDL3 静态编译，关闭 audio/camera/joystick/haptic/sensor/power。
- SDL3_ttf 关闭 HarfBuzz、PlutoSVG，使用 `lib/freetype`。
- SDL3_image 关闭 AVIF/JXL/TIFF/WebP/libpng，保留内置 stb + SVG。
- 不使用系统 SDL 时，构建完全由仓库内源码完成。
