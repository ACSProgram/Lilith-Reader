# tests — 自动化测试（ADR-017，Phase 2 起常备）

把格式识别、边界输入、画布布局这类最容易回归的逻辑固化成断言，绕过应用外壳直接测
`src/document` 与 `src/canvas` 模块。人工验证清单（`docs/04-人工验证.md`）只保留交互/渲染类项目。

## 用法

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1          # 常规：生成样本 → 编译 → 运行 doc_test(55) + canvas_test(126) + page_cache_test(32) + reader_state_test(110) + 资源断言（字体子集 / 图标帧集）
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1 -Probe   # 额外跑 MuPDF 诊断探针（打印 FZ_META_FORMAT 等）
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1 -NoRegenerate  # 复用已有 samples/
```

- 退出码 = 任一测试失败即非零，可直接接入 CI。
- `tests/samples/`、`tests/_build/` 为生成物，已 gitignore。

## 文件

| 文件 | 作用 |
|---|---|
| `make_samples.py` | 合成 28 个常规样本（合法/改名/归档冒充/损坏，含**异构页尺寸** `mixed_size.pdf` 与**带两级目录** `outline.pdf`）+ 3 个加密 PDF（需 PyMuPDF：`pip install pymupdf`，未装则跳过加密例） |
| `doc_test.cpp` | 55 例断言表：合法格式、改名放行、归档冒充拒绝、加密三态、扩展名闸门、**逐页尺寸**（形变防线）、**目录解析**（顺序/层级/页号）、**旋转渲染**（0°/180° 尺寸不变、90° 宽高互换）、**配色**（反色背景白→黑且保留 alpha、护眼 R>B） |
| `canvas_test.cpp` | 126 例断言：fit-width 派生、固定缩放居中、内容尺寸、滚动钳制、以鼠标为锚的缩放定点不变性、命中测试、可见范围、列切换锚定、非均匀页尺寸、缩放钳制、空文档、**翻页游标**（矮页视口不卡住 / 多列按行推进 / 与手动滚动同步）、**页间距随缩放**（ADR-029）、**双页对开**（ADR-038：封面单独居中 / 对开分列 / 按行推进 / `spread=false` 回归守卫 / 切换锚定 / **对开与列数同层级互斥**：切列退出对开、`columns` 值保留） |
| `page_cache_test.cpp` | 32 例断言：预算钳制（0/下限/上限/SIZE_MAX）、自动重试上限与 `should_render_failed`、**LRU 逐出**（最久未用先出、pinned 保护、同序号按下标定序、恰好达标即停、未驻留页跳过、按字节累计）（Phase 4，ADR-030/031） |
| `reader_state_test.cpp` | 110 例断言（Phase 5，ADR-034；ADR-062/065 增身份分层与位置列表）：序列化往返（含书签/确定性）、容器语义（find/upsert/erase/rekey）、**坏输入一律安全拒绝**（nullptr/magic/版本不配对/路径超长/位置条数或长度超限/超限/截断）、字段钳制（列/旋转/配色/非法 zoom）、文档键（不存在→0、同路径稳定）、**v3 身份与位置往返**（path_key/page_count/多条位置及其顺序/last_location）、**v1 迁移**（旧 key → path_key，位置留空）、**v2 迁移**（单路径 → 位置列表，空路径不进表）、**分层定位**（指纹命中/relocated/回到记过的位置不再询问/关掉智能匹配/页数不符不继承/adopt 改挂主键并累积位置/primary_key/rekey）、**位置记忆**（去重、最近优先、封顶丢最旧）、**内容指纹**（同一文件稳定、改中段或尾段一字节即变、复制到新路径不变） |
| `mupdf_probe.cpp` | 诊断工具：打印 MuPDF 对每个样本的原始判定（页数、`FZ_META_FORMAT`），新增格式支持时先用它摸底 |
| `../assets/check_icons.py` | 图标资源断言（Phase 7，ADR-051）：每个 `.ico` 的帧集必须完整（17 帧），且每帧位图的**真实解码尺寸**必须等于目录项声明的尺寸 —— 防"小图塞进大槽位 / 只剩一帧"这类只在特定 DPI 下暴露的错 |
| `run_tests.ps1` | 一键编译 + 运行；`cl.exe` 直调，链接配置独立于 `build.bat` 的 vcpkg，MuPDF 升级时需同步其库列表 |

## 注意

- 脚本/测试均按 UTF-8 输出；控制台若乱码，先 `chcp 65001`。
- 扩展名白名单（`utils.ixx`）与 `doc_test.cpp` 的闸门断言需保持同步。
- 链接 `canvas_test.exe` 时**必须一并链接 `canvas.ixx.obj`**：画布接口单元里的内联
  成员（如 `Canvas::state`）由它发射，只链 `canvas.obj` 会报 LNK2019。
- 同理 `page_cache_test.exe` 必须链 `page_cache.ixx.obj`：`select_evictions` 定义在接口单元里。
- `reader_state_test.exe` 必须链 `reader_state.ixx.obj` + `reader_state.obj`：接口单元发射
  `ReaderState` 的成员函数（`find`/`upsert`/`erase`），实现单元提供 `encode_state`/`document_key` 等。
  用例含内容指纹断言，故编译时还需 `/reference utils.ifc`（`file_fingerprint` 是内联函数，无需额外链 obj）。
- `minimal_pdf` 的对象编号约定是 `Page = 5+2i`、`Contents = 6+2i`（`Kids` 必须用前者）。
  第三轮调试前这里写成 `4+2i`，生成的是**非法 PDF**（靠 MuPDF 修复才打开、各页被修成
  默认 Letter 尺寸），曾掩盖"异构页尺寸形变"这一缺陷 —— 改动该函数时务必保持自洽。
