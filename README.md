# RevDock

RevDock 是一个用 **C++20 + SDL3 + CMake** 从源码层面复刻 macOS Dock 的跨平台项目。
逆向资料用于提取布局、动画、配置和交互参数，运行时代码不依赖 macOS，也不复制
Apple 的二进制、图片或字体。

当前版本重点完成了 Windows 上的可运行核心 Dock，并为 Linux/X11 提供了同一套
引擎和平台实现。Wayland 会以“浮动窗口 + 降级能力”运行。

## 已实现

- 透明、无边框、置顶、不显示任务栏图标的 Dock 窗口
- Tile 单位布局、底部/左侧/右侧、屏幕宽度自适应
- 悬停放大（cosine 包络）、移动、启动 bounce
- 自动隐藏、延迟、动画倍速
- 无 Dock 阴影；模糊、叠色与 1px 边框烘焙进同一张圆角纹理，边角完全对齐
- 分割线更短（命中区域 5px）；只有分割线/Dock 背景的右键菜单显示 Dock 设置
- 图标拖拽时图标本体跟随鼠标，其他图标实时让位；从启动台拖入 Dock 也有插入预览
- 实时高斯模糊背景：内容变化时约 70ms 刷新、静止时降到 150ms；Windows 屏幕捕获 + 降采样模糊 + GPU 双线性放大，失败时回退到壁纸模糊，外圈一圈 1px 细边框
- 1 px 竖向分隔条；图标间距、左右边距与上边距一致
- Windows 任务栏启动时隐藏、正常退出时恢复
- 自动隐藏时 Dock 完全移出屏幕，仅屏幕最底部 1 像素可唤出
- 应用图标、带下指小三角的悬浮标签、运行指示点
- 所有软件图标自动转换为 macOS 风格：裁透明边、识别纯色/透明/复杂背景、主色渐变底、
  标准圆形圆角（占 805/1024 画布的 22.37%）+ 烘焙投影；纯色底占多数的图标（如 UWP 的设置/终端/Realtek）会保留底色，并把中间主体缩放到带边距的居中尺寸，而不是铺满边缘；可在 Dock 菜单里关闭
- 启动台图标使用 `assets/icons/launchpad_dark.icns` / `launchpad_light.icns`（按主题选择）
- 文件管理器统一命名为 Finder，默认固定在最左侧（使用 `assets/icons/finder.png`）；Finder 与 Launchpad 可移动但不可取消固定，分隔线和废纸篓固定不可拖动。Finder 是唯一的文件管理器图标（不再显示 Explorer/Nautilus 等），显示其运行状态且没有关闭/退出菜单
- Finder 与 Launchpad 素材会裁边并归一化到同一显示尺寸/形状，与软件图标观感一致
- Launchpad/Trash 同样有 Tooltip 和悬停放大
- 右键菜单打开时 Dock 放大冻结；启动未运行的程序时图标持续跳动直到进程出现
- Launchpad 只列程序快捷方式（文档/URL/帮助文件会被过滤），无标题和底部提示，分页点在 Dock 上方
- 拖拽排序时其他图标带可打断的滑动动画
- 图标经反锐化掩模锐化，小尺寸 Shell 图标也能保持清晰；图标纹理使用双线性过滤，圆角遮罩使用解析式抗锯齿，放大时不会出现台阶感/钝角
- 支持 UWP/打包应用：从 shell:AppsFolder 枚举应用（设置、终端、Realtek Audio Console、Windows 安全中心等会出现在启动台），通过 AUMID 启动；把 UWP 快捷方式拖到 Dock 会固定对应应用而不是当成普通文件
- 从启动台启动时，Dock 立即出现该图标并按反编译曲线跳动
- 启动台过滤安装/卸载程序（msiexec、uninst*）以及 Linux 的 shell 包装项
- 启动台打开时 Dock 使用同款圆角半透明材质，不再闪烁
- 启动台支持左右滑动翻页（跟手 + 惯性回弹动画），滚轮/触摸板/方向键也有快速滑动动画
- 启动后台逐个预热启动台图标缓存（仅在空闲时，每 100ms 一个），首次打开不再卡顿
- 支持 `.url` 自定义协议快捷方式（steam://、bilibili:// 等），过滤 http/ftp 等网页链接
- 文件夹图标使用用户提供的 macOS 文件夹素材（`assets/icons/folder_empty.png` / `folder_nonempty.png`，已裁剪），按原始宽高比显示；默认启用堆栈模式，右键菜单“Display as”可切换为文件夹图标
- 文件夹堆栈模式在 Dock 图标上叠放最多 3 个文件图标；空文件夹显示空文件夹图标。点击堆栈展开为扇形或网格；文件夹图标模式下点击直接打开资源管理器/文件管理器
- 文件夹栈以竖向扇形展开：图标由下往上排列并沿 3000pt 圆弧向右弯曲，文件名显示在图标左侧，最底下与 Dock 文件夹图标对齐；顶部是“Open in Finder”箭头行，可直接打开文件夹；项目过多时自动回退到分页网格
- 网格模式使用与右键菜单同款的模糊材质；文件图标保持原始比例（与扇形一致），悬停时不再显示亮色圆角背景
- 扇形中的文件使用系统原始图标（不做 macOS 圆角图标转换、不加图标底板），保持原始比例；文件名与图标同角度倾斜，沿扇形切线定位使右端与图标中心对齐，文本在背景圆角矩形中居中（带 padding、8px 圆角，不会拉成胶囊）；悬停文件图标不会放大；Open in Finder 箭头与文件图标可见尺寸一致；展开/收起都有 160ms 动画；展开时 Dock 文件夹图标切换为 pileArrow 图标，并随文件夹图标的放大/位移移动
- Dock 顺序按 macOS 分类：Finder + Launchpad + 固定 App、最近 3 个/未固定但打开的 App、文件夹、最小化窗口缩略图、废纸篓；最小化窗口缩略图最大只比软件图标大 1-2px，按窗口比例缩放并带右下角 App 图标角标，可点击还原、右键还原/关闭；进入和离开 Dock 的放大效果有 160ms 过渡
- Dock 上的文件/文件夹使用系统原始图标并保持原始比例，不做圆角方块化；软件图标始终显示软件名称（最小化窗口、文件、文件夹除外）
- Grid 文件夹背景使用与 Dock 一致的模糊材质和 1px 边框；把文件拖到 App 图标上时不避让、图标变暗，停留 500ms 会让运行中的 App 图标闪两下并置前，松开即用该 App 打开文件
- 扇形中的文件可以直接拖出：可拖到桌面/其他程序，拖到废纸篓会移入回收站，拖到 Dock 其他位置会添加为新的 Dock 项
- Dock 上已有的文件/文件夹拖到软件图标上时不会让位或重排，软件图标按图标形状变暗，松开即用该软件打开；拖到文件夹会移入，拖到废纸篓会回收
- Dock 用两条固定分割线分成三个区域：固定软件 | 最近使用/打开的软件（不足 3 个用最近关闭的软件补齐） | 文件/文件夹、最小化窗口和废纸篓；图标之间的间距比外侧留白小 1/4
- 自动隐藏只在屏幕最后 1px 触发，停留 0.1s 才显示；移开 1px 即取消。隐藏时 Dock 完全移出屏幕，不再残留不同高度的边
- Dock 背景改为从“最终可见位置”预热截屏，滑入时不再出现黑色渐变；捕获去掉了每帧的 affinity/DwmFlush 往返，窗口移动时模糊刷新约 24fps
- 最小化/恢复共用 Genie：恢复时从最小化缩略图展开回窗口；最小化终点使用缩略图的实际显示矩形（与最终 tile 比例一致），自动隐藏时也会先唤出 Dock 再播放
- Windows 任务栏的 work-area 占位会随 Dock 运行一起移除（任务栏临时设为自动隐藏，退出恢复）；“Reserve screen space” 设置已移除：不自动隐藏时预留正好到 Dock 顶边的空间，自动隐藏时不预留
- Dock 默认下移 1px，预留空间与 Dock 顶边对齐（最大化窗口与 Dock 之间没有空隙）
- 最小化/恢复的 Genie 覆盖层只在动画期间显示，平时隐藏，保证桌面和 Dock 都能正常点击；恢复时缩略图立即从 Dock 移除，反向 Genie 从缩略图位置展开到窗口
- 正常关闭（Alt+F4 / WM_CLOSE）会完整退出并恢复 Windows 任务栏状态、工作区和窗口过渡设置
- 右键菜单打开期间 Dock 放大效果保持冻结，鼠标移到菜单上不会缩回；菜单拥有独立窗口并始终位于 Launchpad 之上，Launchpad 打开时右键菜单仍可正常操作
- Launchpad 已打开时再次点击 Launchpad 图标会关闭它（开关切换）
- 中间“最近使用/运行中”区域的图标支持拖拽排序；固定图标的拖拽排序落点偏差 1 位的问题已修复
- 最小化缩略图插入/移除时，两侧图标会用约 0.2s 的滑动动画让位/合拢，而不是瞬间跳动
- Launchpad 支持触摸板双指左右滑动翻页：手势过程中页面跟手，松手后按拖拽阈值吸附/翻页，并带边缘阻尼；方向与手指移动一致
- 触摸屏/多指触摸（`SDL_EVENT_FINGER_*`，两指及以上）也走同一套翻页逻辑；Windows 精密触摸板由 `WM_MOUSEHWHEEL → SDL_EVENT_MOUSE_WHEEL(wheel.x)` 提供输入
- “Open in Finder”使用 macOS 的 openinfinder 图标；废纸篓使用系统资源中的亮/暗、空/满图标，并按回收站内容自动切换
- 启动台显示期间 Dock 通过窗口所有权保持在其上方，鼠标来回移动不再闪烁
- Windows 开始菜单应用扫描、图标提取、启动/激活
- 运行状态检测同时参考可见窗口与全部进程映像：由 Windows Terminal 承载的 cmd/PowerShell 也能显示运行白点；Office 2013 的 `wordicon.exe` 等图标桩会通过 App Paths 解析为真正的 `WINWORD.EXE`/`EXCEL.EXE` 后再启动，启动时若程序换了进程名（启动器/宿主）会自动绑定别名，不再出现图标一直跳动、永远等不到运行状态的问题
- Dock 运行指示点与 Launchpad 页码点改为按设备分辨率烘焙的抗锯齿圆点（4-5px 也清晰），圆角面板/菜单高亮按设备像素对齐，避免半像素模糊和右/下边的伪边框
- 应用缓存（24 小时）、单实例、配置防抖原子写入
- 首次运行没有 `launchpad` 配置时，启动台按 Windows 开始菜单的文件夹树生成初始分页：只取最外层文件夹名（更深的子文件夹并入最外层），只有一个软件的文件夹不建文件夹、直接显示独立图标，UWP 应用永远独立显示
- 配置写盘改为空闲合并策略：拖拽/滑动手势进行期间不写盘，停止操作 2 秒后最多每 3 秒写一次，正常退出时强制落盘；一次拖拽排列只产生一次原子写入
- 点击 Dock 图标时会先恢复被隐藏/最小化到托盘的主窗口（忽略 16px 托盘辅助窗口），QQ 这类关闭到托盘的应用不再新开实例；没有可恢复窗口的应用（如 Rayburst）会重新唤起已有单实例并显示窗口
- 重绘设置窗口：分组卡片、应用图标与渐变标题栏、圆角分段选择器、胶囊开关、带数值标签的滑杆、悬停反馈和整齐的底部操作区
- Dock 右键菜单的 Quit 现在会真正退出整个程序，而不只是关闭当前窗口：向应用的所有顶层窗口发送 WM_QUERYENDSESSION/WM_ENDSESSION 优雅关闭请求，应用可正常保存或拒绝退出；全程不会 TerminateProcess，强制结束仍只由单独的 Force Quit 提供
- 自绘右键菜单（应用/文件夹/废纸篓/Dock 背景），背景为 Dock 同款模糊材质，底部有指向图标的三角，分割线间距紧凑；打开菜单时隐藏 Tooltip
- 与 macOS `DockMenus.plist` 对齐的 Dock/进程菜单：动态隐藏与放大项、位置与最小化方式子菜单、Options、全部窗口/隐藏/退出/强制退出、空废纸篓禁用，支持勾选、图标、右侧详情和二段子菜单
- 应用自带 Dock 菜单：Firefox（新标签页、新窗口、隐私窗口、最近访问）、VS Code（新窗口、最近工作区）、Windows 跳转列表最近文档回退、开机启动、在资源管理器/文件管理器中显示
- 应用网格/启动台（默认 7×5，可由 `springboard-columns`/`springboard-rows` 配置）：持久化分页与文件夹；拖拽排列（实时空位、拖到屏幕边缘翻页）、拖到应用上创建文件夹、拖入/拖出文件夹（拖到文件夹上松手时图标缩小飞入文件夹，而不是直接消失）；文件夹为与软件图标同形状的无边框半透明圆角块（3×3 预览，透明度与展开背景一致），展开主体高度自适应、标题在主体上方并可单击重命名，展开动画从原文件夹图标开始；顶部搜索框（输入即筛选、按精确/前缀/词首/包含排序、回车打开第一项、Esc 清空；支持中文 IME、退格/删除、Ctrl+C/X/V 粘贴与文件夹标题编辑）；可点击/拖动的页码点、模糊壁纸、淡入/淡出、空白处关闭、拖拽到 Dock、右键添加/移除
- 文件夹栈（网格、分页、点击打开）；Windows 下展开列表过滤 `desktop.ini`/`Thumbs.db` 等系统元数据文件
- 最小化窗口缩略图；最小化时先把缩略图槽位插入 Dock（左右图标让位），再用 Genie（默认，逐顶点漏斗网格）收进这个槽位；Windows 下关闭系统自带的 DWM 最小化过渡并在退出时恢复，Linux 由合成器/窗口管理器控制
- 设置窗口：图标尺寸、放大、位置、主题、标签、指示点、自动隐藏、预留工作区
- 拖拽排序、拖出 Dock 移除、从应用网格拖入 Dock、文件拖入 Dock
- Inter 字体 + 系统 CJK 回退
- Linux：XDG 路径、`.desktop` 解析、icon theme/SVG、X11 strut、跳过任务栏
- Wayland：无系统工作区预留、无全局鼠标轮询、无窗口枚举的降级路径

## 尚未实现

- Mission Control、Exposé、Spaces、热区
- NSDockTile 风格插件 ABI
- 高 DPI 下的逐像素视觉校准（参数已可调）
- Linux 实机视觉验收（代码与 CI 构建配置已提供）

这些项在 `docs/parity.md` 中逐条列出。

## 目录

```text
src/                 RevDock 源码
  core/              时钟、缓动、日志、路径、文件、几何
  config/            Settings（键名对齐 com.apple.dock）
  model/             DockModel / DockItem
  layout/            Tile 布局与放大
  anim/              动画值
  render/            2D 渲染、圆角材质、文本、图标缓存
  apps/              应用索引与缓存
  platform/          Windows / Linux 平台层
  ui/                Dock、菜单、应用网格、文件夹栈、设置窗口
lib/                 第三方库（见 lib/README.md）
assets/              Inter 字体、自绘资源、i18n
tests/               doctest 单元测试
tools/               依赖拉取/裁剪/校验、开发脚本
docs/                构建、架构、配置、Linux、功能对照
reverse/             逆向分析归档（analysis/out/spec/tools + Ghidra）
samples/             只读样本，不参与构建，不进入发布包
```

## 构建

### Windows（MinGW + Ninja）

需要：

- GCC 13+（推荐 w64devkit，当前验证为 GCC 16.2）
- CMake 3.24+
- Ninja
- Git（仅重新拉取依赖时需要）

```powershell
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
ctest --preset windows-mingw-release --output-on-failure
.\build\windows-mingw-release\bin\revdock.exe
```

### Linux（X11）

Ubuntu/Debian 依赖示例：

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
  libx11-dev libxext-dev libxrandr-dev libxfixes-dev libxi-dev \
  libxcursor-dev libxss-dev libwayland-dev libxkbcommon-dev \
  wayland-protocols libdecor-0-dev
cmake --preset linux-x11-release
cmake --build --preset linux-x11-release
ctest --preset linux-x11-release --output-on-failure
./build/linux-x11-release/bin/revdock
```

没有 X11 开发包时可以用 `-DREVDOCK_ENABLE_X11=OFF` 构建，但原生 strut 支持会关闭。

## 运行与配置

```text
--settings     启动时打开设置窗口
--verbose, -v  输出调试日志
```

首次运行会在配置目录创建 `config.json`，默认包含：

- 应用网格
- 分隔符
- 下载文件夹（存在时）
- 废纸篓

运行中的应用会自动追加到 Dock 中，并在图标下显示运行指示点。

### 数据路径

| 数据 | Windows | Linux |
|---|---|---|
| 配置 | `%APPDATA%\RevDock\config.json` | `$XDG_CONFIG_HOME/revdock/config.json` |
| 缓存 | `%LOCALAPPDATA%\RevDock\cache\` | `$XDG_CACHE_HOME/revdock/` |
| 状态 | `%LOCALAPPDATA%\RevDock\state\` | `$XDG_STATE_HOME/revdock/` |
| 日志 | `%LOCALAPPDATA%\RevDock\logs\revdock.log` | `$XDG_STATE_HOME/revdock/logs/revdock.log` |

- 程序目录存在 `revdock.portable` 时，使用程序目录下 `data/`。
- 可用 `REVDOCK_CONFIG_DIR`、`REVDOCK_CACHE_DIR` 覆盖配置/缓存目录。
- 配置在变更后 0.5 秒防抖写入，退出时强制保存；写入采用临时文件 + 原子替换。
- 应用缓存 24 小时有效，不会每次启动扫描开始菜单。

### 操作

- 左键应用：启动或激活
- 左键文件夹：打开文件夹栈
- 左键应用网格：打开应用列表
- 右键：上下文菜单
- 拖拽图标：排序
- 把文件拖到 Dock：添加文件/文件夹
- 应用网格中右键应用：添加/移除 Dock
- Dock 空白处右键 → `Dock Settings...`，或 `--settings`

## 字体与许可

- 默认字体：**Inter 4.1**，SIL Open Font License 1.1，可商用、可修改、可再分发。
- 中文回退：运行时使用系统字体（Windows 微软雅黑 / Linux Noto Sans CJK），不随项目分发。
- 项目代码：MIT（见 `LICENSE`）。
- 第三方库：见 `THIRD_PARTY_NOTICES.md` 和 `lib/README.md`。

本项目不包含 Apple 的字体、`Aqua.car`、图标或其他专有资源。
Dock 的 Launchpad、Trash、Folder 等系统图块为项目自绘；建议的
`HurleyWong/macOS-Big-Sur-icon-collection` 是 GPL-3.0 且主要是第三方应用替换图标，
没有引入到 MIT 项目中。

## 逆向资料

`reverse/` 保留了之前的分析结果、规格、反编译索引和 Ghidra 工程说明。
`docs/traceability.md` 将实现模块映射到这些资料中的公式和符号。
