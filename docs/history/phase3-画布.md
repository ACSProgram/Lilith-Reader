# Phase 3：自研画布（已完成 2026-10-05）

> 本文件归档已完成阶段的实施细节、事故与实测数据。
> 待人工执行的验证项见 [../04-人工验证.md](../04-人工验证.md)。

## 1. 目标与范围

按 [迁移计划](../01-迁移计划.md) Phase 3：用纯 ImGui + ImDrawList 自研画布，取代
ImPlot 作产品画布（ADR-001），拆 3a 单页视口 / 3b 缩放 / 3c 多列网格三步，并配套
`PageBitmap → D3D11 纹理上传`、用 render 调度层取代 Phase 2 的 `DocSession` 临时线程。

明确不在本阶段范围：字节预算 LRU、失败重试、方向感知预加载（Phase 4）；
阅读位置/目录/书签/旋转/双页/反色（Phase 5）；内嵌字体与主题（Phase 6）。

## 2. 交付物

| 文件 | 说明 |
|---|---|
| `src/canvas/canvas.ixx` / `canvas.cpp` | 自研画布：**纯布局数学**（不依赖 ImGui/D3D），可单测 |
| `src/render/render.ixx` / `render.cpp` | 渲染调度层：单工作线程、页状态机、纹理上传与退役队列 |
| `src/app/main.cpp` | 重写：新增 `Reading` 状态、画布输入、缩放防抖、页绘制、底部状态栏、跳页弹窗、全屏 |
| `src/LilithReader.vcxproj` | 新增 canvas/render 源文件；**移除 ImPlot**（含 include 与 3 个 .cpp） |
| `tests/canvas_test.cpp` | 61 例画布断言（Phase 3 起常备） |
| `tests/run_tests.ps1` | 增加 canvas 模块编译与 `canvas_test.exe` 运行 |
| `docs/03-决策记录.md` | 新增 ADR-018 ~ ADR-021 |

## 3. 关键设计

### 3.1 画布：纯函数布局 + 可失效缓存（ADR-018）

- 文档内部坐标以**点（pt）**为单位，`zoom` = 每点屏幕像素；`margin_px`/`gap_px` 为
  **屏幕像素**常量（换算进文档坐标时除以 zoom，使缩放时间距视觉恒定）。
- `fit_width` 派生 `zoom = (viewport_w − 2·margin_px − (cols−1)·gap_px) / (cols · max_page_w_pt)`，
  该式保证 `content_width_px ≡ viewport_w`。
- 内容窄/矮于视口时居中；否则按 scroll 偏移并钳制到 `[0, content − viewport]`。
- **以鼠标为锚的缩放**：`scroll = doc_pt · z_new − anchor`，再钳制——定点不变性由单测钉死。
- 派生量（内容尺寸、行顶前缀和）做可失效缓存；**滚动不使缓存失效**（滚动只影响原点），
  故每帧滚动仍是 O(1) 查询。
- 本期所有页面按**统一尺寸**布局（取首页尺寸）；`set_page_sizes` 接口已就位，逐页布局留后续。

### 3.2 渲染调度层：线程与纹理（ADR-019 / ADR-020）

- 单工作线程独占 `Document`；UI 线程零 `fz_*`、零阻塞。命令（打开/关闭/解锁）优先于渲染请求。
- 渲染线程用 `ID3D11Device` 直接建纹理（`CreateTexture2D` + 初始数据，`IMMUTABLE`；
  device 方法 free-threaded），上传完成即释放 `PageBitmap`。
- 被替换/逐出的纹理进**退役队列**；UI 线程**帧首** `drain_retired()` 统一 `Release`。
  正确性依据：帧 N 引用的纹理若在帧 N 期间退役，只在帧 N+1 帧首（帧 N `Present` 之后）
  释放；D3D11 的 `Release` 在 GPU 仍使用时由运行时延迟回收。
- 页状态机 `Unloaded → Loading → Loaded / Failed`；请求按 (页, 倍率) **去重**。
- 缓存：Phase 3 用**保留窗口**（可见 ± 预加载，外扩 4 页）逐出窗口外页；Phase 4 换字节预算 LRU。

### 3.3 缩放防抖（3b）

目标倍率稳定 **150ms** 后，才把 `g_want_scale` 切到新倍率请求高清重渲染；期间维持旧倍率，
页面用现有纹理**双线性放大**显示（不闪白、不错位）。首帧（刚进入阅读态）不走防抖。

### 3.4 输入映射

滚轮滚动 / Ctrl+滚轮以鼠标为锚缩放 / 左键拖拽平移 / ←→ 翻行（单列即翻页）/ ↑↓ 与
PgUp PgDn 滚动 / Home End 首末页 / G 跳页 / 1~4 列 / F 与 Ctrl+0 回 fit-width /
+/− 以视口中心缩放 / F11 全屏。

## 4. 落地过程中遇到的问题（均已解决）

1. **`std::min/max` 被 Windows 宏劫持**：`main.cpp` 新增 `std::min/max` 后报 C2589
   "`::` 右边的非法标记"。修：文件头 `#define NOMINMAX`。
2. **模块内前向声明的 COM 类型不与 `d3d11.h` 同一实体**：`render.ixx` 的全局模块片段里
   `struct ID3D11Device;` 与 `main.cpp` 里 `d3d11.h` 的完整声明不被 MSVC 视为同一类型，
   `Renderer(ID3D11Device*)` 报 C2665 无法转换。修：接口改用**不透明 `void*` 设备句柄**
   （与纹理句柄一致），实现单元再 `static_cast`——顺带强化了后端解耦。
3. **画布内联成员导致测试 LNK2019**：`Canvas::state/page_count` 等内联定义在**接口单元**
   （`canvas.ixx`）发射；`canvas_test.exe` 只链 `canvas.obj` 时缺符号。修：链接时一并
   加上 `canvas.ixx.obj`（已写入 `run_tests.ps1` 与 tests/README 备注）。
4. **两条测试期望写错**（非代码缺陷）：
   - 非均匀页尺寸用例未计入**垂直居中偏移**（内容矮于视口时 `origin_y > 0`）；
   - 空文档内容高：原实现为 `2*margin = 32`，与"无内容即无尺寸"的直觉不符。
     修：`ensure_layout` 对 `n == 0` 显式置内容尺寸为 0，并修正测试期望。

## 5. 构建与测试实测

- **编译**：Release x64，`/W4`、`/MT`、`/O2`、`/GL`、`/std:c++23preview`：
  **0 警告 0 错误**（`build.bat`，约 3~7s 增量）。
- **产物**：`bin\Release\LilithReader.exe` = **40.49 MB**（40,487,424 字节）。
  导入表（`dumpbin /dependents`）：`KERNEL32 / USER32 / SHELL32 / d3d11 /
  D3DCOMPILER_47 / IMM32 / api-ms-win-core-synch-l1-1-2-0` —— **全部系统组件，无第三方 DLL**。
  （移除 ImPlot 后 `.text` 略减，总量仍由 MuPDF 的 34MB CJK 字体主导。）
- **自动化测试**（`tests\run_tests.ps1`）：
  - `doc_test.exe`：**41 通过 / 0 失败 / 0 跳过**（Phase 2 回归，未回退）。
  - `canvas_test.exe`：**61 通过 / 0 失败**（Phase 3 新增）。
  - 两测试退出码均为 0。

## 6. 留给后续阶段

- **Phase 4**：保留窗口缓存 → **字节预算 LRU**；失败占位 + 点击重试（自动重试 ≤1）；
  方向感知预加载；纹理延迟释放队列在本层接口上完整化。
- **Phase 5**：`Renderer::authenticate` 已就位（密码框）；阅读位置持久化、目录侧栏、
  书签、缩略图、旋转、双页对开、反色。
- **后续增强**：异构页尺寸的逐页布局（`set_page_sizes` 已备接口）；平滑滚动/惯性
  （Phase 6 微动效）。

## 7. 验证情况（Phase 3 收尾）

- **可程序判定的证据**：画布布局全部自动化（61 例），编译 0 警告 0 错误，依赖表仅系统组件。
- **仍需人工**（人眼/手感，见 [../04-人工验证.md](../04-人工验证.md) Phase 3）：
  渲染与滚动手感、缩放定点与无闪白、多列切换、跳页/全屏/DPI、1000 页性能与 10 分钟内存曲线。
