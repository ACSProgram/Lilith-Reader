// canvas.cpp — lilithreader.canvas 的实现单元（Phase 3）
//
// 纯几何计算，不含任何 ImGui / D3D / 线程代码，因此可脱离应用外壳单测。
// 所有派生量在 ensure_layout() 里一次算清并缓存；滚动不影响布局，
// 故滚动不会使缓存失效（保证每帧滚动时仍是 O(1) 查询）。

module;

#include <algorithm>
#include <cmath>
#include <utility>

module lilithreader.canvas;

namespace lr {
namespace {

float clampf(float v, float lo, float hi) noexcept {
    if (!(v == v)) return lo;  // NaN 防御
    return v < lo ? lo : (v > hi ? hi : v);
}

int clampi(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

// ---- 输入 ----

void Canvas::set_viewport(float w, float h) {
    viewport_w_ = w > 0.0f ? w : 0.0f;
    viewport_h_ = h > 0.0f ? h : 0.0f;
    dirty_ = true;
}

void Canvas::set_uniform(int page_count, PageSizePt size) {
    sizes_.assign(page_count > 0 ? static_cast<std::size_t>(page_count) : 0, size);
    dirty_ = true;
}

void Canvas::set_page_sizes(std::vector<PageSizePt> sizes) {
    sizes_ = std::move(sizes);
    dirty_ = true;
}

void Canvas::set_default_size(PageSizePt size) {
    if (size.w > 0.0f && size.h > 0.0f) default_ = size;
    dirty_ = true;
}

void Canvas::set_state(const CanvasState& s) {
    state_ = s;
    dirty_ = true;
}

void Canvas::set_margin_gap(float margin_px, float gap_px) {
    state_.margin_px = margin_px > 0.0f ? margin_px : 0.0f;
    state_.gap_px = gap_px >= 0.0f ? gap_px : 0.0f;
    dirty_ = true;
}

PageSizePt Canvas::size_of(int i) const {
    if (i >= 0 && static_cast<std::size_t>(i) < sizes_.size()) {
        const PageSizePt s = sizes_[static_cast<std::size_t>(i)];
        if (s.w > 0.0f && s.h > 0.0f) return s;
    }
    return default_;
}

// ---- 派生缓存 ----

void Canvas::ensure_layout() const {
    if (!dirty_) return;
    dirty_ = false;

    const int n = static_cast<int>(sizes_.size());
    const int cols = clampi(state_.columns, kMinColumns, kMaxColumns);

    // 列宽 = 最宽页；行高 = 该行最高页（统一尺寸时即页高）
    float maxw = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float w = size_of(i).w;
        if (w > maxw) maxw = w;
    }
    if (!(maxw > 0.0f)) maxw = default_.w > 0.0f ? default_.w : 595.0f;
    max_page_w_pt_ = maxw;

    // 解析 zoom
    if (state_.fit_width) {
        const float denom = static_cast<float>(cols) * maxw;
        float z = 1.0f;
        if (denom > 0.0f)
            z = (viewport_w_ - 2.0f * state_.margin_px -
                 static_cast<float>(cols - 1) * state_.gap_px) / denom;
        if (!(z > 0.0f)) z = 0.01f;                 // 视口退化时的下限
        eff_zoom_ = clampf(z, 0.01f, 100.0f);
    } else {
        eff_zoom_ = clampf(state_.zoom, kMinZoom, kMaxZoom);
    }
    if (!(eff_zoom_ > 0.0f)) eff_zoom_ = 1.0f;

    const float margin_pt = state_.margin_px / eff_zoom_;
    const float gap_pt = state_.gap_px / eff_zoom_;
    const float col_w_pt = maxw;
    const float col_pitch_pt = col_w_pt + gap_pt;

    rows_ = n > 0 ? (n + cols - 1) / cols : 0;
    row_heights_pt_.assign(static_cast<std::size_t>(rows_), 0.0f);
    row_tops_pt_.assign(static_cast<std::size_t>(rows_) + 1, 0.0f);

    for (int r = 0; r < rows_; ++r) {
        float h = 0.0f;
        for (int c = 0; c < cols; ++c) {
            const int i = r * cols + c;
            if (i >= n) break;
            const float ph = size_of(i).h;
            if (ph > h) h = ph;
        }
        if (!(h > 0.0f)) h = default_.h > 0.0f ? default_.h : 842.0f;
        row_heights_pt_[static_cast<std::size_t>(r)] = h;
    }
    row_tops_pt_[0] = margin_pt;
    for (int r = 0; r < rows_; ++r) {
        row_tops_pt_[static_cast<std::size_t>(r) + 1] =
            row_tops_pt_[static_cast<std::size_t>(r)] +
            row_heights_pt_[static_cast<std::size_t>(r)] + gap_pt;
    }

    const float inner_w_pt =
        static_cast<float>(cols) * col_w_pt + static_cast<float>(cols - 1) * gap_pt;
    const float inner_h_pt =
        rows_ > 0 ? (row_tops_pt_[static_cast<std::size_t>(rows_)] - margin_pt - gap_pt) : 0.0f;

    // 内容像素尺寸。fit-width 下 content_w_px_ 恒等于 viewport_w_（见头文件注释）。
    content_w_px_ = (inner_w_pt + 2.0f * margin_pt) * eff_zoom_;
    content_h_px_ = (inner_h_pt + 2.0f * margin_pt) * eff_zoom_;
    if (n == 0) {  // 空文档：无内容即无尺寸（不保留留白）
        content_w_px_ = 0.0f;
        content_h_px_ = 0.0f;
    }
    (void)col_pitch_pt;
}

float Canvas::effective_zoom() const { ensure_layout(); return eff_zoom_; }

float Canvas::fit_width_zoom() const {
    // 独立于当前 fit_width 开关，供 UI 显示与测试使用
    const int cols = clampi(state_.columns, kMinColumns, kMaxColumns);
    const int n = static_cast<int>(sizes_.size());
    float maxw = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float w = size_of(i).w;
        if (w > maxw) maxw = w;
    }
    if (!(maxw > 0.0f)) maxw = default_.w > 0.0f ? default_.w : 595.0f;
    const float denom = static_cast<float>(cols) * maxw;
    if (!(denom > 0.0f)) return 1.0f;
    const float z = (viewport_w_ - 2.0f * state_.margin_px -
                     static_cast<float>(cols - 1) * state_.gap_px) / denom;
    if (!(z > 0.0f)) return 0.01f;
    return clampf(z, 0.01f, 100.0f);
}

float Canvas::content_width_px() const { ensure_layout(); return content_w_px_; }
float Canvas::content_height_px() const { ensure_layout(); return content_h_px_; }

float Canvas::max_scroll_x() const {
    ensure_layout();
    return std::max(0.0f, content_w_px_ - viewport_w_);
}
float Canvas::max_scroll_y() const {
    ensure_layout();
    return std::max(0.0f, content_h_px_ - viewport_h_);
}

float Canvas::origin_x() const {
    ensure_layout();
    return content_w_px_ <= viewport_w_ ? (viewport_w_ - content_w_px_) * 0.5f
                                        : -state_.scroll_x;
}
float Canvas::origin_y() const {
    ensure_layout();
    return content_h_px_ <= viewport_h_ ? (viewport_h_ - content_h_px_) * 0.5f
                                        : -state_.scroll_y;
}

// ---- 布局 ----

int Canvas::rows() const { ensure_layout(); return rows_; }

int Canvas::row_of(int index) const {
    const int cols = clampi(state_.columns, kMinColumns, kMaxColumns);
    return index < 0 ? -1 : index / cols;
}

int Canvas::first_page_in_row(int row) const {
    const int cols = clampi(state_.columns, kMinColumns, kMaxColumns);
    return row < 0 ? 0 : row * cols;
}
int Canvas::row_page_begin(int row) const { return first_page_in_row(row); }

int Canvas::row_page_end(int row) const {
    ensure_layout();
    const int cols = clampi(state_.columns, kMinColumns, kMaxColumns);
    const int last = (row + 1) * cols - 1;
    const int n = static_cast<int>(sizes_.size());
    if (row < 0) return -1;
    return last > n - 1 ? n - 1 : last;
}

float Canvas::row_top_px(int row) const {
    ensure_layout();
    if (rows_ <= 0) return 0.0f;
    const int r = clampi(row, 0, rows_ - 1);
    return row_tops_pt_[static_cast<std::size_t>(r)] * eff_zoom_;
}

float Canvas::row_height_px(int row) const {
    ensure_layout();
    if (rows_ <= 0) return 0.0f;
    const int r = clampi(row, 0, rows_ - 1);
    return row_heights_pt_[static_cast<std::size_t>(r)] * eff_zoom_;
}

PageRect Canvas::page_rect(int index) const {
    ensure_layout();
    const int n = static_cast<int>(sizes_.size());
    if (index < 0 || index >= n) return {};

    const int cols = clampi(state_.columns, kMinColumns, kMaxColumns);
    const int col = index % cols;
    const int row = index / cols;
    const PageSizePt s = size_of(index);

    const float z = eff_zoom_;
    const float margin_pt = state_.margin_px / z;
    const float gap_pt = state_.gap_px / z;
    const float col_pitch_pt = max_page_w_pt_ + gap_pt;

    // 列内水平居中（统一尺寸时居中量为 0）
    const float x_pt = margin_pt + static_cast<float>(col) * col_pitch_pt +
                       (max_page_w_pt_ - s.w) * 0.5f;
    const float y_pt = row_tops_pt_[static_cast<std::size_t>(row)];

    return { origin_x() + x_pt * z, origin_y() + y_pt * z, s.w * z, s.h * z };
}

int Canvas::page_at_content_y(float content_y_px) const {
    ensure_layout();
    if (rows_ <= 0) return -1;
    const float y_pt = content_y_px / eff_zoom_;
    // row_tops_pt_ 升序，找第一个 > y_pt 的下标，其前一个即所在行
    const auto it = std::upper_bound(row_tops_pt_.begin(), row_tops_pt_.end(), y_pt);
    int row = static_cast<int>(it - row_tops_pt_.begin()) - 1;
    row = clampi(row, 0, rows_ - 1);
    const int cols = clampi(state_.columns, kMinColumns, kMaxColumns);
    return row * cols;
}

int Canvas::visible_first() const {
    ensure_layout();
    const int n = static_cast<int>(sizes_.size());
    if (n <= 0) return 0;
    if (content_h_px_ <= viewport_h_) return 0;
    const float y_pt = state_.scroll_y / eff_zoom_;
    const auto it = std::upper_bound(row_tops_pt_.begin(), row_tops_pt_.end(), y_pt);
    int row = static_cast<int>(it - row_tops_pt_.begin()) - 1;
    row = clampi(row, 0, rows_ - 1);
    const int cols = clampi(state_.columns, kMinColumns, kMaxColumns);
    return row * cols;
}

int Canvas::visible_last() const {
    ensure_layout();
    const int n = static_cast<int>(sizes_.size());
    if (n <= 0) return -1;
    if (content_h_px_ <= viewport_h_) return n - 1;
    // 视口底边（减一个极小量，避免恰好落在下一行顶部时多算一行）
    const float bottom_pt = (state_.scroll_y + viewport_h_ - 0.001f) / eff_zoom_;
    const auto it = std::upper_bound(row_tops_pt_.begin(), row_tops_pt_.end(), bottom_pt);
    int row = static_cast<int>(it - row_tops_pt_.begin()) - 1;
    row = clampi(row, 0, rows_ - 1);
    const int cols = clampi(state_.columns, kMinColumns, kMaxColumns);
    const int last = (row + 1) * cols - 1;
    return last > n - 1 ? n - 1 : last;
}

// ---- 操作 ----

void Canvas::clamp_scroll() {
    state_.scroll_x = clampf(state_.scroll_x, 0.0f, max_scroll_x());
    state_.scroll_y = clampf(state_.scroll_y, 0.0f, max_scroll_y());
}

void Canvas::scroll_by(float dx_px, float dy_px) {
    state_.scroll_x += dx_px;
    state_.scroll_y += dy_px;
    clamp_scroll();
}

void Canvas::set_zoom(float z, float anchor_sx, float anchor_sy) {
    // 缩放前先记下锚点对应的文档点（pt），缩放后让它仍落在同一屏幕点
    const float z_old = effective_zoom();
    const float ox_old = origin_x();
    const float oy_old = origin_y();
    const float doc_x_pt = (anchor_sx - ox_old) / z_old;
    const float doc_y_pt = (anchor_sy - oy_old) / z_old;

    state_.zoom = clampf(z, kMinZoom, kMaxZoom);
    state_.fit_width = false;
    dirty_ = true;
    ensure_layout();
    const float z_new = eff_zoom_;

    state_.scroll_x = content_w_px_ > viewport_w_ ? doc_x_pt * z_new - anchor_sx : 0.0f;
    state_.scroll_y = content_h_px_ > viewport_h_ ? doc_y_pt * z_new - anchor_sy : 0.0f;
    clamp_scroll();
}

void Canvas::zoom_by(float factor, float anchor_sx, float anchor_sy) {
    if (!(factor > 0.0f)) return;
    set_zoom(effective_zoom() * factor, anchor_sx, anchor_sy);
}

void Canvas::scroll_to_page(int index, float align) {
    ensure_layout();
    const int n = static_cast<int>(sizes_.size());
    if (n <= 0) return;
    index = clampi(index, 0, n - 1);
    const int row = row_of(index);
    const float top_px = row_tops_pt_[static_cast<std::size_t>(row)] * eff_zoom_;
    state_.scroll_y = top_px - clampf(align, 0.0f, 1.0f) * viewport_h_;
    clamp_scroll();
}

void Canvas::set_columns(int columns) {
    const int c = clampi(columns, kMinColumns, kMaxColumns);
    if (c == clampi(state_.columns, kMinColumns, kMaxColumns)) return;
    const int anchor = visible_first();  // 切换前记录首个可见页
    state_.columns = c;
    dirty_ = true;
    scroll_to_page(anchor, 0.0f);
}

void Canvas::fit_to_width() {
    state_.fit_width = true;
    dirty_ = true;
    clamp_scroll();
}

}  // namespace lr
