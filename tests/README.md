# tests — 自动化测试（ADR-017，Phase 2 起常备）

把格式识别、边界输入、画布布局这类最容易回归的逻辑固化成断言，绕过应用外壳直接测
`src/document` 与 `src/canvas` 模块。人工验证清单（`docs/04-人工验证.md`）只保留交互/渲染类项目。

## 用法

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1          # 常规：生成样本 → 编译 → 运行 doc_test(72) + canvas_test(126) + page_cache_test(32) + page_state_test(54) + reader_state_test(110) + persist_test(7) + tone_test(113) + page_map_test(26) + text_hit_test(14) + render_search_test(10) + render_fault_test(7) + imgui_raii_test(8) + 菜单文案宽度 + 主题重置 / 优先级 + 资源断言（字体子集 / 图标帧集）
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1 -Probe   # 额外跑 MuPDF 诊断探针（打印 FZ_META_FORMAT 等）
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1 -NoRegenerate  # 复用已有 samples/
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1 -Asan -NoRegenerate   # ASan 插桩回归（较慢）
```

- 退出码 = 任一测试失败即非零；已接入 CI（`.github/workflows/pr.yml`，PR 与推 `main` 触发，Debug/Release 矩阵 + 安装包门禁，见 ADR-076/084）。
- `tests/samples/`、`tests/_build/` 为生成物，已 gitignore。

### `-Asan`：AddressSanitizer 插桩回归

同一套用例改用 `/fsanitize=address` 编译并运行，抓**真实发生**的内存错误——堆/栈缓冲区越界、
释放后使用、重复释放——报告精确到"文件:行号"。它不替代常规回归，而是发布前（或升级 MuPDF、
大改渲染/持久化之后）的加严复核。

三个必须知道的边界：

1. **不支持泄漏检测**：Windows 平台的 MSVC ASan 没有 LeakSanitizer，`detect_leaks` 会直接报
   `not supported on this platform`。**堆泄漏不在覆盖范围内**。
2. **第三方静态库未被插桩**：MuPDF / FreeType 等以预编译静态库链接，其**内部**的内存错误查不到；
   但 ASan 全局接管分配器，故"MuPDF 分配、本项目代码越界访问/释放后使用"这类**跨边界**错误仍会被捕获。
3. **运行时是动态 DLL**：即便 `/MT` 静态 CRT，ASan 运行时也是 `clang_rt.asan_dynamic-x86_64.dll`。
   脚本会自动把工具集的 `bin\Hostx64\x64`（该 DLL 所在目录）前置到 `PATH`，手工运行时需自己处理。

插桩会关掉 `/O2`（改 `/Od` 并加 `/Zi`）并给链接器补 `/DEBUG`，以保证报告里的"文件:行号"
与调用栈符号准确（缺 `/DEBUG` 时链接器报 LNK4302），故整体比常规回归慢。

## 文件

| 文件 | 作用 |
|---|---|
| `make_samples.py` | 合成 31 个常规样本（合法/改名/归档冒充/损坏，含**异构页尺寸** `mixed_size.pdf`、**带两级目录** `outline.pdf`、**文字+红图** `with_image.pdf`、**整页一张图** `scan_only.pdf`、**一块纯黑** `ink_black.pdf`）+ 3 个加密 PDF（需 PyMuPDF：`pip install pymupdf`，未装则跳过加密例） |
| `doc_test.cpp` | 72 例断言表：合法格式、改名放行、归档冒充拒绝、加密三态、扩展名闸门、**逐页尺寸**（形变防线）、**目录解析**（顺序/层级/页号）、**旋转渲染**（0°/180° 尺寸不变、90° 宽高互换）、**纸张方案配色**（深色纸张背景为深暖灰且保留 alpha、暖色 R>B）、**配色分层**（ADR-067：`with_image.pdf` 深色纸张下照片仍是红的、纸面仍变深；`scan_only.pdf` 整页扫描件整体变深；`ink_black.pdf` 正文（纯黑）映射为浅暖灰、**并与纸面暖度一致**——LUT 两端等斜率）、**文本层**（Phase 8，ADR-069/070：`page_content` 抽到字符与行、码点拼回词、`copy_text` 的**选区端点约定**（字符框中心朝外偏 30%，用中心点会少一个字）、`search_page` 命中 / 大小写不敏感 / 无匹配为空表、**自洽检索**（把抽到的**同一行**前 4 个字符拼成关键字再搜自己 —— PDF/EPUB/FB2 通用，不依赖样本文案） |
| `canvas_test.cpp` | 126 例断言：fit-width 派生、固定缩放居中、内容尺寸、滚动钳制、以鼠标为锚的缩放定点不变性、命中测试、可见范围、列切换锚定、非均匀页尺寸、缩放钳制、空文档、**翻页游标**（矮页视口不卡住 / 多列按行推进 / 与手动滚动同步）、**页间距随缩放**（ADR-029）、**双页对开**（ADR-038：封面单独居中 / 对开分列 / 按行推进 / `spread=false` 回归守卫 / 切换锚定 / **对开与列数同层级互斥**：切列退出对开、`columns` 值保留） |
| `page_cache_test.cpp` | 32 例断言：预算钳制（0/下限/上限/SIZE_MAX）、自动重试上限与 `should_render_failed`、**LRU 逐出**（最久未用先出、pinned 保护、同序号按下标定序、恰好达标即停、未驻留页跳过、按字节累计）（Phase 4，ADR-030/031） |
| `page_state_test.cpp` | 54 例断言（ADR-085，含 `src/render/page_state.ixx`）：**单页渲染状态机的纯逻辑**。`Unloaded→Loading→Loaded` 基本环、`stale`（配色失效保留纹理、重渲清 stale）、自动重试额度消耗、**失败定格**（同倍率不再重渲 / 换倍率视为新请求并重置额度 / 容差 0.002）、手动重试（一次性放行并复位额度）、逐出与旋转作废三条复位路径、`PageStatus` 取值稳定（UI/快照依赖数值）。零 D3D/线程依赖 |
| `reader_state_test.cpp` | 110 例断言（Phase 5，ADR-034；ADR-062/065 增身份分层与位置列表）：序列化往返（含书签/确定性）、容器语义（find/upsert/erase/rekey）、**坏输入一律安全拒绝**（nullptr/magic/版本不配对/路径超长/位置条数或长度超限/超限/截断）、字段钳制（列/旋转/配色/非法 zoom）、文档键（不存在→0、同路径稳定）、**v3 身份与位置往返**（path_key/page_count/多条位置及其顺序/last_location）、**v1 迁移**（旧 key → path_key，位置留空）、**v2 迁移**（单路径 → 位置列表，空路径不进表）、**分层定位**（指纹命中/relocated/回到记过的位置不再询问/关掉智能匹配/页数不符不继承/adopt 改挂主键并累积位置/primary_key/rekey）、**位置记忆**（去重、最近优先、封顶丢最旧）、**内容指纹**（同一文件稳定、改中段或尾段一字节即变、复制到新路径不变） |
| `persist_test.cpp` | 7 例断言（ADR-082）：**异步持久化服务的端到端测试**（跨线程，纯函数单测覆盖不到）。真跑工作线程，用轮询把"什么时候落盘"变成可断言的现象。断言：`write_state_bytes`/`load_state` 字节往返、防抖窗口后自动落盘（dirty 转清）、`flush` 越过长防抖窗口立即落盘、写失败（目录不存在）如实上报、**析构 flush**、空状态落盘、连续多次 `request_save` 合并为最后一次内容 |
| `mupdf_probe.cpp` | 诊断工具：打印 MuPDF 对每个样本的原始判定（页数、`FZ_META_FORMAT`），新增格式支持时先用它摸底 |
| `tone_test.cpp` | 113 例断言（ADR-068，含 `src/app/tone.h`）：**原色零改动**（逐通道相等）、**黄金值**（深色纸张/暖色各 13 个角色色的最终 RGB，改参数即红）、**对比度下限**（正文/次要文字/强调色/控件层次的 WCAG 对比度）、**结构不变量**（换色不改变中性族明暗次序 / 强调族不再偏冷 / 语义色仍是红橙且可区分 / 中性族保亮度 = 原值 × level） |
| `render_search_test.cpp` | 10 例断言（ADR-069/070/071/075）：**渲染层检索通道的端到端测试**。检索是跨线程的（UI 投递 → 工作线程增量扫描 → UI 取结果），纯函数单测覆盖不到；用例真跑整条链路：`open` → 等 Ready → `start_search` → 轮询 `take_search_hits`。断言覆盖页内容、命中、跨页扫描、命中矩形坐标、收敛状态、无匹配、换词不混入旧结果、取消立即收敛、空关键字不启动。**设备传 `nullptr`**：用例只走文本通道、不建纹理。 |
| `render_fault_test.cpp` | 7 例断言（Phase 7，ADR-079）：**渲染工作线程的故障隔离**——必须在真实线程边界上跑，单测覆盖不到 `std::jthread` 入口。断言：5 种坏文件（不存在/截断/随机字节/空 PDF/坏 zip 容器）全部**文档级** Failed 且错误详情非空、失败后**仍能**正常打开下一个文档（无 `std::terminate` 的直接证据）、越界页码/请求（slot/retry/wants/thumbs/copy）安全退化且不污染文档状态、25 轮好坏交替 open/close 无死锁无累积、400 轮请求洪峰 + 检索起停后仍收敛、1200 页文档可打开可检索、A0 幅面（2384×3370pt，走 tile 路径）收敛后调度器仍健康。设备传 `nullptr`：建纹理必然失败，恰好压到"整页分块渲染 → 逐 tile 失败 → 自动重试 → 清理"路径 |
| `imgui_raii_test.cpp` | 8 例断言（Phase 7，ADR-078）：**ImGui RAII 包装与栈平衡自检的 headless 测试**。只编 ImGui 核心（无 Win32/D3D 后端），NewFrame → 绘制 → Render 无后端同样成立，故栈平衡可完全离线、确定性验证。断言：Push/Pop 全家族配对、Begin/End 条件配对（含返回 false 分支）、**异常展开收回全部栈**（抛出点无条件执行，不依赖任何 ImGui 返回值）、`dismiss()` 提前收口且幂等、`format_stack_diff` 能检出注入的漏配对、200 帧连续绘制不累积泄漏、帧首基线跨帧稳定（判据是"帧尾 == 帧首"而非"各层为 0"——ImGui 的隐式窗口与默认字体使后者恒假）。另把 ImGui 自身检出的错误接到计数回调：**报错即失败**，比"崩没崩"严格 |
| `page_map_test.cpp` | 26 例断言（ADR-069，含 `src/app/page_map.h`）：屏幕点 ↔ **未旋转页面 pt** 的折算。四类：**四角对应**（90°/180°/270° 下未旋转页面的四个角各落到屏幕矩形的哪个角 —— 这是最容易写反的一处）、**正逆往返一致**（含页外点，拖动选择依赖它）、**退化输入返回 false**（页尺寸/页矩形为 0，不产生 NaN）、**角度归一化**（负数 / 超 360 / 非 90 倍数）。文本选择、复制、超链接命中、搜索高亮全部建立在这组折算之上，而它的错误是"0° 正常、一旋转就整体偏一格"这类肉眼难判的一类 |
| `text_hit_test.cpp` | 14 例断言（ADR-091，含 `src/app/text_hit.h`）：文本命中测试的**选行**判据。核心是**多列版面**——左右两列的正文行 y 带完全重叠、左列在 stext 顺序里靠前，旧实现"取第一个 y 命中就收"会把右列的点判给左列（实测两列论文右列几乎选不中）。断言覆盖：单列（每 y 至多一行，回归守卫）、多列（按 x 破平取到正确列）、带外（`in_band=0`）、拖拽（横向远离仍返回该行）、竖排行高取宽度。零依赖 |
| `check_theme_reset.py` | 主题重置断言（ADR-068）：`apply_theme_colors` 的两个分支必须先 `StyleColorsLight` / `StyleColorsDark` **整套重置**再逐项覆盖 —— 否则未覆盖的颜色项会带着上一个主题的值活过来（深色勾选框曾因此变成亮奶油色）。跨状态残留这类 bug 在代码里毫无痕迹，只能靠结构性约束挡住 |
| `check_theme_precedence.py` | 外观契约断言：深色纸张只在“跟随系统”时额外令界面变暗，显式浅色/深色不被文档状态覆盖；同时检查菜单分组、统一标签表与设置项顺序 |
| `check_fz_boundary.py` | document 层 fz_* 边界纪律（ADR-083）：断言四个边界函数（`pixmap_attrs` / `drop_pixmap_safe` / `drop_stext_safe` / `drop_document_safe`）存在且包了 `fz_try`、`PageBitmap::reset` 销毁走 `drop_pixmap_safe`、stext/document 拆解经安全包装、`PageBitmap` 的 `width/height/stride/samples` 为零 `fz_*` 的纯访问器。**范围刻意收窄**：整篇 `document.cpp` 并不满足"所有 fz_* 都在 fz_try 内"（设备回调转发、pixmap 读取、fz_caught_message 包装都在其外），故只断言 ADR-083 真正建立的那几条结构事实 |
| `check_menu_width.py` | 菜单文案宽度断言（ADR-067）：扫描 `src/app/ui.cpp` 的菜单字面量，显示宽度（CJK=2 列）不得超过 12 列 —— 弹出菜单的宽度由最长项决定，一条超长文案会把整张菜单撑宽。补充说明应改用悬停提示 |
| `check_installer_size.py` | 安装包体积门禁（ADR-084）：断言 `installer/dist/` 的安装包 ≤ 35 MiB（依据 docs/05 §5）。CI 的 installer job 与本地均可跑：`python tests/check_installer_size.py installer\dist` |
| `../assets/check_icons.py` | 图标资源断言（Phase 7，ADR-051）：每个 `.ico` 的帧集必须完整（17 帧），且每帧位图的**真实解码尺寸**必须等于目录项声明的尺寸 —— 防"小图塞进大槽位 / 只剩一帧"这类只在特定 DPI 下暴露的错 |
| `run_tests.ps1` | 一键编译 + 运行；`cl.exe` 直调，链接配置独立于 `build.bat` 的 vcpkg，MuPDF 升级时需同步其库列表 |

## 注意

- 脚本/测试均按 UTF-8 输出；控制台若乱码，先 `chcp 65001`。
- 扩展名闸门与"扩展名 → 格式族"判断的**唯一出处**是 `utils.ixx` 的 `kExtTable`（ADR-093）；
  `doc_test.cpp` 的闸门 / 归档冒充断言需与它保持一致。`document.cpp` 现在 `import lilithreader.utils;`，
  故编译 `document.cpp` 时要 `/reference utils.ifc`（编译顺序：`utils.ixx` → `document.cpp`）。
- 链接 `canvas_test.exe` 时**必须一并链接 `canvas.ixx.obj`**：画布接口单元里的内联
  成员（如 `Canvas::state`）由它发射，只链 `canvas.obj` 会报 LNK2019。
- 同理 `page_cache_test.exe` 必须链 `page_cache.ixx.obj`：`select_evictions` 定义在接口单元里。
- `page_state_test.exe` 必须链 `page_state.ixx.obj` + `page_cache.ixx.obj`：转移函数与
  `PageState` 内联由接口单元发射，且它 import `page_cache` 的重试策略常量。编译 `page_state.ixx`
  要 `/reference page_cache.ifc`；编译顺序 `page_cache` → `page_state`（ADR-085）。
- `reader_state_test.exe` 必须链 `reader_state.ixx.obj` + `reader_state.obj`：接口单元发射
  `ReaderState` 的成员函数（`find`/`upsert`/`erase`），实现单元提供 `encode_state`/`document_key` 等。
  用例含内容指纹断言，故编译时还需 `/reference utils.ifc`（`file_fingerprint` 是内联函数，无需额外链 obj）。
- `render_search_test.exe` 必须链 `render.ixx.obj` + `render.obj` + `document.ixx.obj` + `document.obj`
  + `page_cache.ixx.obj` + `page_state.ixx.obj` + `utils.ixx.obj`，并额外加 `d3d11.lib`（`render.cpp`
  里建纹理用；本用例不实际建，但符号必须能解析）。编译 `render.ixx` 时要 `/reference` 它 import 的
  `document` / `page_cache` / `page_state` / `utils` 四个 `.ifc`（ADR-085：`PageStatus` 迁入 `page_state`）。
- `persist_test.exe` 必须链 `persist.ixx.obj` + `persist.obj` + `reader_state.ixx.obj` +
  `reader_state.obj` + `log.obj` + `log.ixx.obj`。编译 `persist.ixx` 要 `/reference reader_state.ifc`；
  编译 `persist.cpp` 要 `/reference persist.ifc` + `reader_state.ifc` + `log.ifc`。
  编译顺序：`reader_state`/`log` → `persist`（见 ADR-082）。
- Phase 7 起 `render.cpp` import 了 `lilithreader.log`，故编译 `render.obj` 时需 `/reference log.ifc`，
  链接时需加 `log.obj` + `log.ixx.obj`（`render_fault_test.exe` 同）。
  编译顺序：`log.ixx` 必须先于 `render.cpp`。
- `imgui_raii_test.exe` 只编 ImGui **核心**四个 TU（imgui/draw/tables/widgets，放 `_build/raii/`
  子目录避免与其它用例重名），**不**编任何后端；头文件搜索路径需 `/I<repo>\src\app`
  （`imgui_raii.h` / `imgui_stacks.h`）。目标文件多源编译时每个源要单独 `/Fo`——
  MSVC 不允许多个源文件共用一个 `/Fo<文件>`。
- `page_map_test.cpp` / `tone_test.cpp` 只依赖 C++ 标准库，但都要 `/I<repo>\src\app` 才能找到头；
  两者都是"把 app 层里可判定的纯函数抽出来单测"（ADR-068/069），不需要额外的 .obj。
  用例里**不要**写 `for (int r : {0,90,180,270})`：range-for 初始化列表需要 `<initializer_list>`，
  这两个测试刻意只依赖 `<cmath>`/`<cstdio>`，用显式数组即可。
- `minimal_pdf` 的对象编号约定是 `Page = 5+2i`、`Contents = 6+2i`（`Kids` 必须用前者）。
  第三轮调试前这里写成 `4+2i`，生成的是**非法 PDF**（靠 MuPDF 修复才打开、各页被修成
  默认 Letter 尺寸），曾掩盖"异构页尺寸形变"这一缺陷 —— 改动该函数时务必保持自洽。
