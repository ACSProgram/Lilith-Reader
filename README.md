# Lilith Reader

从 [Lilith](F:\programs\Lilith) 主项目分离出来的轻量级文档阅读器。目标：**单 exe、零外部依赖、极致效率、体面美观、长期稳定**。

## 目标特性

- 单文件 exe（/MT 静态链接，MuPDF 静态库，无 DLL/运行时依赖），目标体积 < 30MB
- `reader.exe <文件路径>` 直接打开，可注册"打开方式"双击关联
- 支持格式：PDF、epub、mobi、fb2、cbz、xps（MuPDF 原生多格式）
- 自研画布：连续滚动阅读 / 1~4 列网格并列 / 以鼠标为中心的平滑缩放
- 按缩放级别按需重渲染（低清占位 + 高清换入），放大不模糊
- 记住每本书的阅读位置；书签、目录（PDF outline）
- UI 字体子集化嵌入 exe 内（常用 3500 汉字 + fallback 系统字体），运行时零文件依赖

## 技术栈

| 层 | 选型 |
|---|---|
| UI | ImGui（源码内联）+ imgui_freetype，D3D11 后端 |
| 画布 | 自研（ImDrawList + 自管缩放/平移/网格布局） |
| 文档解析 | MuPDF（静态链接，每线程 clone context） |
| 语言 | C++20 / MSVC，C++20 modules（.ixx）组织 |
| 包管理 | vcpkg（manifest 模式，`x64-windows-static` triplet） |

## 目录结构

```
├── docs/                 设计与计划文档
├── src/
│   ├── app/              入口、Win32 窗口、D3D11/ImGui 初始化、命令行参数
│   ├── document/         MuPDF 封装（Document/页面渲染/线程模型）
│   ├── canvas/           自研画布（缩放、平移、网格、滚动、渲染请求调度）
│   ├── render/           纹理管理、LRU 页缓存、后台线程池
│   └── utils/            通用工具（字符串、路径、日志）
├── third_party/imgui/    ImGui 源码（从 Lilith 复制后随仓库提交）
├── assets/               图标、字体子集、资源脚本
└── vcpkg.json
```

## 构建（当前状态：Phase 1 应用外壳已完成 ✅）

- Visual Studio 18 (2026) Community，PlatformToolset v145（MSVC 14.51）
- vcpkg，manifest 模式，triplet `x64-windows-static`
- MSVC `/MT`（静态 CRT）+ `/O2` + `/utf-8`
- 构建：VS 打开 `LilithReader.slnx` 直接 F5，或命令行运行 `build.bat`
- 产出：`bin\Release\LilithReader.exe`（Phase 0 约 2.3MB，静态 CRT，仅依赖系统组件 DLL）
- 当前程序：应用外壳——拖放/命令行打开文档、格式识别占位页、窗口状态持久化、F3 调试浮层（UI 字体暂用系统微软雅黑，Phase 6 换内嵌子集字体）

## 状态

| 阶段 | 状态 |
|---|---|
| Phase 0 骨架与环境 | ✅ 完成（2026-10-05） |
| Phase 1 应用外壳 | ✅ 完成（2026-10-05，[人工验证](docs/04-人工验证.md) §1 待人工执行） |
| Phase 2 MuPDF 文档核心 | 未开始 |
| Phase 3 自研画布 | 未开始 |

## 纪律

开发过程中**不实际运行软件做测试**（编译与自动化测试除外）；需人工运行确认的项统一记录在 [人工验证](docs/04-人工验证.md)。

详见 [迁移计划](docs/01-迁移计划.md)。

## 文档

- [迁移计划](docs/01-迁移计划.md) —— 分阶段执行的主计划
- [架构设计](docs/02-架构设计.md) —— 画布、渲染管线、字体、稳定性设计
- [决策记录](docs/03-决策记录.md) —— 关键选型及理由（ADR）
- [人工验证](docs/04-人工验证.md) —— 需人工运行确认的验证清单
