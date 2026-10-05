# Lilith Reader

从 [Lilith](F:\programs\Lilith) 主项目分离出来的轻量级文档阅读器。目标：**单 exe、零外部依赖、极致效率、体面美观、长期稳定**。

## 目标特性

- 单文件 exe（/MT 静态链接，MuPDF 静态库，无 DLL/运行时依赖）；当前 40.5MB，其中约 34MB 是
  MuPDF 内置的 CJK 兜底字体（思源宋体）——中文文档未内嵌字体时的必需资源，裁剪计划见迁移计划 §4
- `reader.exe <文件路径>` 直接打开，可注册"打开方式"双击关联
- 支持格式：PDF、epub、mobi、fb2、cbz、xps 及常见图片（png/jpg/gif/bmp/tif，
  按单页文档打开）；识别策略为**内容优先**，见 [决策记录 ADR-016/017](docs/03-决策记录.md)
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
│   ├── canvas/           自研画布（纯布局数学：缩放、平移、网格、滚动、命中测试）
│   ├── render/           渲染调度：工作线程、页状态机、纹理上传与退役队列
│   └── utils/            通用工具（字符串、路径、日志）
├── third_party/imgui/    ImGui 源码（从 Lilith 复制后随仓库提交）
├── tests/                自动化测试（样本合成 + document/canvas 模块断言，`run_tests.ps1` 一键运行）
├── assets/               图标、字体子集、资源脚本
└── vcpkg.json
```

## 构建（当前状态：Phase 3 自研画布已完成 ✅）

- Visual Studio 18 (2026) Community，PlatformToolset v145（MSVC 14.51）
- vcpkg，manifest 模式，triplet `x64-windows-static`；依赖 `libmupdf` 1.26.10
- MSVC `/MT`（静态 CRT）+ `/O2` + `/utf-8`
- 构建：VS 打开 `LilithReader.slnx` 直接 F5，或命令行运行 `build.bat`
- 产出：`bin\Release\LilithReader.exe`（40.5MB，静态 CRT，仅依赖系统组件 DLL）

### 依赖安装位置（重要）

vcpkg 的依赖**不在项目目录内**，而在：

```
F:\programs\.Environment\vcpkg-roots\LilithReader\x64-windows-static\
```

原因见 [ADR-010](docs/03-决策记录.md)：项目路径 `F:\programs\Lilith Reader` 含空格，
而 meson 端口（harfbuzz）生成的链接命令行未给 `/libpath:` 加引号，路径会被截断成两个参数
（LNK1181）；同时 vcpkg 的 MSBuild 集成会把 triplet 拼两次。把安装根设到无空格且不含
triplet 的独立目录可一并消除，故在 `src/LilithReader.vcxproj` 中显式设置
`<VcpkgInstalledDir>`。**首次克隆后直接 `build.bat` 即可**（vcpkg 会自动装依赖）。

`overlay-ports/gumbo/` 是修正上游归档哈希的覆盖端口（[ADR-011](docs/03-决策记录.md)）；
项目里若残留旧的 `vcpkg_installed/` 目录，已不再使用，可安全删除。

- 当前程序：应用外壳 + 文档核心 + **自研画布**——拖放/命令行打开文档、后台线程异步打开、
  真实元信息（页数/首页尺寸/标题/目录/加密状态）、结构化错误分类、窗口状态持久化；
  **阅读态**支持连续滚动、以鼠标为中心的平滑缩放（防抖后高清重渲染）、1~4 列网格、
  键盘翻页/跳页/全屏，底部状态栏显示页码/缩放/列数；F3 调试浮层
  （UI 字体暂用系统微软雅黑，Phase 6 换内嵌子集字体）

## 状态

| 阶段 | 状态 |
|---|---|
| Phase 0 骨架与环境 | ✅ 完成（2026-10-05） |
| Phase 1 应用外壳 | ✅ 完成并人工验证通过（2026-10-05） |
| Phase 2 MuPDF 文档核心 | ✅ 完成，编译已验证；运行期验收待人工执行（2026-10-05） |
| Phase 3 自研画布 | ✅ 完成，编译 + 61 例画布断言通过；运行期验收待人工执行（2026-10-05） |
| Phase 4 渲染调度与纹理管理 | 未开始（下一阶段） |

## 已知问题

- **格式识别是"内容优先"的**：只要 MuPDF 能认出内容是什么，就照认出的格式打开，扩展名
  不实只作提示（如 epub 改名 `.pdf` 仍按 epub 阅读，界面标注"扩展名不符"）。唯一例外是
  **压缩包冒充单文档**（zip 图集改名 `.epub`/`.pdf`）会被拒绝并提示"这是压缩包，不是单个
  文档"——归档类会被处理器按图片条目凭空编出页数，必须拦住。依据与实测见
  [决策记录](docs/03-决策记录.md) 的 ADR-014 ~ ADR-016。
- **Windows Defender 误报**：未签名 + 静态 CRT 的小体积 exe 容易被启发式引擎误判。缓解与根治方案：
  1. 本机：把项目 `bin\` 目录加入 Defender 排除项，或从"保护历史"还原被隔离的 exe
  2. 上报误报：https://www.microsoft.com/en-us/wdsi/filesubmission （通常 24~72h 内更新病毒定义）
  3. 中期：Phase 7 加入版本资源与图标（无版本信息的空壳 exe 更易被命中）
  4. 根治：购买代码签名证书对发布版签名（自签名无效；EV 证书可获得即时信誉）

## 纪律

- 开发过程中**不实际运行软件做测试**（编译与自动化测试除外）；需人工运行确认的项统一记录在 [人工验证](docs/04-人工验证.md)。
- **每个 Phase 收尾必须立即 git 提交**，不得让阶段改动与后续修改混在工作区。
- 格式/边界类验证优先做成自动化用例进 `tests/`，人工清单只留交互与渲染类项目。

详见 [迁移计划](docs/01-迁移计划.md)。

## 文档

- [迁移计划](docs/01-迁移计划.md) —— 分阶段执行的主计划（只含当前状态与未来计划）
- [架构设计](docs/02-架构设计.md) —— 画布、渲染管线、字体、稳定性设计
- [决策记录](docs/03-决策记录.md) —— 关键选型及理由（ADR）
- [人工验证](docs/04-人工验证.md) —— 待执行的人工验证清单
- [历史归档](docs/history/) —— 已完成阶段的实施细节、事故与验证记录
