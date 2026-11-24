# Third-Party Notices

RevDock 以源码形式内置以下第三方组件。每个组件保留其原始 LICENSE 与 README，
并按构建需要删除了文档、测试、示例和 IDE 工程。完整版本与裁剪规则见
`lib/README.md`。

| 组件 | 版本 | 许可证 | 用途 | 位置 |
|---|---|---|---|---|
| SDL3 | 3.4.20 | zlib | 窗口、输入、2D 渲染、文件系统 | `lib/SDL3` |
| SDL3_ttf | 3.2.2 | zlib | 字体渲染、fallback 字体 | `lib/SDL3_ttf` |
| SDL3_image | 3.4.8 | zlib | PNG/JPEG/SVG 图标 | `lib/SDL3_image` |
| FreeType | 2.14.3 | FreeType License (FTL) | 字体解析 | `lib/freetype` |
| nlohmann/json | 3.12.0 | MIT | 配置、缓存、应用索引 | `lib/nlohmann` |
| doctest | 2.5.3 | MIT | 单元测试 | `lib/doctest` |
| Inter | 4.1 | SIL OFL 1.1 | 默认 UI 字体 | `assets/fonts/Inter` |

### SDL3 / SDL3_ttf / SDL3_image

zlib license。允许商业使用、修改和再分发；保留版权声明与许可证文本即可。

### FreeType

FreeType Project License（FTL）。允许商业使用、修改和再分发；保留
`lib/freetype/LICENSE.TXT`。本项目未修改 FreeType 源码。

### nlohmann/json

MIT。单头文件 `json.hpp` 与 `LICENSE.MIT`。

### doctest

MIT。单头文件 `doctest.h` 与 `LICENSE.txt`，仅测试目标使用。

### Inter

SIL Open Font License 1.1。允许免费使用、研究、修改和再分发，包括商业用途。
本项目未修改字体文件，保留 `assets/fonts/Inter/OFL.txt`。

### 未分发的系统字体

中文回退字体由运行时从操作系统读取，不随 RevDock 分发，也不复制
`reverse/samples` 或 macOS 样本中的任何字体。

### 图标资源

应用图标由运行时的 Windows Shell / Linux icon theme 提供，并在运行时转换为 macOS 风格（不使用第三方图标包）。
Dock 的 Trash、Folder 等系统图块由 RevDock 自己绘制；Launchpad 图块使用用户提供的
`assets/icons/launchpad_dark.icns` 与 `assets/icons/launchpad_light.icns`。不包含
`HurleyWong/macOS-Big-Sur-icon-collection` 的内容（该仓库为 GPL-3.0，
且其主要内容是第三方应用替换图标，而非系统图标）。
