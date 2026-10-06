# Phase 2：MuPDF 文档核心（已完成 2026-10-05）

> 本文件归档已完成阶段的实施细节、事故与实测数据。
> 待人工执行的验证项见 [../04-人工验证.md](../../04-人工验证.md)。

## 1. 目标与范围

按 [迁移阶段总览](迁移阶段总览.md) Phase 2：移植 `Document` 类、修正线程模型、补齐错误处理，
使程序能**真正打开**文档并给出结构化结果。

明确不在本阶段范围：页面渲染上屏、画布、缩放（Phase 3）；纹理与缓存（Phase 4）；
密码输入框、目录侧栏、阅读位置（Phase 5）。

## 2. 交付物

| 文件 | 说明 |
|---|---|
| `src/document/document.ixx` | 文档核心模块接口（MuPDF 无关类型：`DocError` / `DocumentInfo` / `PageBitmap` / `Document`） |
| `src/document/document.cpp` | 模块实现单元（唯一允许出现 `fz_*` 的地方） |
| `src/app/main.cpp` | 接入：后台线程异步打开、六态 UI（引导/拒绝/打开中/失败/就绪）、F3 调试浮层扩展 |
| `src/LilithReader.vcxproj` | 新增源文件、vcpkg 安装根、MuPDF 兼容宏、外部头警告策略 |
| `vcpkg.json` / `vcpkg-configuration.json` | 依赖 `libmupdf`；注册覆盖端口 |
| `overlay-ports/gumbo/` | 覆盖端口（修正上游归档哈希，见 ADR-011） |
| `docs/03-决策记录.md` | 新增 ADR-009 ~ ADR-013 |

## 3. 落地过程中的三个阻塞（均已解决）

### 3.1 项目路径含空格 → harfbuzz 构建失败 LNK1181

- 现象：`harfbuzz:x64-windows` 在宿主 triplet 阶段 `LINK : fatal error LNK1181:
  无法打开输入文件"Reader\vcpkg_installed\x64-windows\debug\lib.obj"`。
- 根因：meson 端口生成的链接命令行**未给 `/libpath:` 加引号**，
  `F:\programs\Lilith Reader\…` 从空格处被截断成两个参数。
- 定位证据：`buildtrees/harfbuzz/package-x64-windows-dbg-out.log` 中该输入文件路径只剩后半段。

### 3.2 vcpkg 的 MSBuild 集成把 triplet 拼两次

- 现象：显式传 `--x-install-root` 后目录变成 `…\x64-windows-static\x64-windows-static\`。
- 根因：`_ZVcpkgInstalledDir`（MSBuild 侧）默认值已含 triplet，它被直接作为
  `--x-install-root` 传给 vcpkg；而 vcpkg 内部 `scripts/ports.cmake:128`
  又拼一次 `CURRENT_INSTALLED_DIR = ${_VCPKG_INSTALLED_DIR}/${TARGET_TRIPLET}`。
  同时 MSBuild 读 include/lib 走 `_ZVcpkgCurrentInstalledDir = VcpkgInstalledDir + triplet`。
- 解法：在 vcxproj 显式设 `VcpkgInstalledDir` 为**不含空格、不含 triplet** 的
  `F:\programs\.Environment\vcpkg-roots\LilithReader`。这样"传给 vcpkg 的安装根"
  与"MSBuild 读到的 `根\triplet`"两种口径同时成立，3.1 与 3.2 一并消除。
- 副产品：Phase 0/1 之所以没暴露 3.2，是因为当时依赖数为 0，错误路径从未被真正使用。

### 3.3 gumbo 上游归档哈希变化

- 现象：`download … had an unexpected hash`（期望 `1513e8…`，实际 `9f5996…`）。
- 排查：手工完整下载 tag 归档与 `refs/tags` 变体，两次都是同一哈希且 `gzip -t` 通过
  （**不是下载截断**）；改用 tag 指向的提交号拉取，哈希同样为 `9f5996…`。
  即 Forgejo 的 tag 归档不是字节稳定的，vcpkg 内置端口钉的哈希已过期。
- 解法：新增覆盖端口，改为按提交号 `322c54c1…` 拉取自包含归档并写入实测哈希（ADR-011）。

## 4. 核对过的 MuPDF 行为（1.26.10，均有源码/头文件依据）

| 结论 | 依据 | 影响 |
|---|---|---|
| `fz_try/fz_catch` 是 `setjmp/longjmp`，与 C++ 异常不互通 | `fitz/context.h:62-64` | 定下 ADR-009 两条纪律 |
| `fz_drop_device` **不会**隐式 `fz_close_device` | `fitz/device.h:390-403` 明确写出 | 修正了 Lilith 原版遗漏 close 的缺陷 |
| 空 magic 合法，会退回内容嗅探 | `source/fitz/document.c` 中 `if (magic == NULL) magic = "";` | 简化了调用路径 |
| 不认识的格式抛 `FZ_ERROR_UNSUPPORTED`（错误码可判，不必猜消息） | 同上，`"cannot find document handler for file type: '%s'"` | 错误分类以错误码为主、消息为辅 |
| `fz_lookup_metadata` 的 size 参数是 `size_t`（不是 `int`） | `fitz/document.h:945` | 避免隐式转换警告 |
| 没有 `fz_keep_context`，context 不可引用计数 | 全头文件无此符号 | 促成 ADR-012 的共享 `CtxHandle` 设计 |
| MSVC 对任何 `_setjmp`（含仅平凡局部的函数）都报 C4611 | 最小样例实测（见 §6.2） | 该警告无法通过改写代码消除，只能定点关闭 |

## 5. 实现要点

- **纪律**：`fz_try` 内只出现 `fz_*` 裸指针与 POD（`char[]`）；跨 `fz_try` 的局部量一律
  `fz_var()`。所有 C++ 对象操作（`std::string`、`shared_ptr`）都在 `fz_try` 之外。
- **零拷贝**：`render_page` 产出 RGBA8 视图，直接指向 `fz_pixmap` 内部缓冲，
  可原样上传 `DXGI_FORMAT_R8G8B8A8_UNORM`；清屏用不透明白底（目标 alpha 恒 255，
  无需反预乘）。
- **尺寸钳制**：单边像素上限（默认 8192），超限等比降采样，真实倍率由
  `PageBitmap::effective_scale()` 回传；只缩不放。
- **生命周期**：`fz_context` 由 `shared_ptr<CtxHandle>` 持有，`Document` 与 `PageBitmap`
  各一份 ⇒ 结构上不可能出现悬垂 context。
- **错误分类**：优先 `fz_caught()` 错误码，`FZ_ERROR_GENERIC` 时回退消息特征
  （加密 PDF 走的正是 GENERIC + "password"）。
- **UI 侧**：专用工作线程独占 `Document`，请求带自增序号，UI 丢弃过期结果；
  格式闸门（扩展名白名单）在本地即时判定，不进线程。

## 6. 构建与产物实测

### 6.1 构建

- Release x64，`/MT`、`/O2`、`/GL`、`/W4`、`/std:c++23preview`：
  **0 警告 0 错误**（`build.bat`，约 10s 增量）。
- 警告策略：`/external:anglebrackets` + `ExternalWarningLevel=TurnOffAllWarnings`
  屏蔽外部头（MuPDF/Windows/STL）的噪声；implot 的两个文件定点关闭 C5054
  （上游在 C++23 下的写法问题）；`document.cpp` 定点关闭 C4611（见 6.2）。

### 6.2 C4611 的验证过程（为什么可以关闭）

现象：`document.cpp` 的 `fz_try` 报 `C4611: "_setjmp" 和 C++ 对象析构之间的交互是不可移植的`，
且同一文件内多个 `fz_try` 只报一条、位置会随外部头警告的开关而移动（说明按编译单元去重）。

用最小样例（`C:\...\Temp` 下临时文件，已删）验证：

```cpp
int case_a(int x) { jmp_buf b; if (_setjmp(b) == 0) return 1; return x; }  // 只有平凡局部量
```

结果：**仍然报警**。即 MSVC 对 C++（/EHsc）中出现的任何 `_setjmp` 都无条件报 C4611，
与函数内是否存在待析构对象无关。MuPDF 的 `fz_setjmp` 在 MSVC 上展开为 `_setjmp`，
故该警告在本文件必然出现、无法靠改写代码消除，也不代表 ADR-009 的纪律被破坏。
因此在该文件内关闭，纪律改由注释 + ADR + 评审把关。

### 6.3 产物

- `bin\Release\LilithReader.exe` = **40.4 MB**（+ 34MB PDB，仅开发用）。
- 依赖表（`dumpbin /dependents`）：`KERNEL32 / USER32 / SHELL32 / d3d11 /
  D3DCOMPILER_47 / IMM32 / api-ms-win-core-synch-l1-2-0` —— **全部为系统组件，无第三方 DLL**。
- 体积构成（`dumpbin` 节大小，十六进制）：`.data` ≈ 34 MB、`.text` ≈ 3.8 MB、
  `.rdata` ≈ 2.4 MB，其余为 `.pdata/.reloc/.rsrc` 等。
  `.data` 的绝对主体是 MuPDF **内置字体资源**：
  `SourceHanSerif-Regular.ttc` 24.8 MB + `DroidSansFallbackFull.ttf` 5.1 MB
  + `DroidSansFallback.ttf` 3.6 MB。
- 该字体是**运行期真的会用到**的 CJK 兜底：`source/fitz/font-table.h:285`
  `FONT(han, SourceHanSerif_Regular_ttc, …)`，由 `fz_new_cjk_font` 在文档未内嵌
  中文字体时调用。对中文读者属刚需，故本阶段**不做裁剪**。
- 结论：README 原定 "< 30MB" 在纳入 CJK 兜底字体后不成立，预算已据实调整
  （见迁移计划 §4），裁剪手段留到 Phase 7（编译 `TOFU_CJK_LANG` 走 Droid、
  去掉罕用 Noto 文种、UPX 等）。

## 7. 留给后续阶段

- Phase 3：画布、缩放、网格；`PageBitmap` → D3D11 纹理上传；届时 `DocSession`
  这套临时线程被 render 调度层取代。
- Phase 4：字节预算页缓存、纹理延迟释放队列、失败重试。
- Phase 5：密码输入框（`Document::authenticate` 已就绪）、目录侧栏（`has_outline` 已就绪）、
  阅读位置持久化。
- Phase 7：体积裁剪（见 6.3）、图标与版本资源、崩溃转储。
- 待评估：ImPlot 在 Phase 3 自研画布落地后即可从构建中移除（当前仍在链接，约 1MB 量级）。

## 8. 验证情况（Phase 2 收尾更新，2026-10-05）

- **人工验证**：用户已实际运行并验证了大部分项目——各格式打开、拖拽与交互、
  窗口状态等；加密与损坏样本当时无法构造故未覆盖（现已由自动化补齐，见下）。
- **自动化覆盖**：Phase 2 收尾建立了 `tests/` 常备测试（ADR-017）——
  合成 29 个样本（含 PyMuPDF 生成的 AES-128/AES-256/owner-only 加密 PDF），
  `doc_test.cpp` 对项目自己的 `document` 模块跑 **41 例断言，全部通过**：
  合法格式、改名放行（图片/epub/xps/fb2/pdf 互换）、归档冒充拒绝、
  0 字节/截断/垃圾头/随机字节、加密三态、扩展名闸门 12 例。
- **仍需人工**：交互与回归（2-13/2-14/2-15/2-16 快速拖入拖出）、真实 `.mobi`
  样本、渲染性能与 DPI（部分属 Phase 3 范畴）。清单见 [../04-人工验证.md](../../04-人工验证.md)。
- **过程中修掉的问题**：① 拖文件到 exe 图标时在文件目录生成 `imgui.ini`
  （根因：Explorer 把工作目录设为被拖文件目录；已 `io.IniFilename = nullptr` 关闭）；
  ② zip 图集改名 `.epub` 显示假页数（ADR-015/016，`Mismatched` 错误码 + 归档冒充判定）；
  ③ 原生图片被扩展名闸门误拦（ADR-017 放行）。

## 9. 人工验证归档（2026-10-05，第四轮收尾）

Phase 2 的待验证清单（原 `docs/04-人工验证.md`「Phase 2 待验证」整节）已由用户实际运行
逐项试用，**未发现异常**，故整节从待验证清单移入本文件存档。逐项结论：

| # | 操作 | 结论 |
|---|---|---|
| 2-1 | 拖入正常多页 PDF | ✅ 元信息/标题/无卡顿均正常 |
| 2-2 | 命令行打开 | ✅ |
| 2-3 | 带目录 PDF | ✅ 显示"含目录" |
| 2-4 | epub / mobi / fb2 / cbz / xps 各 1 个 | ✅ 均能读出页数与首页尺寸（真实 `.mobi` 未单独复测，列为非阻塞复核项） |
| 2-5 | 打开后 Esc 再拖入另一个文件 | ✅ 无残留状态 |
| 2-6 | 连按两次 Esc | ✅ 先回引导页、再退出 |
| 2-13 | 不存在的路径 | ✅ 显示"文件不存在"及完整路径 |
| 2-14 | 不在支持清单内的扩展名 | ✅ 显示"暂不支持的格式"及支持列表 |
| 2-15 | 0 字节非支持后缀 | ✅ 闸门优先，显示"暂不支持的格式" |
| 2-16 | 连续快速拖入拖出失败样本 20 次 | ✅ 不崩溃、不卡死 |
| 2-16h | 真实 `.mobi` 样本 | ⏸ 无法程序化构造，列为非阻塞复核项 |
| 2-17 | 1000 页 PDF 打开期间界面 | ✅ 可响应、有"正在打开"动画、无"未响应" |
| 2-18 | 打开中按 Esc | ✅ 立即回引导页，无结果回填覆盖 |
| 2-19 | 打开大文件时改拖小文件 | ✅ 稳定显示后拖入的那个 |
| 2-20 | F3 | ✅ 浮层正常显示/关闭 |
| 2-21 | 关闭再启动 | ✅ 窗口位置/尺寸/最大化状态保持 |
| 2-22 | 双显示器拖到副屏再重启 | ✅ 回到上次那块屏 |
| 2-23 | 100%/150%/200% DPI 下打开 | ✅ 文字清晰、居中文本无错位 |
| 2-24 | exe 体积 | ✅ 40.5MB（Phase 3/4 修复后仍为 40.5MB，无第三方 DLL 增量） |
| 2-25 | 干净环境双击运行 | ✅ 可启动、不提示缺 DLL |
| 2-26 | 检查导入表 | ✅ 仅系统组件（KERNEL32/USER32/GDI32/D3D11/DXGI/SHELL32/SHCORE…） |
| 2-27 | 把 PDF 拖到 exe 图标上打开 | ✅ 不再生成 `imgui.ini`；`LilithReader.ini` 正常更新 |

> 纪律提示：本表是**已完成**的记录，不再回填到 `04-人工验证.md`。后续若发现回归，
> 在该文件新增条目即可。

