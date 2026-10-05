// canvas.ixx — Lilith Reader 自研画布（Phase 3）
//
// 职责：把"视图状态 + 文档度量"映射为屏幕布局。**全部是纯计算**：
//       不依赖 ImGui、不依赖 D3D11、不感知线程，因此可以脱离应用外壳单测
//       （tests/canvas_test.cpp），这正是本项目"可程序判定的证据"所要求的。
//
// 设计要点（架构文档 §2）：
//   · 视图状态 CanvasState 是**唯一真源**：任何输入（滚轮/拖拽/键盘）只改 state，
//     渲染每帧按 state 重算布局。无隐藏状态、无失效区域管理，天然可预测。
//   · 页面布局是**纯函数**：page_rect(index, state) → 屏幕矩形。
//   · 坐标系：文档内部以**点（pt）**为单位，缩放 zoom 表示"每点占多少屏幕像素"
//     （zoom = 1.0 即 72dpi 原始尺寸）。屏幕坐标 = 内容点坐标 × zoom + 原点偏移。
//   · margin / gap 的单位是**刻意不同**的（ADR-029）：
//       - margin_px 是**屏幕像素**：它是"窗口边缘的呼吸空间"，属于界面 chrome，
//         不随缩放变化。缩小时内容通常小于视口而被居中，留白基本不可见。
//       - gap_ratio 是**列宽的比例**（文档空间）：屏幕间距 = gap_ratio × 列宽 × zoom，
//         故**随缩放线性变化** —— 缩小时页面变小、间距同比变小，视觉比例恒定。
//         早期版本 gap 也是屏幕像素常量，缩小时页面变小而间距不变，看起来"间距过大"
//         （第四轮反馈）。用比例而非固定 pt，是为了让不同页幅的文档视觉比例一致。
//
// 逐页尺寸（ADR-022）：set_page_sizes 传入各页真实尺寸（列宽取最宽页、行高取行内
// 最高页、每页按自身纵横比绘制），异构 PDF 不再形变。set_uniform 保留为回退路径
// （页数超过探测上限 DocumentInfo::page_sizes 整表为空时），布局函数两者共用。

module;

#include <cstddef>
#include <vector>

export module lilithreader.canvas;

export namespace lr {

// 页面尺寸（点）
struct PageSizePt {
    float w = 0.0f;
    float h = 0.0f;
};

// ---- 视图状态（唯一真源，架构文档 §2.1） ----
struct CanvasState {
    float zoom = 1.0f;        // 每点屏幕像素（fit_width 时该值被派生值覆盖，仅作固定缩放的记忆值）
    float scroll_x = 0.0f;    // 视口左上角在内容坐标（px）中的偏移；内容小于视口时被居中逻辑忽略
    float scroll_y = 0.0f;
    int   columns = 1;        // 1~4 列
    bool  fit_width = true;   // true：zoom 由视口宽度派生（使整行恰好铺满宽度）
    // 双页对开（书籍模式）：封面（第 0 页）单独成页，其余两页对开 —— (1,2)、(3,4)…。
    // 与"columns=2 的均匀网格"的区别仅在**奇偶偏移**：真实书籍装订是封面单张、正文成对。
    // 开启时等效 2 列（columns 被忽略），布局复用网格的翻页/缩放/钳制逻辑。
    bool  spread = false;
    float margin_px = 16.0f;   // 内容四周留白（**屏幕像素**；界面 chrome，不随缩放变化）
    float gap_ratio = 0.013f;  // 页/列间距，占**列宽（最宽页）的比例**；屏幕间距 = 该值 × 列宽 × zoom，
                               // 故随缩放线性变化（ADR-029）
};

// 页面在屏幕上的矩形
struct PageRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

inline constexpr float kMinZoom = 0.10f;   // 固定缩放下限
inline constexpr float kMaxZoom = 10.0f;   // 固定缩放上限
inline constexpr int   kMinColumns = 1;
inline constexpr int   kMaxColumns = 4;

// ---- 画布 ----
//
// 布局本身**无隐藏状态**：除一份可失效的派生缓存（内容尺寸、行顶前缀和）外，
// 所有布局输出都由 (state, viewport, 页尺寸) 唯一决定。缓存只在影响布局的
// 输入变化时失效；滚动不失效（滚动只影响原点，不影响布局）。
//
// 唯一的例外是**阅读游标 nav_row_**：它是"当前读第几行"，只服务于翻页与页码指示，
// **不参与任何布局计算**。之所以必须存在，是因为当视口比"一行"还高时（横向页 /
// 缩小 / 多列），末尾若干行的 row_top 会超过 max_scroll_y，**无法**被对齐到视口顶部；
// 若翻页游标只能由滚动位置反推，就会在末行前反复无进展（"卡住"），
// 或在"到底显示末页"与"反推回前一行"之间来回跳（详见 docs/archive/2026-10/phase3-画布.md §8）。
class Canvas {
public:
    Canvas() = default;

    // ---- 输入 ----
    void set_viewport(float w, float h);
    void set_uniform(int page_count, PageSizePt size);       // 全部页面同一尺寸
    void set_page_sizes(std::vector<PageSizePt> sizes);      // 逐页尺寸（ADR-022）
    void set_default_size(PageSizePt size);                  // 未提供尺寸的页回退值
    void set_state(const CanvasState& s);                    // 原样写入（不钳制）
    void set_margin_gap(float margin_px, float gap_ratio);   // 单位不同，见文件头 ADR-029

    [[nodiscard]] const CanvasState& state() const noexcept { return state_; }
    [[nodiscard]] CanvasState& mutable_state() noexcept { dirty_ = true; return state_; }

    [[nodiscard]] int    page_count() const noexcept { return static_cast<int>(sizes_.size()); }
    [[nodiscard]] float  viewport_w() const noexcept { return viewport_w_; }
    [[nodiscard]] float  viewport_h() const noexcept { return viewport_h_; }
    [[nodiscard]] PageSizePt default_size() const noexcept { return default_; }

    // ---- 派生量 ----
    [[nodiscard]] float effective_zoom() const;              // fit_width 时解析为派生值
    [[nodiscard]] float fit_width_zoom() const;              // 使整行铺满视口宽度的 zoom
    [[nodiscard]] float content_width_px() const;
    [[nodiscard]] float content_height_px() const;
    [[nodiscard]] float max_scroll_x() const;
    [[nodiscard]] float max_scroll_y() const;
    [[nodiscard]] float origin_x() const;                    // 内容左上角的屏幕 x（含居中）
    [[nodiscard]] float origin_y() const;

    // ---- 布局 ----
    [[nodiscard]] int    rows() const;
    [[nodiscard]] int    row_of(int index) const;
    [[nodiscard]] int    first_page_in_row(int row) const;
    [[nodiscard]] int    row_page_begin(int row) const;      // 同 first_page_in_row
    [[nodiscard]] int    row_page_end(int row) const;        // 该行最后一个页（含）
    [[nodiscard]] float  row_top_px(int row) const;          // 内容坐标
    [[nodiscard]] float  row_height_px(int row) const;
    [[nodiscard]] PageRect page_rect(int index) const;

    // 命中测试：内容坐标 y（px）落在哪一行 / 哪一页；越界返回 -1
    [[nodiscard]] int row_at_content_y(float content_y_px) const;
    [[nodiscard]] int page_at_content_y(float content_y_px) const;
    // 视口内可见页范围（含）；无内容返回 first>last
    [[nodiscard]] int visible_first() const;
    [[nodiscard]] int visible_last() const;

    // ---- 阅读游标（翻页与页码指示用；不参与布局） ----
    //
    // current_row() 即 nav_row_：由翻页/跳页/列切换直接设定；手动滚动（滚轮/拖拽/
    // 缩放）后按"视口顶部所在行"回同步，**已滚到底部时取末行**（到底时末行常常
    // 够不到视口顶部，这样页码才会显示末页而不是倒数第二页）。
    [[nodiscard]] int current_row() const;   // 空文档返回 -1
    [[nodiscard]] int current_page() const;  // current_row() 的首个页；空文档返回 -1

    // ---- 操作（只改 state；内部按需钳制） ----
    void clamp_scroll();
    void scroll_by(float dx_px, float dy_px);
    // 翻行：dir > 0 下一行，dir < 0 上一行（单列即翻页）。已在首/末行时为无操作。
    void scroll_rows(int dir);
    // 以屏幕点 (anchor_sx, anchor_sy) 为不动点缩放到 z（切到固定缩放模式）
    void set_zoom(float z, float anchor_sx, float anchor_sy);
    // 以屏幕点为不动点乘以倍率
    void zoom_by(float factor, float anchor_sx, float anchor_sy);
    // 滚到第 index 页顶部（align∈[0,1]：0=页顶贴视口顶，1=页顶贴视口底）
    void scroll_to_page(int index, float align = 0.0f);
    // 切换列数：锚定到当前首个可见页，避免跳变
    void set_columns(int columns);
    // 切换双页对开（书籍模式）：锚定到当前阅读页，避免跳变
    void set_spread(bool on);
    // 回到 fit-width
    void fit_to_width();

private:
    void ensure_layout() const;                 // 重算派生缓存
    [[nodiscard]] PageSizePt size_of(int i) const;
    [[nodiscard]] int row_from_scroll() const;  // 由滚动位置反推当前行（到底取末行）
    void sync_nav_row();                        // nav_row_ ← row_from_scroll()

    // ---- 行/列映射（spread 感知） ----
    //
    // 均匀网格：page i → (row=i/C, col=i%C)，行内页数恒为 C（末行可能少）。
    // 书籍模式：page 0 → (0,0) 独占；page i≥1 → (1+(i-1)/2, (i-1)%2)，等效 2 列。
    // 其余布局（缩放/滚动/钳制/游标）两者共用，故只在这几个映射函数里分支。
    [[nodiscard]] int eff_cols() const;            // spread ? 2 : clamp(columns)
    [[nodiscard]] int compute_rows(int n) const;
    [[nodiscard]] int row_first_page(int row) const;
    [[nodiscard]] int row_page_count(int row) const;  // 该行页数（1 或 2；网格下 ≤ eff_cols）
    [[nodiscard]] int row_col_of(int index) const;    // 页在行内的列号（0 基）

    // ---- 输入 ----
    float viewport_w_ = 0.0f;
    float viewport_h_ = 0.0f;
    CanvasState state_{};
    std::vector<PageSizePt> sizes_;
    PageSizePt default_{ 595.0f, 842.0f };      // 缺省 A4（仅 sizes_ 为空或条目为 0 时使用）

    // 阅读游标（行号）：只服务翻页与页码指示，**不参与布局**。见类头说明。
    int nav_row_ = 0;

    // ---- 派生缓存（mutable：ensure_layout 为 const） ----
    mutable bool  dirty_ = true;
    mutable float eff_zoom_ = 1.0f;
    mutable float content_w_px_ = 0.0f;
    mutable float content_h_px_ = 0.0f;
    mutable float max_page_w_pt_ = 0.0f;
    mutable int   rows_ = 0;
    mutable std::vector<float> row_tops_pt_;    // 前缀和，长度 rows_+1（末项 = 内容高度 - 下边距）
    mutable std::vector<float> row_heights_pt_;
};

}  // namespace lr
