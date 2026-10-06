# Phase 9：渲染基线与大页面 tile

## 基线方法

`tests/perf_baseline.cpp` 通过 `tests/run_tests.ps1 -Perf` 编译运行，不启动阅读器。每份文档取首页、
中页、末页，倍率 0.75/1/2，记录打开耗时、单页渲染耗时、输出尺寸、RGBA 字节数和进程工作集。
同一进程内顺序执行，首个样本包含字体与 MuPDF 冷启动成本；比较同一文件/页/倍率时以重复行的中位数为准。

本次基线覆盖 `tests/samples` 的格式边界样本，以及 `C:\Users\ACSProgram\Downloads\documents` 中的 PDF、
EPUB、MOBI 和大型 PDF。大型样本显示：单页输出在 2048px 上限下仍可能达到 8~16MiB，扫描/插画页的渲染
耗时随倍率明显上升，工作集随连续取页增长。因此并发不能只追求吞吐，必须同时控制单次 pixmap 与驻留缓存峰值。

同一工具的独立 context 对照（每个 worker 各打开一份 `Document`，并发渲染不同页）没有显示稳定的线性收益：
`Noah Story.pdf` 在 1x 三页串行约 99ms，并发 2/3 worker 约 96/92ms（约 1.03/1.08x），工作集约 120/148MiB；
`Sociology ...pdf` 三页串行约 10ms，而并发批次约 32/54ms，反而变慢。该结果支持“阅读器默认单 worker，
只有经过更长批次和更多机器实测后才考虑并发”的结论。

## 实现事实

- `Document::render_page_tile` 按旋转后输出像素空间裁剪 pixmap，tile 输出尺寸受档位单边上限约束。
- `Renderer` 在不创建额外 D3D11 device 的前提下发布 tile 列表，UI 按整页比例拼接；tile 与普通纹理共用两段式退役队列和字节预算。
- `[cache] ResourceTier` 是唯一资源设置：低/中/高分别为 256/512/768MiB 页缓存、1536/2048/2560px tile 单边。
  旧版 `BudgetMB` 只在首次读取时映射到最接近档位。
- MuPDF worker 并发上限作为档位 profile 暴露，但在更多实测前保持单 `Document` 线程串行，避免共享设备/上下文协调反而放大峰值。
