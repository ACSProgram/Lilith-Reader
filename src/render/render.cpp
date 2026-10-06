// render.cpp — lilithreader.render 的实现单元（Phase 3 建立，Phase 4 完善）
//
// 线程模型（架构文档 §3.1）：
//   · UI 线程：只投递命令/渲染请求、读取快照；零 fz_*、零阻塞。
//   · 工作线程（本文件）：独占 Document；渲染 → 建纹理 → 发布快照。
//   · 纹理生命周期：渲染线程建纹理（ID3D11Device 方法可多线程调用），
//     被替换/逐出的纹理进**两段式退役队列**；UI 线程帧首 drain_retired() 释放。
//
// 两段式退役队列（ADR-033）：
//   渲染线程把退役纹理放入 retired_pending（"本帧"）；drain_retired() 先释放
//   retired_ready（"上一帧的 pending"），再把 pending 移入 ready。
//   于是任何纹理都至少**活过一个完整帧**才被 Release —— 即使将来有人把
//   drain 挪到帧末，"本帧 draw list 引用过的纹理"也不会被提前释放。
//
// 本文件不含任何 fz_* 调用（只经 Document/PageBitmap 的公开接口），
// 故不受 ADR-009 的 setjmp 纪律约束。

module;

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

module lilithreader.render;

import lilithreader.document;
import lilithreader.page_cache;
import lilithreader.utils;   // file_fingerprint（ADR-062）

namespace lr {
namespace {

// 用 MuPDF 的 RGBA8 缓冲直接建纹理：IMMUTABLE + 初始数据，一次调用完成上传。
// 这是 ID3D11Device 的方法（free-threaded），可在渲染线程安全调用。
bool create_page_texture(ID3D11Device* dev, const std::uint8_t* data,
                         int w, int h, int stride, ID3D11ShaderResourceView** out) {
    *out = nullptr;
    if (dev == nullptr || data == nullptr || w <= 0 || h <= 0 || stride <= 0) return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = data;
    init.SysMemPitch = static_cast<UINT>(stride);

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&desc, &init, &tex))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = desc.Format;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MostDetailedMip = 0;
    sv.Texture2D.MipLevels = 1;
    const HRESULT hr = dev->CreateShaderResourceView(tex, &sv, out);
    tex->Release();
    if (FAILED(hr)) {
        *out = nullptr;
        return false;
    }
    return true;
}

bool wants_equal(const std::vector<RenderWant>& a, const std::vector<RenderWant>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].page != b[i].page) return false;
        if (std::fabs(a[i].scale - b[i].scale) > 1e-3f) return false;
    }
    return true;
}

}  // namespace

struct Renderer::Impl {
    explicit Impl(ID3D11Device* dev) noexcept : device(dev) {}

    ID3D11Device* device = nullptr;

    mutable std::mutex       mtx;
    std::condition_variable_any cv;

    struct Command {
        enum class Kind { Open, Close, Authenticate };
        Kind          kind = Kind::Close;
        std::wstring  path;
        std::string   password;
        std::uint64_t id = 0;
    };

    std::optional<Command>  cmd;
    std::vector<RenderWant> wants;
    std::vector<RenderWant> last_wants;  // 已投递的最后一组请求，用于去重
    bool                    wants_dirty = false;
    std::uint64_t           next_id = 0;

    DocState doc;

    // 页缓存条目：同时承载"发布给 UI 的快照"与"缓存策略所需的元数据"。
    struct Entry {
        PageSlot      slot;                  // 发布给 UI（值拷贝，UI 只读）
        std::size_t   bytes = 0;             // 纹理字节数；0 = 未驻留
        std::uint64_t last_use = 0;          // LRU 序号（越大越新）
        int           auto_retries = 0;      // 本轮失败已消耗的自动重试次数
        bool          manual_retry = false;  // UI 请求重试（点击失败占位）
        float         failed_scale = -1.0f;  // 定格失败时的倍率（用于识别"新请求"）
        bool          stale = false;         // 视图变换已变、本纹理待重渲（仍可显示，不闪白）
    };

    std::vector<Entry>  pages;
    std::size_t         used_bytes = 0;                       // 已驻留纹理字节数
    std::size_t         budget_bytes = kCacheBudgetDefault;   // 字节预算
    std::uint64_t       use_tick = 0;                         // LRU 时钟
    int                 evictions = 0;                        // 累计逐出页数

    // ---- 视图变换（Phase 5）----
    // 全局（文档级）的旋转与配色：渲染每一页时传给 Document::render_page。
    int rotation_ = 0;      // 0/90/180/270
    int scheme_ = 0;        // 见 document.ixx 的 PageScheme

    // ---- 缩略图通道（Phase 5）----
    // 与页缓存分开存储、独立字节记账；不参与页缓存 LRU/预算。
    std::vector<Entry>   thumbs;
    std::size_t          thumb_bytes = 0;
    std::vector<int>     thumb_pages;       // 本帧需要的缩略图页
    std::vector<int>     last_thumb_pages;  // 去重用
    int                  thumb_target_px = 150;
    int                  last_thumb_px = -1;
    bool                 thumbs_dirty = false;

    // 文档信息副本：缩略图需按"旋转后"的页尺寸算目标倍率（页纹理不需要，倍率由 UI 给定）。
    DocumentInfo info_;

    // 目录（outline）快照：打开/解锁成功后一次性加载，供 UI 取用。
    std::vector<OutlineItem> outline_;

    // 两段式退役队列（见文件头）
    std::vector<void*>  retired_pending;
    std::vector<void*>  retired_ready;

    // 内容指纹（ADR-062）：**工作线程**在打开前算好（≤320KB 采样读），UI 线程不读文件内容。
    // 加密文档打开会失败在 NeedsPassword，指纹要留给随后 authenticate 成功的那次发布，故存一份。
    std::uint64_t fp_ = 0;

    Document     doc_engine;  // 仅工作线程访问
    std::jthread worker;

    void start() { worker = std::jthread([this](std::stop_token st) { run(std::move(st)); }); }

    void shutdown() {
        worker.request_stop();
        cv.notify_all();
        if (worker.joinable()) worker.join();
        std::lock_guard lock(mtx);
        for (Entry& e : pages) release_srv(e.slot.texture);
        pages.clear();
        for (Entry& e : thumbs) release_srv(e.slot.texture);
        thumbs.clear();
        for (void* p : retired_pending) release_srv(p);
        for (void* p : retired_ready) release_srv(p);
        retired_pending.clear();
        retired_ready.clear();
        used_bytes = 0;
        thumb_bytes = 0;
    }

    static void release_srv(void* p) {
        if (p) static_cast<ID3D11ShaderResourceView*>(p)->Release();
    }

    // ---- 加锁访问辅助（以下 *_locked 均要求调用者持 mtx） ----

    // 把一页的纹理退役（入 pending）并从驻留统计扣除。槽位的状态/错误保留，
    // 由调用方决定后续语义（逐出置 Unloaded / 替换置 Loaded）。
    void retire_entry_locked(Entry& e) {
        if (e.slot.texture) {
            retired_pending.push_back(e.slot.texture);
            e.slot.texture = nullptr;
        }
        if (e.bytes) {
            used_bytes = used_bytes >= e.bytes ? used_bytes - e.bytes : 0;
            e.bytes = 0;
        }
        e.slot.pixel_w = 0;
        e.slot.pixel_h = 0;
        e.slot.scale = 0.0f;
    }

    void clear_pages_locked() {
        for (Entry& e : pages) retire_entry_locked(e);
        pages.clear();
        used_bytes = 0;
        evictions = 0;
    }

    // 缩略图纹理退役（扣减 thumb_bytes 而不是页缓存 used_bytes）
    void retire_thumb_locked(Entry& e) {
        if (e.slot.texture) {
            retired_pending.push_back(e.slot.texture);
            e.slot.texture = nullptr;
        }
        if (e.bytes) {
            thumb_bytes = thumb_bytes >= e.bytes ? thumb_bytes - e.bytes : 0;
            e.bytes = 0;
        }
        e.slot.pixel_w = 0;
        e.slot.pixel_h = 0;
        e.slot.scale = 0.0f;
    }

    void clear_thumbs_locked() {
        for (Entry& e : thumbs) retire_thumb_locked(e);
        thumbs.clear();
        thumb_bytes = 0;
        last_thumb_pages.clear();
        last_thumb_px = -1;
    }

    // 视图变换变化（旋转/配色）时调用：全部页纹理与缩略图作废并重新排队。
    // 变换结果已固化进纹理像素，无法在原纹理上"改属性"，必须整篇重渲。
    void invalidate_all_locked() {
        for (Entry& e : pages) {
            retire_entry_locked(e);
            e.slot.status = PageStatus::Unloaded;
            e.slot.error = DocError::Ok;
            e.auto_retries = 0;
            e.failed_scale = -1.0f;
            e.stale = false;
        }
        for (Entry& e : thumbs) {
            retire_thumb_locked(e);
            e.slot.status = PageStatus::Unloaded;
            e.slot.error = DocError::Ok;
            e.stale = false;
        }
        last_wants.clear();
        last_thumb_pages.clear();
        last_thumb_px = -1;
    }

    // 仅配色变化时调用（旋转会改版面，仍走 invalidate_all_locked）：
    // **保留已驻留纹理继续显示**，只把它们标记为 stale 让下一轮调度重渲；
    // 新纹理就绪后由 publish_loaded 原地换入 —— 全程无白屏（ADR-024 精神、ADR-042）。
    void mark_all_stale_locked() {
        for (Entry& e : pages) {
            e.stale = true;
            e.auto_retries = 0;    // 给重渲一次全新机会（含此前失败的页）
            e.failed_scale = -1.0f;
        }
        for (Entry& e : thumbs) e.stale = true;
    }

    void publish_doc(const DocState& s) {
        std::lock_guard lock(mtx);
        doc = s;
    }

    PageSlot slot_snapshot(int page) const {
        std::lock_guard lock(mtx);
        if (page >= 0 && static_cast<std::size_t>(page) < pages.size())
            return pages[static_cast<std::size_t>(page)].slot;
        return {};
    }

    // ---- 工作线程主循环 ----

    void run(std::stop_token st) {
        for (;;) {
            std::optional<Command>  c;
            std::vector<RenderWant> w;
            std::vector<int>        tw;
            bool have_wants = false;
            bool have_thumbs = false;
            int  thumb_px = 150;
            {
                std::unique_lock lock(mtx);
                cv.wait(lock, st, [this] { return cmd.has_value() || wants_dirty || thumbs_dirty; });
                if (st.stop_requested()) return;
                if (cmd) {                      // 命令优先，渲染请求留到下一轮
                    c = std::move(cmd);
                    cmd.reset();
                } else if (wants_dirty) {
                    w = std::move(wants);
                    wants.clear();
                    wants_dirty = false;
                    have_wants = true;
                } else if (thumbs_dirty) {
                    tw = thumb_pages;
                    thumb_px = thumb_target_px;
                    thumbs_dirty = false;
                    have_thumbs = true;
                }
            }
            if (c) handle_command(*c);
            else if (have_wants) handle_wants(w);
            else if (have_thumbs) handle_thumbs(tw, thumb_px);
        }
    }

    void handle_command(const Command& c) {
        switch (c.kind) {
        case Command::Kind::Open:         do_open(c); break;
        case Command::Kind::Close:        do_close(c); break;
        case Command::Kind::Authenticate: do_auth(c); break;
        }
    }

    void do_open(const Command& c) {
        doc_engine.close();
        {
            std::lock_guard lock(mtx);
            clear_pages_locked();
            clear_thumbs_locked();
            last_wants.clear();
        }

        DocState s;
        s.id = c.id;
        fp_ = lr::file_fingerprint(c.path);   // 工作线程：不占 UI 时间（ADR-009）
        s.file_fingerprint = fp_;
        const DocError err = doc_engine.open(c.path);
        if (err == DocError::Ok) {
            DocumentInfo info;
            const DocError ierr = doc_engine.info(info);
            if (ierr == DocError::Ok) {
                s.phase = DocPhase::Ready;
                s.info = info;
                info_ = info;  // 缩略图按页尺寸算倍率用（工作线程私有，无需加锁）
                std::vector<OutlineItem> ol;
                doc_engine.outline(ol);  // 锁外加载（可能触及页树），避免长时间持锁
                std::lock_guard lock(mtx);
                const std::size_t n =
                    static_cast<std::size_t>(info.page_count > 0 ? info.page_count : 0);
                pages.assign(n, Entry{});
                thumbs.assign(n, Entry{});
                outline_ = std::move(ol);
            } else {
                s.phase = DocPhase::Failed;
                s.error = ierr;
                s.detail_u8.assign(doc_engine.last_error());
                doc_engine.close();
            }
        } else {
            s.phase = DocPhase::Failed;
            s.error = err;
            s.detail_u8.assign(doc_engine.last_error());
        }
        publish_doc(s);
    }

    void do_close(const Command& c) {
        doc_engine.close();
        DocState s;
        s.phase = DocPhase::Idle;
        s.id = c.id;
        std::lock_guard lock(mtx);
        clear_pages_locked();
        clear_thumbs_locked();
        last_wants.clear();
        info_ = DocumentInfo{};
        outline_.clear();
        fp_ = 0;
        doc = s;
    }

    void do_auth(const Command& c) {
        DocState s;
        s.id = c.id;
        s.file_fingerprint = fp_;   // 解锁成功时沿用打开时算的指纹（密码错误也不重算）
        const DocError err = doc_engine.authenticate(c.password);
        if (err == DocError::Ok) {
            DocumentInfo info;
            const DocError ierr = doc_engine.info(info);
            if (ierr == DocError::Ok) {
                s.phase = DocPhase::Ready;
                s.info = info;
                info_ = info;
                std::vector<OutlineItem> ol;
                doc_engine.outline(ol);
                std::lock_guard lock(mtx);
                clear_pages_locked();
                clear_thumbs_locked();
                const std::size_t n =
                    static_cast<std::size_t>(info.page_count > 0 ? info.page_count : 0);
                pages.assign(n, Entry{});
                thumbs.assign(n, Entry{});
                outline_ = std::move(ol);
                last_wants.clear();
            } else {
                s.phase = DocPhase::Failed;
                s.error = ierr;
                s.detail_u8.assign(doc_engine.last_error());
            }
        } else {
            s.phase = DocPhase::Failed;
            s.error = err;
            s.detail_u8.assign(doc_engine.last_error());
        }
        publish_doc(s);
    }

    // ---- 渲染请求处理 ----

    void handle_wants(const std::vector<RenderWant>& w) {
        if (!doc_engine.is_open()) return;

        // 1) 标记本帧需要的页（pinned，不可逐出）并刷新其 LRU 序号
        std::vector<char> wanted;
        {
            std::lock_guard lock(mtx);
            wanted.assign(pages.size(), 0);
            ++use_tick;
            for (const RenderWant& x : w) {
                if (x.page < 0 || static_cast<std::size_t>(x.page) >= pages.size()) continue;
                wanted[static_cast<std::size_t>(x.page)] = 1;
                pages[static_cast<std::size_t>(x.page)].last_use = use_tick;
            }
        }

        // 2) 按字节预算逐出（纯策略：lilithreader.page_cache::select_evictions）
        evict_to_budget(wanted);

        // 3) 渲染缺失/不够清晰/已因视图变换而 stale 的页（按 wants 顺序，可见页在前）
        for (const RenderWant& x : w) {
            if (x.page < 0 || !(x.scale > 0.0f)) continue;
            if (static_cast<std::size_t>(x.page) >= pages.size()) continue;
            PageSlot cur;
            bool stale = false;
            {
                std::lock_guard lock(mtx);
                cur = pages[static_cast<std::size_t>(x.page)].slot;
                stale = pages[static_cast<std::size_t>(x.page)].stale;
            }
            if (!stale && cur.status == PageStatus::Loaded && cur.scale >= x.scale * 0.999f)
                continue;  // 已够清晰且未过期
            render_one(x.page, x.scale);
        }
    }

    void evict_to_budget(const std::vector<char>& wanted) {
        std::vector<CachePageView> views;
        std::size_t used = 0, budget = 0;
        {
            std::lock_guard lock(mtx);
            views.resize(pages.size());
            for (std::size_t i = 0; i < pages.size(); ++i) {
                CachePageView v;
                v.bytes = pages[i].bytes;
                v.last_use = pages[i].last_use;
                v.pinned = i < wanted.size() && wanted[i] != 0;
                views[i] = v;
            }
            used = used_bytes;
            budget = budget_bytes;
        }
        const std::vector<int> victims = select_evictions(views, used, budget);
        if (victims.empty()) return;
        std::lock_guard lock(mtx);
        for (int i : victims) {
            if (i < 0 || static_cast<std::size_t>(i) >= pages.size()) continue;
            Entry& e = pages[static_cast<std::size_t>(i)];
            if (e.bytes == 0) continue;  // 已被处理（防御）
            retire_entry_locked(e);
            e.slot.status = PageStatus::Unloaded;
            e.slot.error = DocError::Ok;
            e.auto_retries = 0;          // 逐出即"从未渲染"，下次请求给全新额度
            e.failed_scale = -1.0f;
            e.stale = false;
            ++evictions;
        }
    }

    // 渲染单页。失败时自动重试至多 kMaxAutoRetries 次，仍失败则定格为 Failed
    // （等待 UI 点击重试，见 retry_page）。加载/重试期间**保留旧纹理**（ADR-024）。
    void render_one(int page, float scale) {
        {
            std::lock_guard lock(mtx);
            if (page < 0 || static_cast<std::size_t>(page) >= pages.size()) return;
            Entry& e = pages[static_cast<std::size_t>(page)];
            const bool manual = e.manual_retry;
            if (e.slot.status == PageStatus::Failed && !manual) {
                // 同一倍率的失败已定格（避免随 wants 变化无限重渲）；
                // 倍率变化视为**新请求**，重新给自动重试额度 —— 否则页会永久卡在失败时的
                // 旧倍率上（曾成功渲染过时旧纹理仍在，永远等不到高清换入）。
                const bool same_scale = std::fabs(e.failed_scale - scale) <= 0.002f;
                if (same_scale && !should_render_failed(false, e.auto_retries)) return;
                if (!same_scale) e.auto_retries = 0;
            }
            if (manual) {
                e.manual_retry = false;
                e.auto_retries = 0;  // 手动重试重置额度
            }
            e.stale = false;         // 本次已开始重渲：不再视为"待重渲"
            // 只改状态，绝不动 texture/scale —— 重渲染期间 UI 继续显示旧纹理，不闪白。
            e.slot.status = PageStatus::Loading;
            e.slot.error = DocError::Ok;
        }

        for (;;) {
            PageBitmap bmp;
            DocError err = doc_engine.render_page(page, scale, bmp, 8192, rotation_,
                                                  static_cast<PageScheme>(scheme_));

            ID3D11ShaderResourceView* srv = nullptr;
            if (err == DocError::Ok &&
                !create_page_texture(device, bmp.samples(), bmp.width(), bmp.height(),
                                     bmp.stride(), &srv)) {
                err = DocError::Internal;
            }
            if (err == DocError::Ok) {
                publish_loaded(page, srv, bmp);
                return;
            }
            if (srv) { srv->Release(); srv = nullptr; }

            // 失败：还有自动重试额度吗？
            bool again = false;
            {
                std::lock_guard lock(mtx);
                if (page >= 0 && static_cast<std::size_t>(page) < pages.size()) {
                    Entry& e = pages[static_cast<std::size_t>(page)];
                    if (e.auto_retries < kMaxAutoRetries) {
                        ++e.auto_retries;
                        again = true;
                    }
                }
            }
            if (!again) { mark_failed(page, err, scale); return; }
        }
    }

    void publish_loaded(int page, ID3D11ShaderResourceView* srv, const PageBitmap& bmp) {
        const std::size_t bytes =
            static_cast<std::size_t>(bmp.width()) * static_cast<std::size_t>(bmp.height()) * 4u;

        PageSlot loaded;
        loaded.status = PageStatus::Loaded;
        loaded.texture = srv;
        loaded.pixel_w = bmp.width();
        loaded.pixel_h = bmp.height();
        loaded.scale = bmp.effective_scale();

        std::lock_guard lock(mtx);
        if (page < 0 || static_cast<std::size_t>(page) >= pages.size()) {
            retired_pending.push_back(srv);  // 文档已切换，结果作废
            return;
        }
        Entry& e = pages[static_cast<std::size_t>(page)];
        retire_entry_locked(e);  // 旧纹理入退役队列（由 UI 帧首释放）
        e.slot = loaded;
        e.bytes = bytes;
        e.auto_retries = 0;
        e.manual_retry = false;
        e.failed_scale = -1.0f;
        e.stale = false;
        used_bytes += bytes;
    }

    void mark_failed(int page, DocError err, float scale) {
        std::lock_guard lock(mtx);
        if (page < 0 || static_cast<std::size_t>(page) >= pages.size()) return;
        Entry& e = pages[static_cast<std::size_t>(page)];
        e.slot.status = PageStatus::Failed;
        e.slot.error = err;
        e.failed_scale = scale;
        // 保留已有纹理（ADR-024）：宁可继续显示旧图，也不闪回失败占位。
    }

    // ---- 缩略图渲染（Phase 5）----

    void handle_thumbs(const std::vector<int>& list, int target_px) {
        if (!doc_engine.is_open()) return;
        if (target_px < 32) target_px = 32;
        if (target_px > 1024) target_px = 1024;
        for (int p : list) {
            if (p < 0 || static_cast<std::size_t>(p) >= thumbs.size()) continue;
            PageSlot cur;
            bool stale = false;
            {
                std::lock_guard lock(mtx);
                cur = thumbs[static_cast<std::size_t>(p)].slot;
                stale = thumbs[static_cast<std::size_t>(p)].stale;
            }
            if (!stale && cur.status == PageStatus::Loaded && cur.scale > 0.0f) continue;
            render_thumb(p, target_px);
        }
    }

    // 按"旋转后"页尺寸算目标倍率，使最长边 ≈ target_px。
    // 缩略图失败**不自动重试**（侧栏是辅助视图，失败就留空占位）；
    // 视图变换变化或重新打开文档会重置状态、自然重试。
    void render_thumb(int page, int target_px) {
        float w = info_.page_width_pt;
        float h = info_.page_height_pt;
        if (page >= 0 && static_cast<std::size_t>(page) < info_.page_sizes.size()) {
            const PageSize& ps = info_.page_sizes[static_cast<std::size_t>(page)];
            if (ps.width_pt > 0.0f && ps.height_pt > 0.0f) { w = ps.width_pt; h = ps.height_pt; }
        }
        if (!(w > 0.0f) || !(h > 0.0f)) { w = 595.0f; h = 842.0f; }
        if (rotation_ == 90 || rotation_ == 270) { const float t = w; w = h; h = t; }
        const float longest = w > h ? w : h;
        float scale = longest > 0.0f ? static_cast<float>(target_px) / longest : 1.0f;
        if (!(scale > 0.0f)) scale = 1.0f;

        {
            std::lock_guard lock(mtx);
            if (page < 0 || static_cast<std::size_t>(page) >= thumbs.size()) return;
            Entry& e = thumbs[static_cast<std::size_t>(page)];
            if (e.slot.status == PageStatus::Failed) return;  // 已失败，不再重试
            e.slot.status = PageStatus::Loading;
            e.stale = false;
        }

        PageBitmap bmp;
        DocError err = doc_engine.render_page(page, scale, bmp, 4096, rotation_,
                                              static_cast<PageScheme>(scheme_));
        ID3D11ShaderResourceView* srv = nullptr;
        if (err == DocError::Ok &&
            !create_page_texture(device, bmp.samples(), bmp.width(), bmp.height(),
                                 bmp.stride(), &srv)) {
            err = DocError::Internal;
        }

        std::lock_guard lock(mtx);
        if (page < 0 || static_cast<std::size_t>(page) >= thumbs.size()) {
            if (srv) srv->Release();
            return;
        }
        Entry& e = thumbs[static_cast<std::size_t>(page)];
        if (err != DocError::Ok) {
            if (srv) srv->Release();
            e.slot.status = PageStatus::Failed;
            e.slot.error = err;
            return;
        }
        retire_thumb_locked(e);
        e.slot.status = PageStatus::Loaded;
        e.slot.texture = srv;
        e.slot.pixel_w = bmp.width();
        e.slot.pixel_h = bmp.height();
        e.slot.scale = bmp.effective_scale();
        e.slot.error = DocError::Ok;
        e.bytes = static_cast<std::size_t>(bmp.width()) *
                  static_cast<std::size_t>(bmp.height()) * 4u;
        thumb_bytes += e.bytes;
    }
};

// ---- Renderer 对外接口 ----

Renderer::Renderer(void* device) noexcept
    : impl_(new (std::nothrow) Impl(static_cast<ID3D11Device*>(device))) {
    if (impl_) impl_->start();
}

Renderer::~Renderer() {
    if (impl_) impl_->shutdown();
}

std::uint64_t Renderer::open(std::wstring path) {
    if (!impl_) return 0;
    std::lock_guard lock(impl_->mtx);
    const std::uint64_t id = ++impl_->next_id;
    Impl::Command c;
    c.kind = Impl::Command::Kind::Open;
    c.path = std::move(path);
    c.id = id;
    impl_->cmd = std::move(c);
    impl_->doc.phase = DocPhase::Opening;
    impl_->doc.error = DocError::Ok;
    impl_->doc.detail_u8.clear();
    impl_->doc.id = id;
    impl_->cv.notify_all();
    return id;
}

std::uint64_t Renderer::close() {
    if (!impl_) return 0;
    std::lock_guard lock(impl_->mtx);
    const std::uint64_t id = ++impl_->next_id;
    Impl::Command c;
    c.kind = Impl::Command::Kind::Close;
    c.id = id;
    impl_->cmd = std::move(c);
    impl_->doc = DocState{};
    impl_->doc.phase = DocPhase::Idle;
    impl_->doc.id = id;
    impl_->cv.notify_all();
    return id;
}

std::uint64_t Renderer::authenticate(std::string_view utf8_password) {
    if (!impl_) return 0;
    std::lock_guard lock(impl_->mtx);
    const std::uint64_t id = ++impl_->next_id;
    Impl::Command c;
    c.kind = Impl::Command::Kind::Authenticate;
    c.password.assign(utf8_password);
    c.id = id;
    impl_->cmd = std::move(c);
    // 与 open() 一致地把 phase 置为 Opening：否则 UI 会在工作线程真正处理之前，
    // 读到上一次 open 失败时残留的 Failed，把"正在认证"误判成结果（时序竞态）。
    impl_->doc.phase = DocPhase::Opening;
    impl_->doc.error = DocError::Ok;
    impl_->doc.detail_u8.clear();
    impl_->doc.id = id;
    impl_->cv.notify_all();
    return id;
}

DocState Renderer::doc_state() const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->mtx);
    return impl_->doc;
}

PageSlot Renderer::slot(int page) const {
    if (!impl_) return {};
    return impl_->slot_snapshot(page);
}

std::vector<OutlineItem> Renderer::outline() const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->mtx);
    return impl_->outline_;
}

void Renderer::set_wanted(std::vector<RenderWant> wants) {
    if (!impl_) return;
    std::lock_guard lock(impl_->mtx);
    if (wants_equal(impl_->last_wants, wants)) return;  // 与上次相同，避免空转唤醒
    impl_->last_wants = wants;
    impl_->wants = std::move(wants);
    impl_->wants_dirty = true;
    impl_->cv.notify_all();
}

void Renderer::drain_retired() {
    if (!impl_) return;
    std::vector<void*> local;
    {
        std::lock_guard lock(impl_->mtx);
        local.swap(impl_->retired_ready);       // 本帧释放"上一帧之前"退役的
        impl_->retired_ready.swap(impl_->retired_pending);
        impl_->retired_pending.clear();
    }
    for (void* p : local) Impl::release_srv(p);
}

void Renderer::set_cache_budget(std::size_t bytes) {
    if (!impl_) return;
    std::lock_guard lock(impl_->mtx);
    impl_->budget_bytes = clamp_cache_budget(bytes);
    // 预算调小可能需立即逐出：用上一帧的请求重跑一次调度（无请求时无可逐出页）
    if (!impl_->last_wants.empty()) {
        impl_->wants = impl_->last_wants;
        impl_->wants_dirty = true;
        impl_->cv.notify_all();
    }
}

CacheStats Renderer::cache_stats() const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->mtx);
    CacheStats s;
    s.budget_bytes = impl_->budget_bytes;
    s.used_bytes = impl_->used_bytes;
    s.evictions = impl_->evictions;
    for (const Impl::Entry& e : impl_->pages)
        if (e.bytes > 0) ++s.resident_pages;
    return s;
}

void Renderer::retry_page(int page) {
    if (!impl_) return;
    std::lock_guard lock(impl_->mtx);
    if (page < 0 || static_cast<std::size_t>(page) >= impl_->pages.size()) return;
    Impl::Entry& e = impl_->pages[static_cast<std::size_t>(page)];
    if (e.manual_retry) return;  // 已在待重试队列
    e.manual_retry = true;
    e.auto_retries = 0;
    // 重新武装一次调度：否则 set_wanted 的去重会拦住"同参数"的再次投递
    impl_->wants = impl_->last_wants;
    impl_->wants_dirty = true;
    impl_->cv.notify_all();
}

// ---- 视图变换（Phase 5）----

void Renderer::set_view_transform(int rotation_deg, PageScheme scheme) {
    if (!impl_) return;
    int rot = rotation_deg % 360;
    if (rot < 0) rot += 360;
    rot = (rot / 90) * 90;
    const int mode = static_cast<int>(scheme);
    if (mode < 0 || mode > 2) return;  // 非法配色忽略

    std::lock_guard lock(impl_->mtx);
    if (impl_->rotation_ == rot && impl_->scheme_ == mode) return;
    const bool rot_changed = (impl_->rotation_ != rot);
    impl_->rotation_ = rot;
    impl_->scheme_ = mode;

    // 保留本帧请求，作废/标记全部纹理后原样重新投递（触发整篇重渲）。
    const std::vector<RenderWant> keep_wants = impl_->last_wants;
    const std::vector<int>        keep_thumbs = impl_->last_thumb_pages;
    const int                     keep_px = impl_->last_thumb_px;

    if (rot_changed) {
        // 旋转会交换版面宽高：旧纹理贴进新框会拉伸错位，只能作废重渲（页码/滚动已在 UI 侧重定位）。
        impl_->invalidate_all_locked();
        impl_->last_wants = keep_wants;
        impl_->last_thumb_pages = keep_thumbs;
        impl_->last_thumb_px = keep_px;
    } else {
        // 仅配色变化：版面不变，**保留旧纹理继续显示**，只标记待重渲，避免整篇瞬间白屏（ADR-042）。
        impl_->mark_all_stale_locked();
        // last_wants / last_thumb_pages 保持原值：它们仍是"已投递"记录，去重语义不变。
    }
    impl_->wants = keep_wants;
    impl_->wants_dirty = !keep_wants.empty();
    impl_->thumb_pages = keep_thumbs;
    impl_->thumb_target_px = keep_px > 0 ? keep_px : impl_->thumb_target_px;
    impl_->thumbs_dirty = !keep_thumbs.empty();
    impl_->cv.notify_all();
}

// ---- 缩略图通道（Phase 5）----

void Renderer::set_thumbs_wanted(std::vector<int> pages, int target_px) {
    if (!impl_) return;
    if (target_px < 32) target_px = 32;
    if (target_px > 1024) target_px = 1024;
    std::lock_guard lock(impl_->mtx);
    if (impl_->last_thumb_px == target_px && impl_->last_thumb_pages == pages) return;
    impl_->last_thumb_pages = pages;      // 先拷贝再移动
    impl_->last_thumb_px = target_px;
    impl_->thumb_pages = std::move(pages);
    impl_->thumb_target_px = target_px;
    impl_->thumbs_dirty = true;

    // 退役**请求范围之外**的缩略图：否则长时间滚动后缩略图会无限累积
    // （每张约 100KB，1000 页即上百 MB），与"内存有界"的纪律相悖。
    // 侧栏请求的是一段连续区间，取其 [min,max] 即可。
    if (!impl_->thumb_pages.empty()) {
        int lo = impl_->thumb_pages.front();
        int hi = impl_->thumb_pages.front();
        for (int p : impl_->thumb_pages) {
            if (p < lo) lo = p;
            if (p > hi) hi = p;
        }
        for (int i = 0; i < static_cast<int>(impl_->thumbs.size()); ++i) {
            if (i >= lo && i <= hi) continue;
            Impl::Entry& e = impl_->thumbs[static_cast<std::size_t>(i)];
            if (e.bytes == 0 && e.slot.texture == nullptr) continue;
            impl_->retire_thumb_locked(e);
            e.slot.status = PageStatus::Unloaded;
            e.slot.error = DocError::Ok;
        }
    }
    impl_->cv.notify_all();
}

PageSlot Renderer::thumb_slot(int page) const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->mtx);
    if (page >= 0 && static_cast<std::size_t>(page) < impl_->thumbs.size())
        return impl_->thumbs[static_cast<std::size_t>(page)].slot;
    return {};
}

}  // namespace lr
