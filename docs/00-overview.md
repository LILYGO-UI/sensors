# LILYGO UI 应用模板总览

返回[文档目录](README.md)。

本仓库是一个可独立配置、构建、测试、安装和打包的 LILYGO UI 应用
模板。模板中的计数器是用来展示推荐工程结构的示例，不是可直接发布的
产品。

本文中“必须”表示模板及由模板创建的应用都需要遵守；“按需”表示只在
实际功能需要时才创建，不使用空目录占位。

## 稳定约束

- 应用源码和测试使用 C++17 或更新标准；仅在 AppKit 集成边界保留 C ABI。
- UI 使用 LVGL 9.5，版本和补丁由 LilyGoUI SDK 统一提供，应用不自行引入
  LVGL 副本。
- 桌面预览使用 SDL2；设备默认使用 DRM/KMS。
- 标准测试视口为竖屏 `568x1232` 和横屏 `1232x568`。布局必须依据容器的
  实际几何尺寸响应，不得对外暴露应用展示模式。
- 顶层构建系统是 CMake 3.21 或更新版本，稳定预设名为 `host-simulator` 和
  `cm0-cross`。
- 应用只能通过 `find_package(LilyGoUI CONFIG REQUIRED)` 消费平台 SDK。不得引用
  Launcher 源码、其他应用仓库或 AppKit 私有文件。
- `/usr/share/launcher/apps` 下的 Launcher manifest 只是运行时发现协议，不是
  构建依赖。
- 公开接口不得使用 `cm0-app-*`、`lilygo-cm0-*`、`cc.lilygo.cm0.*` 或
  `org.cm0.*` 等旧命名。
- UI 必须使用 LVGL Flex 或 Grid 组织响应式布局。文本使用
  `<cm0/typography.h>` 中的 `lilygo_ui_font_get()` 统一获取 AppKit 字体。

## 开发环境

开发机必须提供以下工具：

- Git，用于获取仓库和初始化 Git submodule。
- CMake 3.21 或更新版本，并提供 `ctest` 和 `cpack`。
- 支持 C++17 的 C/C++ 编译器和 `make`。
- `pkg-config` 和 SDL2 开发包，用于桌面模拟器。
- 与当前 `lpm.toml` schema 兼容的 LPM，并且 `lpm` 在 `PATH` 中可用。
- 设备构建需要网络以首次下载已锁定且校验过 SHA-256 的 AArch64 BSP。

克隆后必须先初始化 SDK submodule：

```sh
git submodule update --init --recursive
```

`third_party/cm0-appkit` 是为模板开发锁定的 SDK 配置包，并不改变应用的依赖
边界。应用 CMake 不得通过 `add_subdirectory()` 编译 AppKit，应用源码也不得使用
指向该子模块的相对包含路径。

## 配置与元数据

`lpm.toml` 是唯一由开发者维护的项目配置和产品元数据源，包含：

- 应用标识、版本、标题、描述、许可证和作者；
- 图标、预览图、权限、兼容性和 Launcher 排序；
- 主机、交叉构建 preset 和包输出目录；
- 部署主机、用户和端口默认值。

CMake 在配置阶段通过 LPM 读取并校验 `lpm.toml`，然后在构建目录中生成 CMake
变量、应用身份头文件、Launcher manifest、desktop 文件和 AppStream 元数据。
`data/*.in` 只能保留通用模板，不得重复写入专属于应用的值。

公开身份必须满足：

```text
包名和可执行文件  lilygo-ui-<component>
应用 ID            cc.lilygo.ui.<Component>
Desktop/AppStream     cc.lilygo.ui.<Component>
```

`publish.json` 是 LPM 发布流程中的临时派生数据，不是项目配置入口，不得手工编辑或
提交，也不得生成到仓库根目录。项目源码、CMake 和文档不得依赖该临时文件
存在。

## 工作流程

LPM 是开发者的首选入口，CMake presets 是可独立调用的底层稳定接口。

| 目标 | LPM 入口 | CMake 底层入口 | 输出 |
| --- | --- | --- | --- |
| 启动桌面模拟器 | `lpm start` | `cmake --preset host-simulator` 后构建 | `build/host-simulator/` |
| 运行主机测试 | `lpm test` | `ctest --preset host-simulator` | 测试结果和渲染快照 |
| 交叉构建和打包 | `lpm pack` | `cmake --preset cm0-cross` 后运行 CPack | `dist/*.deb` |
| 部署到设备 | `lpm deploy` | 无 | 设备上已安装的 Debian 包 |

不得绕过 presets 依赖 `build/` 内部目录结构，也不得在开发机上进行源码内
构建。

## 目录结构

```text
lilygo-ui-<component>/
├── assets/
│   ├── app-icon.png                 # 必须，128x128 RGBA 应用图标
│   ├── images/                     # 按需，应用专用图片
│   ├── audio/                      # 按需，应用专用音频
│   ├── fonts/                      # 按需，仅放应用专用字体
│   └── previews/                   # 按需，发布预览图，不进入 deb
├── cmake/
│   ├── LpmMetadata.cmake           # 读取并转换 lpm.toml
│   ├── app_identity.h.in           # 应用身份头文件模板
│   └── cm0-bsp-toolchain.cmake     # 设备交叉工具链入口
├── data/                               # Launcher/Desktop/AppStream 通用模板
├── docs/                               # 架构、UI 和产品设计文档
├── src/
│   ├── main.cpp                    # AppKit 运行时入口
│   ├── app.cpp/.hpp                # 应用生命周期和页面装配
│   ├── pages/<page>/               # 页面私有的 Model、ViewModel 和 View
│   ├── domain/                     # 按需，跨页面共享的业务对象和规则
│   └── components/                 # 可复用 LVGL 组件和绑定辅助类
├── tests/                              # 单元、绑定和无头响应式渲染测试
├── third_party/
│   └── cm0-appkit/                 # 已锁定的 SDK Git submodule
├── .clang-format
├── AGENTS.md
├── CMakeLists.txt                      # 顶层构建、安装和 CPack 规则
├── CMakePresets.json                  # 稳定的主机和设备构建入口
├── lpm.toml                           # 唯一开发者维护的项目元数据源
└── README.md                          # 面向使用者的快速开始
```

`images/`、`audio/` 和 `fonts/` 是按需目录，无资源时不必保留空目录。应用
不得复制或再打包 AppKit 提供的 Inter、Source Han Sans CN 或 Font Awesome 完整
字体。`fonts/` 只用于 AppKit 运行时字体不包含的应用专用字体。

## 源码边界

- 每个页面位于独立的 `src/pages/<page>/` 目录，页面私有的 Model、ViewModel
  和 View 放在一起。
- Model 保存业务状态和领域规则，不得依赖 LVGL。只有当业务对象被多个页面
  实际共享时，才将它提升到 `src/domain/`。
- ViewModel 将 Model 转换为可观察的展示状态并提供用户命令；不得持有具体页面
  widget 的所有权。
- View 只负责 LVGL widget 创建、布局、事件转发和状态绑定；不得直接修改
  Model。
- 全局导航状态由 `app.cpp` 或按需
  新增的 `app_router.cpp/.hpp` 管理，不得由页面 View 持有。
- `components/` 只放置至少被两个页面使用、或明确承担公共绑定和资源生命周期的
  组件。

数据流向固定为：

```text
用户输入 -> View 事件 -> ViewModel 命令 -> Model 更新
         -> 可观察状态 -> View 绑定 -> LVGL 渲染
```

## 生成物与提交边界

以下目录是本地状态或构建产物，不属于源码目录，不得提交：

```text
.cache/    # 已校验的 BSP 等下载缓存
.lpm/      # LPM 运行状态和报告
.venv/     # 本地 Python 环境
build/     # CMake 构建树和测试渲染产物
dist/      # Debian 包及打包临时产物
```

项目不预设 `packaging/` 目录。Debian 安装和打包规则应优先保持在顶层
`CMakeLists.txt` 和 `data/` 模板中；只有出现 CPack 无法表达的真实打包文件时，
才按需创建该目录。

## 验收要求

每次提交至少需要完成主机配置、构建和测试：

```sh
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --preset host-simulator
```

修改 UI 时必须检查竖屏和横屏渲染结果，确认文本无缺字、重叠和裁切，主要
操作始终可见且可达。修改安装、SDK、工具链、元数据或打包逻辑时，还必须完成
`cm0-cross` 构建和 Debian 打包验证。

新应用不得声明 Debian `Provides`、`Conflicts` 或 `Replaces`；只有在迁移真实已发布
软件包时才能添加这些关系。
