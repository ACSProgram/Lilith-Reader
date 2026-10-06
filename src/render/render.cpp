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
#include <deque>
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

ResourceProfile resource_profile(ResourceTier tier) noexcept {
    switch (tier) {
    case ResourceTier::Low:
        return { ResourceTier::Low, 1536, 256u << 20 };
    case ResourceTier::High:
        return { ResourceTier::High, 2560, 768u << 20 };
    case ResourceTier::Balanced:
    default:
        return { ResourceTier::Balanced, 2048, kCacheBudgetDefault };
    }
}

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

// 全文搜索的**全局**命中上限：单页上限（document 层 200）之上再加一道总闸，
// 防止"文档里全是 e"这类查询把结果表撑到几十万条（列表/内存都受不了）。
// 触顶即标记 truncated 并停止检索，UI 会如实告知"仅显示前 N 处"。
constexpr int kMaxSearchHits = 5000;

// 每轮检索的页数。取值权衡：太小 → 线程唤醒次数多、调度开销占比高；
// 太大 → 单轮占用工作线程过久，期间渲染请求要排队（滚动会顿）。
// 24 页约合"人眼刚好察觉不到的一次停顿"，且一批之后必然让位给渲染请求。
constexpr int kSearchBatchPages = 24;

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
    ResourceTier        tier = ResourceTier::Balanced;
    ResourceProfile     profile = lr::resource_profile(ResourceTier::Balanced);

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

    // ---- 辅助请求队列（Phase 8：文本/图片/链接）----
    //
    // 为什么不复用上面的 `cmd` 单槽：`cmd` 是"后到覆盖先到"的语义（open/close/auth 都是
    // 用户显式动作，覆盖上一次待处理是合理的）。而文本请求由鼠标悬停触发、频率高得多，
    // 若共用槽位，一次悬停就能把"待打开的文档"覆盖掉 —— 用户点了打开却没反应。
    // 故另开一条 FIFO 队列，与渲染请求同优先级、互不覆盖。
    struct AuxReq {
        enum class Kind { PageContent, CopyText, CopyImage };
        Kind  kind = Kind::PageContent;
        int   page = -1;
        float ax = 0, ay = 0, bx = 0, by = 0;   // CopyText：选区两端（未旋转页面 pt）
        float px = 0, py = 0;                   // CopyImage：取图点（未旋转页面 pt）
    };
    std::deque<AuxReq> aux;
    bool               aux_dirty = false;

    // 已发布的页内容快照（只保留一页 + "未被取走"标记）。
    // 只保留一页的理由同 document 的 stext 缓存：交互始终集中在鼠标所在的那一页。
    PageContent content_pub;
    int         content_page = -1;
    bool        content_fresh = false;

    // 复制结果：一次性取用（取走即清空）。文本用 optional 区分"空结果"与"无结果"。
    std::optional<std::string> copy_text_res;
    std::optional<ImageData>   copy_image_res;

    // ---- 全文搜索作业（Phase 8）----
    struct SearchJob {
        bool          active = false;
        std::string   needle;              // UTF-8
        int           next_page = 0;       // 下一个待扫描页
        int           hits = 0;            // 已累计命中数
        bool          truncated = false;   // 命中触顶，提前收工
        std::uint64_t id = 0;              // 查询序号（在途批次用它判断"是否已被取代"）
        std::vector<SearchHit> pending;    // 待 UI 取走的新命中
    };
    SearchJob     search;
    std::uint64_t next_search_id = 0;

    Document     doc_engine;  // 仅工作线程访问
    std::jthread worker;

    void start() { worker = std::jthread([this](std::stop_token st) { run(std::move(st)); }); }

    void shutdown() {
        worker.request_stop();
        cv.notify_all();
        if (worker.joinable()) worker.join();
        std::lock_guard lock(mtx);
        for (Entry& e : pages) {
            release_srv(e.slot.texture);
            for (const PageSlot::Tile& t : e.slot.tiles) release_srv(t.texture);
        }
        pages.clear();
        for (Entry& e : thumbs) {
            release_srv(e.slot.texture);
            for (const PageSlot::Tile& t : e.slot.tiles) release_srv(t.texture);
        }
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
        for (const PageSlot::Tile& t : e.slot.tiles)
            if (t.texture) retired_pending.push_back(t.texture);
        e.slot.tiles.clear();
        if (e.bytes) {
            used_bytes = used_bytes >= e.bytes ? used_bytes - e.bytes : 0;
            e.bytes = 0;
        }
        e.slot.pixel_w = 0;
        e.slot.pixel_h = 0;
        e.slot.full_pixel_w = 0;
        e.slot.full_pixel_h = 0;
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
        e.slot.full_pixel_w = 0;
        e.slot.full_pixel_h = 0;
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
            std::optional<AuxReq>   a;
            std::vector<RenderWant> w;
            std::vector<int>        tw;
            bool have_wants = false;
            bool have_thumbs = false;
            int  thumb_px = 150;
            {
                std::unique_lock lock(mtx);
                cv.wait(lock, st, [this] {
                    return cmd.has_value() || aux_dirty || wants_dirty || thumbs_dirty ||
                           search.active;
                });
                if (st.stop_requested()) return;
                // 优先级：显式命令 > 辅助请求 > 渲染请求 > 缩略图 > 检索。
                // 辅助请求排在渲染之前：它由用户动作直接触发（悬停/复制），延迟可感知；
                // 且每个请求只处理一页，成本与渲染一页同量级，不会造成长停顿。
                // 检索排在最后：它是长任务，必须让位给渲染（滚动时不卡）。
                if (cmd) {                      // 命令优先，渲染请求留到下一轮
                    c = std::move(cmd);
                    cmd.reset();
                } else if (aux_dirty) {
                    if (!aux.empty()) {
                        a = aux.front();
                        aux.pop_front();
                    }
                    aux_dirty = !aux.empty();
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
            else if (a) handle_aux(*a);
            else if (have_wants) handle_wants(w);
            else if (have_thumbs) handle_thumbs(tw, thumb_px);
            else search_step();
            // 渲染/缩略图之后捎带一轮检索：两者都不忙时才轮到它，天然与渲染交替。
            if (have_wants || have_thumbs) search_step();
        }
    }

    void handle_command(const Command& c) {
        switch (c.kind) {
        case Command::Kind::Open:         do_open(c); break;
        case Command::Kind::Close:        do_close(c); break;
        case Command::Kind::Authenticate: do_auth(c); break;
        }
    }

    // ---- 辅助请求处理（Phase 8）----
    void handle_aux(const AuxReq& a) {
        if (!doc_engine.is_open()) return;
        switch (a.kind) {
        case AuxReq::Kind::PageContent: {
            PageContent pc;
            if (doc_engine.page_content(a.page, pc) != DocError::Ok) return;
            std::lock_guard lock(mtx);
            content_pub = std::move(pc);
            content_page = a.page;
            content_fresh = true;
            break;
        }
        case AuxReq::Kind::CopyText: {
            std::string text;
            // 失败不改变结果槽：UI 会一直等不到结果 —— 但复制失败没有别的补救，
            // 宁可让它"没反应"，也不要写入半截文本（半截更糟：用户以为复制成功）。
            if (doc_engine.copy_text(a.page, a.ax, a.ay, a.bx, a.by, text) != DocError::Ok) return;
            std::lock_guard lock(mtx);
            copy_text_res = std::move(text);
            break;
        }
        case AuxReq::Kind::CopyImage: {
            ImageData img;   // 无图片时保持空 → take_copy_image 返回 true 但 out 无效
            doc_engine.image_at(a.page, a.px, a.py, img);
            std::lock_guard lock(mtx);
            copy_image_res = std::move(img);
            break;
        }
        }
    }

    // ---- 全文检索：一轮 = 一小批页（Phase 8）----
    //
    // 三处让步设计：
    //   1) 每页开始前重读 `search.id` —— 新查询/取消会换 id，在途批次据此立即收工；
    //   2) 每轮只扫 kSearchBatchPages 页 —— 之后必然回到主循环，渲染请求得以及时插队；
    //   3) 结果攒够一批才进 pending —— UI 每帧取一次，不产生逐页的锁竞争。
    void search_step() {
        std::string   needle;
        std::uint64_t id = 0;
        int           page = 0;
        int           hits = 0;
        bool          truncated = false;
        {
            std::lock_guard lock(mtx);
            if (!search.active) return;
            needle = search.needle;
            id = search.id;
            page = search.next_page;
            hits = search.hits;
            truncated = search.truncated;
        }
        if (!doc_engine.is_open() || needle.empty()) {
            std::lock_guard lock(mtx);
            if (search.id == id) search.active = false;
            return;
        }
        const int total = info_.page_count;

        std::vector<SearchHit> found;
        std::vector<SearchHit> page_hits;
        for (int n = 0; n < kSearchBatchPages && page < total; ++n, ++page) {
            {
                // 每页前检查：是否已被新查询取代 / 已取消 / 文档已换
                std::lock_guard lock(mtx);
                if (!search.active || search.id != id) return;
            }
            page_hits.clear();
            if (doc_engine.search_page(page, needle, 200, page_hits) != DocError::Ok) continue;
            for (SearchHit& h : page_hits) {
                if (hits >= kMaxSearchHits) { truncated = true; break; }
                found.push_back(std::move(h));
                ++hits;
            }
            if (truncated) { ++page; break; }
        }

        std::lock_guard lock(mtx);
        if (!search.active || search.id != id) return;
        for (SearchHit& h : found) search.pending.push_back(std::move(h));
        search.hits = hits;
        search.next_page = page;
        search.truncated = truncated;
        if (truncated || page >= total) search.active = false;
    }

    // 取消检索（换文档 / 关文档 / 用户取消时调用）。换 id 让在途批次失效。
    void cancel_search_locked() {
        search.active = false;
        search.needle.clear();
        search.next_page = 0;
        search.hits = 0;
        search.truncated = false;
        search.pending.clear();
        search.id = ++next_search_id;
    }

    void do_open(const Command& c) {
        doc_engine.close();
        {
            std::lock_guard lock(mtx);
            clear_pages_locked();
            clear_thumbs_locked();
            cancel_search_locked();   // 换文档：旧的检索结果一律作废（页号不再对应）
            content_fresh = false;
            content_page = -1;
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
        cancel_search_locked();
        content_fresh = false;
        content_page = -1;
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
                cancel_search_locked();
                content_fresh = false;
                content_page = -1;
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
    struct BuiltTile {
        PageSlot::Tile view;
    };

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

        // 超过档位 tile 单边时，按整页输出像素拆分。每次只在 MuPDF 中保留一个
        // tile pixmap，避免巨型页面先分配完整 RGBA 缓冲；上传仍在本工作线程完成。
        int full_w = 0;
        int full_h = 0;
        if (page >= 0 && static_cast<std::size_t>(page) < info_.page_sizes.size()) {
            const PageSize ps = info_.page_sizes[static_cast<std::size_t>(page)];
            const bool swap = (rotation_ % 180) != 0;
            const double pw = std::max(0.0, static_cast<double>(ps.width_pt) * scale);
            const double ph = std::max(0.0, static_cast<double>(ps.height_pt) * scale);
            const double fw = swap ? ph : pw;
            const double fh = swap ? pw : ph;
            full_w = fw > 2147483000.0 ? 2147483000 : static_cast<int>(std::lround(fw));
            full_h = fh > 2147483000.0 ? 2147483000 : static_cast<int>(std::lround(fh));
        } else if (info_.page_sizes.empty()) {
            const bool swap = (rotation_ % 180) != 0;
            const double pw = std::max(0.0, static_cast<double>(info_.page_width_pt) * scale);
            const double ph = std::max(0.0, static_cast<double>(info_.page_height_pt) * scale);
            full_w = static_cast<int>(std::lround(swap ? ph : pw));
            full_h = static_cast<int>(std::lround(swap ? pw : ph));
        }
        if (full_w > profile.tile_size_px || full_h > profile.tile_size_px) {
            render_one_tiled(page, scale, full_w, full_h);
            return;
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

    void render_one_tiled(int page, float scale, int full_w, int full_h) {
        const int tile_size = std::max(256, profile.tile_size_px);
        if (full_w <= 0 || full_h <= 0) {
            mark_failed(page, DocError::Internal, scale);
            return;
        }
        for (;;) {
            std::vector<BuiltTile> built;
            DocError failure = DocError::Ok;
            for (int y = 0; y < full_h && failure == DocError::Ok; y += tile_size) {
                for (int x = 0; x < full_w; x += tile_size) {
                    TileRect r;
                    r.x = x; r.y = y;
                    r.w = std::min(tile_size, full_w - x);
                    r.h = std::min(tile_size, full_h - y);
                    PageBitmap bmp;
                    const DocError err = doc_engine.render_page_tile(
                        page, scale, r, bmp, tile_size, rotation_,
                        static_cast<PageScheme>(scheme_));
                    if (err != DocError::Ok) { failure = err; break; }
                    ID3D11ShaderResourceView* srv = nullptr;
                    if (!create_page_texture(device, bmp.samples(), bmp.width(), bmp.height(),
                                             bmp.stride(), &srv)) {
                        failure = DocError::Internal;
                        break;
                    }
                    BuiltTile t;
                    t.view.texture = srv;
                    t.view.x = x; t.view.y = y;
                    t.view.w = bmp.width(); t.view.h = bmp.height();
                    built.push_back(std::move(t));
                }
            }
            if (failure == DocError::Ok) {
                publish_loaded_tiles(page, scale, full_w, full_h, std::move(built));
                return;
            }
            for (BuiltTile& t : built) {
                if (t.view.texture) {
                    static_cast<ID3D11ShaderResourceView*>(t.view.texture)->Release();
                    t.view.texture = nullptr;
                }
            }
            bool again = false;
            {
                std::lock_guard lock(mtx);
                if (page >= 0 && static_cast<std::size_t>(page) < pages.size()) {
                    Entry& e = pages[static_cast<std::size_t>(page)];
                    if (e.auto_retries < kMaxAutoRetries) { ++e.auto_retries; again = true; }
                }
            }
            if (!again) { mark_failed(page, failure, scale); return; }
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
        loaded.full_pixel_w = bmp.width();
        loaded.full_pixel_h = bmp.height();
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

    void publish_loaded_tiles(int page, float requested_scale, int full_w, int full_h,
                              std::vector<BuiltTile> built) {
        std::size_t bytes = 0;
        PageSlot loaded;
        loaded.status = PageStatus::Loaded;
        loaded.scale = requested_scale;
        loaded.full_pixel_w = full_w;
        loaded.full_pixel_h = full_h;
        loaded.tiles.reserve(built.size());
        for (BuiltTile& t : built) {
            bytes += static_cast<std::size_t>(t.view.w) * static_cast<std::size_t>(t.view.h) * 4u;
            loaded.tiles.push_back(t.view);
            t.view.texture = nullptr; // ownership moved into the published slot
        }
        if (!loaded.tiles.empty()) {
            loaded.pixel_w = loaded.tiles.front().w;
            loaded.pixel_h = loaded.tiles.front().h;
        }
        std::lock_guard lock(mtx);
        if (page < 0 || static_cast<std::size_t>(page) >= pages.size()) {
            for (const PageSlot::Tile& t : loaded.tiles) if (t.texture) retired_pending.push_back(t.texture);
            return;
        }
        Entry& e = pages[static_cast<std::size_t>(page)];
        retire_entry_locked(e);
        e.slot = std::move(loaded);
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

void Renderer::set_resource_tier(ResourceTier tier) {
    if (!impl_) return;
    const ResourceProfile p = lr::resource_profile(tier);
    std::lock_guard lock(impl_->mtx);
    impl_->tier = tier;
    impl_->profile = p;
    impl_->budget_bytes = clamp_cache_budget(p.cache_bytes);
    if (!impl_->last_wants.empty()) {
        impl_->wants = impl_->last_wants;
        impl_->wants_dirty = true;
        impl_->cv.notify_all();
    }
}

ResourceTier Renderer::resource_tier() const {
    if (!impl_) return ResourceTier::Balanced;
    std::lock_guard lock(impl_->mtx);
    return impl_->tier;
}

ResourceProfile Renderer::resource_profile() const {
    if (!impl_) return lr::resource_profile(ResourceTier::Balanced);
    std::lock_guard lock(impl_->mtx);
    return impl_->profile;
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

// ---- 文本 / 图片 / 链接（Phase 8）----

void Renderer::request_page_content(int page) {
    if (!impl_ || page < 0) return;
    std::lock_guard lock(impl_->mtx);
    for (const Impl::AuxReq& a : impl_->aux)
        if (a.kind == Impl::AuxReq::Kind::PageContent && a.page == page) return;  // 已在队列
    Impl::AuxReq r;
    r.kind = Impl::AuxReq::Kind::PageContent;
    r.page = page;
    impl_->aux.push_back(r);
    impl_->aux_dirty = true;
    impl_->cv.notify_all();
}

bool Renderer::take_page_content(int page, PageContent& out) {
    if (!impl_) return false;
    std::lock_guard lock(impl_->mtx);
    if (!impl_->content_fresh || impl_->content_page != page) return false;
    out = std::move(impl_->content_pub);
    impl_->content_pub = PageContent{};
    impl_->content_fresh = false;
    return true;
}

void Renderer::request_copy_text(int page, float ax, float ay, float bx, float by) {
    if (!impl_ || page < 0) return;
    std::lock_guard lock(impl_->mtx);
    Impl::AuxReq r;
    r.kind = Impl::AuxReq::Kind::CopyText;
    r.page = page;
    r.ax = ax; r.ay = ay;
    r.bx = bx; r.by = by;
    impl_->aux.push_back(r);
    impl_->aux_dirty = true;
    impl_->cv.notify_all();
}

bool Renderer::take_copy_text(std::string& out) {
    if (!impl_) return false;
    std::lock_guard lock(impl_->mtx);
    if (!impl_->copy_text_res.has_value()) return false;
    out = std::move(*impl_->copy_text_res);
    impl_->copy_text_res.reset();
    return true;
}

void Renderer::request_copy_image(int page, float x, float y) {
    if (!impl_ || page < 0) return;
    std::lock_guard lock(impl_->mtx);
    Impl::AuxReq r;
    r.kind = Impl::AuxReq::Kind::CopyImage;
    r.page = page;
    r.px = x; r.py = y;
    impl_->aux.push_back(r);
    impl_->aux_dirty = true;
    impl_->cv.notify_all();
}

bool Renderer::take_copy_image(ImageData& out) {
    if (!impl_) return false;
    std::lock_guard lock(impl_->mtx);
    if (!impl_->copy_image_res.has_value()) return false;
    out = std::move(*impl_->copy_image_res);
    impl_->copy_image_res.reset();
    return true;
}

// ---- 全文搜索（Phase 8）----

void Renderer::start_search(std::string utf8_needle) {
    if (!impl_) return;
    std::lock_guard lock(impl_->mtx);
    impl_->cancel_search_locked();     // 旧查询立即作废（换 id，在途批次收工）
    // 只挡空关键字。**不挡"页表为空"**：那种情况下 search_step 会因为文档未打开而立刻
    // 收敛（active=false），UI 如实显示"没有找到"；若在这里直接返回，active 一直是 false
    // 而 needle 也没记下，故障会变成"按了搜索什么都没发生"，无从排查。
    if (utf8_needle.empty()) {
        impl_->cv.notify_all();
        return;
    }
    impl_->search.needle = std::move(utf8_needle);
    impl_->search.id = ++impl_->next_search_id;
    impl_->search.next_page = 0;
    impl_->search.active = true;
    impl_->cv.notify_all();
}

void Renderer::cancel_search() {
    if (!impl_) return;
    std::lock_guard lock(impl_->mtx);
    impl_->cancel_search_locked();
    impl_->cv.notify_all();
}

SearchStatus Renderer::search_status() const {
    SearchStatus s;
    if (!impl_) return s;
    std::lock_guard lock(impl_->mtx);
    s.active = impl_->search.active;
    s.scanned = impl_->search.next_page;
    s.total = impl_->doc.info.page_count;   // 走 doc 快照：info_ 是工作线程私有、不加锁
    s.hits = impl_->search.hits;
    s.truncated = impl_->search.truncated;
    s.id = impl_->search.id;
    s.needle = impl_->search.needle;
    return s;
}

void Renderer::take_search_hits(std::vector<SearchHit>& out) {
    if (!impl_) return;
    std::lock_guard lock(impl_->mtx);
    if (impl_->search.pending.empty()) return;
    for (SearchHit& h : impl_->search.pending) out.push_back(std::move(h));
    impl_->search.pending.clear();
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
