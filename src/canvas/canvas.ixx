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
//   · margin/gap 是**屏幕像素**常量，换算进文档坐标时除以 zoom，
//     因此缩放时页间距在视觉上保持恒定（符合阅读器直觉）。
//
// 本期（Phase 3）简化：**所有页面按统一尺寸布局**（取首页尺寸）。
//   PDF/EPUB/CBZ 绝大多数页面同尺寸；异构尺寸页的逐页布局留待后续阶段，
//   届时只需把 set_uniform 换成 set_page_sizes 的逐页数据即可，布局函数无需改动。

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
    float margin_px = 16.0f;  // 内容四周留白（屏幕像素）
    float gap_px = 12.0f;     // 页间/列间间距（屏幕像素）
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
// 无隐藏状态：除一份**可失效的派生缓存**（内容尺寸、行顶前缀和）外，
// 所有输出都由 (state, viewport, 页尺寸) 唯一决定。缓存只在影响布局的
// 输入变化时失效；滚动不失效（滚动只影响原点，不影响布局）。
class Canvas {
public:
    Canvas() = default;

    // ---- 输入 ----
    void set_viewport(float w, float h);
    void set_uniform(int page_count, PageSizePt size);       // 全部页面同一尺寸
    void set_page_sizes(std::vector<PageSizePt> sizes);      // 逐页尺寸（留待后续阶段）
    void set_default_size(PageSizePt size);                  // 未提供尺寸的页回退值
    void set_state(const CanvasState& s);                    // 原样写入（不钳制）
    void set_margin_gap(float margin_px, float gap_px);

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

    // 命中测试：内容坐标 y（px）落在哪一行 → 返回该行首页；越界返回 -1
    [[nodiscard]] int page_at_content_y(float content_y_px) const;
    // 视口内可见页范围（含）；无内容返回 first>last
    [[nodiscard]] int visible_first() const;
    [[nodiscard]] int visible_last() const;

    // ---- 操作（只改 state；内部按需钳制） ----
    void clamp_scroll();
    void scroll_by(float dx_px, float dy_px);
    // 以屏幕点 (anchor_sx, anchor_sy) 为不动点缩放到 z（切到固定缩放模式）
    void set_zoom(float z, float anchor_sx, float anchor_sy);
    // 以屏幕点为不动点乘以倍率
    void zoom_by(float factor, float anchor_sx, float anchor_sy);
    // 滚到第 index 页顶部（align∈[0,1]：0=页顶贴视口顶，1=页顶贴视口底）
    void scroll_to_page(int index, float align = 0.0f);
    // 切换列数：锚定到当前首个可见页，避免跳变
    void set_columns(int columns);
    // 回到 fit-width
    void fit_to_width();

private:
    void ensure_layout() const;                 // 重算派生缓存
    [[nodiscard]] PageSizePt size_of(int i) const;

    // ---- 输入 ----
    float viewport_w_ = 0.0f;
    float viewport_h_ = 0.0f;
    CanvasState state_{};
    std::vector<PageSizePt> sizes_;
    PageSizePt default_{ 595.0f, 842.0f };      // 缺省 A4（仅 sizes_ 为空或条目为 0 时使用）

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
