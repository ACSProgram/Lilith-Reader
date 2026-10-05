# tests — 自动化测试（ADR-017，Phase 2 起常备）

把格式识别、边界输入、画布布局这类最容易回归的逻辑固化成断言，绕过应用外壳直接测
`src/document` 与 `src/canvas` 模块。人工验证清单（`docs/04-人工验证.md`）只保留交互/渲染类项目。

## 用法

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1          # 常规：生成样本 → 编译 → 运行 doc_test(43 例) + canvas_test(92 例)
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1 -Probe   # 额外跑 MuPDF 诊断探针（打印 FZ_META_FORMAT 等）
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1 -NoRegenerate  # 复用已有 samples/
```

- 退出码 = 任一测试失败即非零，可直接接入 CI。
- `tests/samples/`、`tests/_build/` 为生成物，已 gitignore。

## 文件

| 文件 | 作用 |
|---|---|
| `make_samples.py` | 合成 27 个常规样本（合法/改名/归档冒充/损坏，含**异构页尺寸** `mixed_size.pdf`）+ 3 个加密 PDF（需 PyMuPDF：`pip install pymupdf`，未装则跳过加密例） |
| `doc_test.cpp` | 43 例断言表：合法格式、改名放行、归档冒充拒绝、加密三态、扩展名闸门、**逐页尺寸**（形变防线） |
| `canvas_test.cpp` | 92 例断言：fit-width 派生、固定缩放居中、内容尺寸、滚动钳制、以鼠标为锚的缩放定点不变性、命中测试、可见范围、列切换锚定、非均匀页尺寸、缩放钳制、空文档、**翻页游标**（矮页视口不卡住 / 多列按行推进 / 与手动滚动同步）、**页间距随缩放**（ADR-029：间距 = 比例×页宽×zoom，fit-width 视口减半则间距约减半） |
| `mupdf_probe.cpp` | 诊断工具：打印 MuPDF 对每个样本的原始判定（页数、`FZ_META_FORMAT`），新增格式支持时先用它摸底 |
| `run_tests.ps1` | 一键编译 + 运行；`cl.exe` 直调，链接配置独立于 `build.bat` 的 vcpkg，MuPDF 升级时需同步其库列表 |

## 注意

- 脚本/测试均按 UTF-8 输出；控制台若乱码，先 `chcp 65001`。
- 扩展名白名单（`utils.ixx`）与 `doc_test.cpp` 的闸门断言需保持同步。
- 链接 `canvas_test.exe` 时**必须一并链接 `canvas.ixx.obj`**：画布接口单元里的内联
  成员（如 `Canvas::state`）由它发射，只链 `canvas.obj` 会报 LNK2019。
- `minimal_pdf` 的对象编号约定是 `Page = 5+2i`、`Contents = 6+2i`（`Kids` 必须用前者）。
  第三轮调试前这里写成 `4+2i`，生成的是**非法 PDF**（靠 MuPDF 修复才打开、各页被修成
  默认 Letter 尺寸），曾掩盖"异构页尺寸形变"这一缺陷 —— 改动该函数时务必保持自洽。
