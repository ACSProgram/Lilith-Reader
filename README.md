# Lilith Reader

从 [Lilith](F:\programs\Lilith) 主项目分离出来的轻量级文档阅读器。目标：**单 exe、零外部依赖、极致效率、体面美观、长期稳定**。

## 目标特性

- 单文件 exe（/MT 静态链接，MuPDF 静态库，无 DLL/运行时依赖）；当前 **40.5MB**，其中约 34MB 是
  MuPDF 内置的 CJK 兜底字体（思源宋体）——中文文档未内嵌字体时的必需资源，构成与裁剪手段见
  [体积预算](docs/05-体积预算.md)
- `LilithReader.exe <文件路径>` 直接打开，可注册"打开方式"双击关联
- 支持格式：PDF、EPUB、MOBI、FB2、CBZ、XPS 及常见图片（png/jpg/jpeg/gif/bmp/tif/tiff，
  按单页文档打开；`.webp` 无 MuPDF 解码器，不支持）；识别策略为**内容优先**（扩展名不符时
  如实标注，只拦"压缩包冒充单文档"），见 [决策记录 ADR-016/017](docs/03-决策记录.md)
- 自研画布：连续滚动阅读 / 1~4 列网格并列 / 以鼠标为中心的平滑缩放 / **逐页真实尺寸布局**
  （异构 PDF 不形变）
- 按缩放级别按需重渲染（低清占位 + 高清换入，重渲染期间保留旧纹理不闪烁），放大不模糊
- 页纹理缓存按**字节预算**（默认 512MiB，`LilithReader.ini` 的 `[cache] BudgetMB` 可调
  128~2048）；渲染失败页自动重试 1 次，仍失败则显示占位并可**点击重试**
- 界面随显示器 DPI 缩放；键盘快捷键为应用级语义、不依赖窗口焦点、不受输入法干扰
- **阅读功能**：记住每本书的阅读位置与书签（exe 同目录 `reader_state.bin`，原子写入）；
  目录（PDF outline）侧栏 + 跳转；页面缩略图侧栏；旋转（0/90/180/270）；双页对开（书籍模式）；
  反色 / 护眼配色；加密 PDF 密码输入框（支持 AES-128/256 等标准加密），权限位读取
- **界面**：顶栏图标按钮 + 主菜单 + 画布右键菜单（`Shift+F10`/菜单键为等价入口），
  命令集中在菜单里、不往工具栏堆按钮；浅色/深色/跟随系统三主题；**设置界面**
  （界面缩放/主题/顶栏自动隐藏/动效/页面间距/缓存预算，改动即时生效并落盘）与
  **「快捷键与帮助」界面**；可选的顶栏自动隐藏、页面淡入等微动效；自带应用图标。
  实现要点见 [架构设计 §7](docs/02-架构设计.md)
- 快捷键：`O` 侧栏、`B` 书签、`R` 旋转、`D` 双页对开、`I` 反色、`E` 护眼；
  全部命令与按键见程序内置的「快捷键与帮助」（`F1`）或
  [架构设计 §2.2](docs/02-架构设计.md)

## 技术栈

| 层 | 选型 |
|---|---|
| UI | ImGui（源码内联，1.93）+ imgui_freetype，D3D11 后端 |
| 画布 | 自研（ImDrawList + 自管缩放/平移/网格布局，布局纯函数、可单测） |
| 文档解析 | MuPDF（静态链接 1.26.10，每线程独立 context，UI 线程零 fz_*） |
| 语言 | C++20/23 / MSVC，C++20 modules（.ixx）组织 |
| 包管理 | vcpkg（manifest 模式，`x64-windows-static` triplet） |

## 目录结构

```
├── AGENTS.md             AI 助手工作规约（文档写入纪律、项目纪律——先读这个）
├── docs/                 项目文档（导航见 docs/README.md）
├── src/
│   ├── app/              入口、Win32 窗口、D3D11/ImGui 初始化、命令行参数
│   ├── document/         MuPDF 封装（Document/页面渲染/线程模型）
│   ├── canvas/           自研画布（纯布局数学：缩放、平移、网格、滚动、命中测试）
│   ├── render/           渲染调度：工作线程、页状态机、字节预算 LRU、纹理上传与两段式退役队列、缩略图
│   ├── state/            阅读状态持久化（reader_state.bin：阅读位置、书签；纯序列化 + 原子写入）
│   └── utils/            通用工具（路径、编码、扩展名闸门、ini 持久化）
├── third_party/imgui/    ImGui 源码（从 Lilith 复制后随仓库提交）
├── tests/                自动化测试（样本合成 + document/canvas 模块断言，`run_tests.ps1` 一键运行）
├── assets/               图标、字体子集、资源脚本
└── vcpkg.json
```

## 构建

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
旧 `vcpkg_installed/` 已删除（一直在 `.gitignore` 里，不影响构建）。

## 状态

| 阶段 | 状态 |
|---|---|
| Phase 0 骨架与环境 | ✅ 完成（2026-10-05） |
| Phase 1 应用外壳 | ✅ 完成并人工验证通过（2026-10-05） |
| Phase 2 MuPDF 文档核心 | ✅ 完成并人工验证通过（2026-10-05） |
| Phase 3 自研画布 | ✅ 完成并人工验证通过（含第三、四轮调试修复，2026-10-05） |
| Phase 4 渲染调度与纹理管理 | 实现完成（2026-10-05），运行期验收待人工执行 |
| Phase 5 阅读功能 | 实现完成（2026-10-05），运行期验收待人工执行（与 Phase 4 合并验收） |
| Phase 6 美化 | 界面外壳/图标/主题/设置/帮助/微动效已落地（2026-10-05）；字体子集化与缩放插值未做；主观评审待人工执行 |

各阶段实施细节、调试修复过程与验证记录见 [docs/archive/](docs/archive/README.md)（按 [归档索引](docs/archive/README.md) 查）。

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

详见 [迁移计划](docs/01-迁移计划.md) 与 [AGENTS.md](AGENTS.md)。

## 文档

- [文档导航](docs/README.md) —— **入口**：按任务选择上下文 + 写入规则
- [迁移计划](docs/01-迁移计划.md) —— 分阶段执行的主计划（只含当前状态与未来计划）
- [架构设计](docs/02-架构设计.md) —— 当前实现的边界、线程模型与设计要点
- [决策记录](docs/03-决策记录.md) —— 关键选型及理由（ADR，追加式台账）
- [人工验证](docs/04-人工验证.md) —— 待执行的人工验证清单
- [体积预算](docs/05-体积预算.md) —— exe 体积构成与 Phase 7 裁剪手段
- [归档索引](docs/archive/README.md) —— 已完成阶段的实施细节、事故与验证记录
