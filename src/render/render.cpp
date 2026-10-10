// render.cpp — lilithreader.render 的实现单元
//
// 线程模型（架构文档 §3.1）：
//   · UI 线程：只投递命令/渲染请求、读取快照；零 fz_*、零阻塞。
//   · 工作线程（本文件）：独占 Document；渲染 → 建纹理 → 发布快照。
//   · 纹理生命周期：渲染线程建纹理（ID3D11Device 方法可多线程调用），
//     被替换/逐出的纹理进**两段式退役队列**；UI 线程帧首 drain_retired() 释放。
//
// 内部组件（架构加固，ADR-085）——Impl 过去一个人管文档生命周期、渲染、缩略图、检索、
// 文本抽取、图片复制、LRU 缓存、D3D 纹理所有权、退役队列、线程调度与故障隔离。现拆为：
//   · TextureStore   —— 页/缩略图条目、字节记账、两段式退役队列、LRU 逐出、缓存统计
//                        （唯一持有 D3D 纹理句柄与退役队列的地方）
//   · DocumentWorker —— Document 实例、文档快照、目录、视图变换（旋转/配色），仅工作线程访问
//   · PageScheduler  —— 渲染/缩略图请求的去重与调度、tile 拆分、失败定格、资源档位
//   · ContentService —— 辅助请求队列（文本/图片/链接）与一次性结果槽
//   · SearchJob      —— 全文检索代次、增量扫描、结果发布
//   · Impl           —— 唯一的互斥量/条件变量/工作线程与命令槽，按优先级分派任务
// 组件是**同一实现单元内的结构体**（不设独立模块/子单元：纹理路径自动化覆盖薄，跨 TU 拆分
// 线程与所有权的收益不抵回归风险；可脱离硬件的纯策略已由 page_cache / page_state 独立成模块）。
//
// 页状态机显式化（ADR-085）：Entry 的 status/stale/manual_retry/auto_retries/failed_scale
// 收进 PageState（lilithreader.page_state），以转移函数取代字段组合。TextureStore 持有
// PageState；发布给 UI 的 PageSlot::status 由它派生。
//
// 两段式退役队列（ADR-033）：
//   渲染线程把退役纹理放入 retired_pending（"本帧"）；drain_retired() 先释放
//   retired_ready（"上一帧的 pending"），再把 pending 移入 ready。
//   于是任何纹理都至少**活过一个完整帧**才被 Release。
//
// 本文件不含任何 fz_* 调用（只经 Document/PageBitmap 的公开接口），
// 故不受 ADR-009 的 setjmp 纪律约束。
//
// ---- 稳定性纪律（ADR-078/079）----
//   1. **工作线程入口绝不放异常逃逸**：std::jthread 的入口函数抛出 = std::terminate = 进程死亡。
//      run() 外层包了 try/catch，且各子任务（单页渲染 / 辅助请求 / 检索）**各自**再包一层 ——
//      粒度越细，"一次失败波及的范围"越小。
//   2. **所有 D3D 句柄有明确的所有权**：工作线程构建过程中的临时 SRV 一律用 SrvHandle 持有，
//      失败分支只需"不发布"即可；发布出去的那一刻才 release_raw() 转交所有权。
//      PageSlot::texture 是**不拥有**的视图。
//   3. **尺寸与数量先算后分配**：整页输出像素、tile 个数、页数都有上限，超限返回 TooLarge。

module;

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <exception>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

module lilithreader.render;

import lilithreader.document;
import lilithreader.page_cache;
import lilithreader.page_state;
import lilithreader.utils;   // file_fingerprint（ADR-062）
import lilithreader.log;     // 工作线程异常留痕（ADR-077）

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

// ---- 尺寸/数量上限 ----
//
// 为什么要有这些常量：这些上限原先"隐含"在 D3D 的返回值与分配失败里 ——
// 于是超限的表现是"渲染失败"甚至 bad_alloc，而不是一句明确的 TooLarge。
// 把边界显式化有两个好处：错误码能如实反映原因；乘法运算不会先溢出再当检查。
constexpr int           kMaxTextureDim    = 16384;                  // D3D11 feature level 11 上限
constexpr std::uint64_t kMaxTextureBytes  = 512ull * 1024 * 1024;   // 单张纹理上限（512 MiB）
constexpr std::uint64_t kMaxOutputPixels  = 268435456ull;           // 整页输出像素上限（2^28 ≈ 2.7 亿）
constexpr int           kMaxTiles         = 4096;                   // 单页 tile 个数上限
constexpr int           kMaxPageCount     = 100000;                 // 页数上限（防伪造页数撑爆分配）

// 全文搜索的**全局**命中上限：单页上限（document 层 200）之上再加一道总闸，
// 防止"文档里全是 e"这类查询把结果表撑到几十万条。
constexpr int kMaxSearchHits = 5000;

// 每轮检索的页数。取值权衡：太小 → 线程唤醒次数多、调度开销占比高；
// 太大 → 单轮占用工作线程过久，期间渲染请求要排队（滚动会顿）。
constexpr int kSearchBatchPages = 24;

// ---- D3D 句柄的所有权包装 ----
//
// PageSlot::texture 是**不拥有**的只读视图（UI 只读、绝不释放），于是"谁在什么时刻真正拥有
// 这个 SRV"全靠人工记忆。SrvHandle 把所有权变成类型属性：只要没显式 release_raw() 交出去，
// 析构必定释放。
struct SrvHandle {
    ID3D11ShaderResourceView* p = nullptr;

    SrvHandle() = default;
    explicit SrvHandle(ID3D11ShaderResourceView* v) noexcept : p(v) {}
    ~SrvHandle() { if (p) p->Release(); }

    SrvHandle(const SrvHandle&) = delete;
    SrvHandle& operator=(const SrvHandle&) = delete;
    SrvHandle(SrvHandle&& o) noexcept : p(o.p) { o.p = nullptr; }
    SrvHandle& operator=(SrvHandle&& o) noexcept {
        if (this != &o) {
            if (p) p->Release();
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }

    [[nodiscard]] ID3D11ShaderResourceView* get() const noexcept { return p; }
    // 交出所有权（发布或入退役队列时用）。此后本对象不再释放它。
    void* release_raw() noexcept {
        void* r = p;
        p = nullptr;
        return r;
    }
};

// 用 MuPDF 的 RGBA8 缓冲直接建纹理：IMMUTABLE + 初始数据，一次调用完成上传。
// 这是 ID3D11Device 的方法（free-threaded），可在渲染线程安全调用。
bool create_page_texture(ID3D11Device* dev, const std::uint8_t* data,
                         int w, int h, int stride, ID3D11ShaderResourceView** out) {
    *out = nullptr;
    if (dev == nullptr || data == nullptr || w <= 0 || h <= 0 || stride <= 0) return false;
    if (w > kMaxTextureDim || h > kMaxTextureDim) return false;   // D3D11 单边上限
    // 先算后分配：stride 至少要有 w×4，否则 D3D 会按 pitch 跨步读取、越过缓冲区尾部。
    if (stride < w * 4) return false;
    // 字节数用 64 位算，避免 w×h×4 在 int 上溢出（超大页 + 高倍率时真会到这一步）。
    const std::uint64_t bytes = static_cast<std::uint64_t>(stride) *
                                static_cast<std::uint64_t>(h);
    if (bytes > kMaxTextureBytes) return false;

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
    tex->Release();   // SRV 已自持对纹理的引用
    if (FAILED(hr)) {
        *out = nullptr;
        return false;
    }
    return true;
}

// SrvHandle 版本：成功时句柄归调用方所有。
bool create_page_texture(ID3D11Device* dev, const std::uint8_t* data,
                         int w, int h, int stride, SrvHandle& out) {
    ID3D11ShaderResourceView* raw = nullptr;
    if (!create_page_texture(dev, data, w, h, stride, &raw)) return false;
    out = SrvHandle(raw);
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

// ---- 导出为图片的路径工具（纯字符串，不引入 <filesystem>）----

// 多页导出的文件名：<主干>_p<N>.<ext>。N 为 1 基页号，补零到 4 位（保证字典序=页序）。
// 目录拼接用 lr::join_path（utils 模块的路径工具）。
std::wstring page_file_name(const std::string& base_u8, int page0, int format) {
    std::wstring stem = base_u8.empty() ? std::wstring(L"page") : lr::utf8_to_wide(base_u8);
    wchar_t suffix[32] = {};
    std::swprintf(suffix, 32, L"_p%04d", page0 + 1);
    stem += suffix;
    stem += (format == 1) ? L".jpg" : L".png";
    return stem;
}

}  // namespace

// ---- 共享同步原语（ADR-085）----
//
// 无论拆出多少组件，**互斥量始终只有一把**：组件不各自持锁，`*_locked` 约定不变。
// 组件只持有指向它的指针，跨组件调用在同一把锁下进行，锁纪律与重构前逐字一致。
struct Sync {
    std::mutex                 mtx;
    std::condition_variable_any cv;
};

// 一条显式命令（open/close/authenticate）：后到覆盖先到（用户显式动作）。
struct Command {
    enum class Kind { Open, Close, Authenticate };
    Kind          kind = Kind::Close;
    std::wstring  path;
    std::string   password;
    std::uint64_t id = 0;
};

// ============================================================
//  TextureStore —— D3D 纹理所有权 / 缓存 / 退役队列（ADR-085）
// ============================================================
//
// 零依赖组件：不认识线程、文档、检索，只持有像素快照与字节记账。所有成员方法都要求
// 调用者持 Sync::mtx。
struct TextureStore {
    // 页缓存条目：像素快照（发布给 UI）+ 策略状态（PageState）+ 缓存元数据。
    struct Entry {
        PageSlot      slot;                  // 发布给 UI（值拷贝，UI 只读）
        PageState     state;                 // 显式状态机（ADR-085）
        std::size_t   bytes = 0;             // 纹理字节数；0 = 未驻留
        std::uint64_t last_use = 0;          // LRU 序号（越大越新）
    };

    std::vector<Entry>  pages;
    std::vector<Entry>  thumbs;
    std::size_t         used_bytes = 0;                       // 已驻留页纹理字节数
    std::size_t         thumb_bytes = 0;                      // 已驻留缩略图字节数
    std::size_t         budget_bytes = kCacheBudgetDefault;   // 字节预算
    std::uint64_t       use_tick = 0;                         // LRU 时钟
    int                 evictions = 0;                        // 累计逐出页数

    // 两段式退役队列（见文件头）
    std::vector<void*>  retired_pending;
    std::vector<void*>  retired_ready;

    static void release_srv(void* p) {
        if (p) static_cast<ID3D11ShaderResourceView*>(p)->Release();
    }

    // 页纹理退役：入 pending 并从驻留统计扣除。状态/错误保留，由调用方决定后续语义。
    void retire_pixels(Entry& e) {
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

    // 缩略图纹理退役（扣减 thumb_bytes 而不是页缓存 used_bytes）
    void retire_thumb_pixels(Entry& e) {
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

    void clear_pages() {
        for (Entry& e : pages) retire_pixels(e);
        pages.clear();
        used_bytes = 0;
        evictions = 0;
    }

    void clear_thumbs() {
        for (Entry& e : thumbs) retire_thumb_pixels(e);
        thumbs.clear();
        thumb_bytes = 0;
    }

    void resize_pages(std::size_t n) { pages.assign(n, Entry{}); }
    void resize_thumbs(std::size_t n) { thumbs.assign(n, Entry{}); }

    // 视图变换变化（旋转）时调用：全部页纹理与缩略图作废并重新排队。
    // 变换结果已固化进纹理像素，无法在原纹理上"改属性"，必须整篇重渲。
    void invalidate_all() {
        for (Entry& e : pages) {
            retire_pixels(e);
            e.state.on_invalidated();
            e.slot.error = DocError::Ok;
        }
        for (Entry& e : thumbs) {
            retire_thumb_pixels(e);
            e.state.on_invalidated();
            e.slot.error = DocError::Ok;
        }
    }

    // 仅配色变化时调用（旋转会改版面，仍走 invalidate_all）：**保留已驻留纹理继续显示**，
    // 只把它们标记为 stale 让下一轮调度重渲；新纹理就绪后原地换入 —— 全程无白屏（ADR-042）。
    void mark_all_stale() {
        for (Entry& e : pages) e.state.on_staled();
        for (Entry& e : thumbs) e.state.on_staled();
    }

    // 逐出单页：退纹理 + 状态回到 Unloaded。
    void evict_at(int i) {
        if (i < 0 || static_cast<std::size_t>(i) >= pages.size()) return;
        Entry& e = pages[static_cast<std::size_t>(i)];
        if (e.bytes == 0) return;  // 已被处理（防御）
        retire_pixels(e);
        e.state.on_evicted();
        e.slot.error = DocError::Ok;
        ++evictions;
    }

    // 发布页像素快照：旧纹理入退役队列并原地换入。返回 false = 页码越界（文档已切换），
    // 调用方需自行把新纹理退役。
    bool publish_page(int page, PageSlot&& slot, std::size_t bytes) {
        if (page < 0 || static_cast<std::size_t>(page) >= pages.size()) return false;
        Entry& e = pages[static_cast<std::size_t>(page)];
        retire_pixels(e);
        e.slot = std::move(slot);
        e.bytes = bytes;
        e.state.on_loaded();
        used_bytes += bytes;
        return true;
    }

    bool publish_thumb(int page, PageSlot&& slot, std::size_t bytes) {
        if (page < 0 || static_cast<std::size_t>(page) >= thumbs.size()) return false;
        Entry& e = thumbs[static_cast<std::size_t>(page)];
        retire_thumb_pixels(e);
        e.slot = std::move(slot);
        e.bytes = bytes;
        e.state.on_loaded();
        thumb_bytes += bytes;
        return true;
    }

    // UI 线程读取的页/缩略图快照：status 由 PageState 派生。
    PageSlot page_slot(int page) const {
        if (page < 0 || static_cast<std::size_t>(page) >= pages.size()) return {};
        PageSlot s = pages[static_cast<std::size_t>(page)].slot;
        s.status = pages[static_cast<std::size_t>(page)].state.status;
        return s;
    }
    PageSlot thumb_slot(int page) const {
        if (page < 0 || static_cast<std::size_t>(page) >= thumbs.size()) return {};
        PageSlot s = thumbs[static_cast<std::size_t>(page)].slot;
        s.status = thumbs[static_cast<std::size_t>(page)].state.status;
        return s;
    }

    // 两段式退役：本帧释放"上一帧之前"退役的，pending → ready。
    void drain_swap(std::vector<void*>& local) {
        local.swap(retired_ready);
        retired_ready.swap(retired_pending);
        retired_pending.clear();
    }

    // 工作线程已 join 后统一释放全部纹理（含退役队列）。返回释放的槽位数，供日志。
    std::size_t release_all() {
        std::size_t released = 0;
        for (Entry& e : pages) {
            release_srv(e.slot.texture);
            for (const PageSlot::Tile& t : e.slot.tiles) release_srv(t.texture);
        }
        released += pages.size();
        pages.clear();
        for (Entry& e : thumbs) {
            release_srv(e.slot.texture);
            for (const PageSlot::Tile& t : e.slot.tiles) release_srv(t.texture);
        }
        thumbs.clear();
        released += retired_pending.size() + retired_ready.size();
        for (void* p : retired_pending) release_srv(p);
        for (void* p : retired_ready) release_srv(p);
        retired_pending.clear();
        retired_ready.clear();
        used_bytes = 0;
        thumb_bytes = 0;
        return released;
    }
};

// ============================================================
//  SearchJob —— 全文检索作业（ADR-085）
// ============================================================
//
// 三处让步设计：
//   1) 每页开始前重读 id —— 新查询/取消会换 id，在途批次据此立即收工；
//   2) 每轮只扫 kSearchBatchPages 页 —— 之后必然回到主循环，渲染请求得以及时插队；
//   3) 结果攒够一批才进 pending —— UI 每帧取一次，不产生逐页的锁竞争。
struct SearchJob {
    Sync*           sync = nullptr;
    Document*       engine = nullptr;   // 工作线程私有，仅检索读取
    DocumentInfo*   info = nullptr;     // 总页数来源
    const DocState* doc = nullptr;      // 文档快照（status 的 total 走它，工作线程私有的 info 不加锁）

    bool          active = false;
    std::string   needle;              // UTF-8
    int           next_page = 0;       // 下一个待扫描页
    int           hits = 0;            // 已累计命中数
    bool          truncated = false;   // 命中触顶，提前收工
    std::uint64_t id = 0;              // 查询序号（在途批次用它判断"是否已被取代"）
    std::vector<SearchHit> pending;    // 待 UI 取走的新命中
    std::uint64_t next_id = 0;

    // 取消检索（换文档 / 关文档 / 用户取消时调用）。换 id 让在途批次失效。要求持锁。
    void cancel_locked() {
        active = false;
        needle.clear();
        next_page = 0;
        hits = 0;
        truncated = false;
        pending.clear();
        id = ++next_id;
    }

    void start(std::string utf8_needle);
    void cancel();
    [[nodiscard]] SearchStatus status() const;
    void take(std::vector<SearchHit>& out);

    void step() noexcept;
    void step_impl();
    void fault_locked() noexcept;
};

// ============================================================
//  ContentService —— 辅助请求队列与结果槽（文本/图片/链接，ADR-085）
// ============================================================
//
// 辅助请求走**独立队列**，刻意不共用 open/close/auth 的那个命令槽：文本请求由鼠标悬停
// 触发、频率高得多，若共用槽位，一次悬停就能把"待打开的文档"覆盖掉。辅助请求与渲染请求
// 同优先级、互不覆盖。
// 辅助队列的硬上限（ADR-092）：页内容请求按页去重后本应很少，但工作线程被渲染长任务
// 占用时，悬停/复制可能持续投递；超出即丢最旧的（可自愈：页内容有 0.6s 超时重发）。
inline constexpr std::size_t kMaxAuxQueue = 64;

struct ContentService {
    Sync*     sync = nullptr;
    Document* engine = nullptr;

    struct AuxReq {
        enum class Kind { PageContent, CopyText, CopyImage };
        Kind  kind = Kind::PageContent;
        int   page = -1;
        float ax = 0, ay = 0, bx = 0, by = 0;   // CopyText：选区两端（未旋转页面 pt）
        float px = 0, py = 0;                   // CopyImage：取图点（未旋转页面 pt）
    };
    std::deque<AuxReq> aux;
    bool               aux_dirty = false;

    // 入队（调用方须已持有 sync->mtx）。
    // · **latest-wins**：复制文本 / 复制图片只保留最近一次 —— 用户连点几下"复制"时旧请求
    //   毫无价值，还会在工作线程里排队做多余的重活（ADR-092）。
    // · **有界**：超过 kMaxAuxQueue 即丢最旧的。
    void enqueue_locked(AuxReq r) {
        if (r.kind == AuxReq::Kind::CopyText || r.kind == AuxReq::Kind::CopyImage) {
            for (auto it = aux.begin(); it != aux.end();) {
                if (it->kind == r.kind) it = aux.erase(it);
                else ++it;
            }
        }
        aux.push_back(std::move(r));
        while (aux.size() > kMaxAuxQueue) aux.pop_front();
        aux_dirty = true;
        sync->cv.notify_all();
    }

    // 已发布的页内容快照（只保留一页 + "未被取走"标记，理由同 document 的 stext 缓存）。
    PageContent content_pub;
    int         content_page = -1;
    bool        content_fresh = false;

    // 复制结果：一次性取用（取走即清空）。文本用 optional 区分"空结果"与"无结果"。
    std::optional<std::string> copy_text_res;
    std::optional<ImageData>   copy_image_res;

    // 换文档时只作废页内容快照（不改动辅助队列与复制结果的既有语义）。
    void clear_content_locked() {
        content_pub = PageContent{};
        content_page = -1;
        content_fresh = false;
    }

    void handle(const AuxReq& a) noexcept;
    void handle_impl(const AuxReq& a);

    void request_page_content(int page);
    bool take_page_content(int page, PageContent& out);
    void request_copy_text(int page, float ax, float ay, float bx, float by);
    bool take_copy_text(std::string& out);
    void request_copy_image(int page, float x, float y);
    bool take_copy_image(ImageData& out);
};

struct PageScheduler;

// ============================================================
//  DocumentWorker —— 文档生命周期与视图变换（ADR-085）
// ============================================================
//
// Document 实例归本组件，仅工作线程访问（UI 线程零 fz_*，ADR-009/012）。
struct DocumentWorker {
    Sync*           sync = nullptr;
    TextureStore*   store = nullptr;
    SearchJob*      search = nullptr;
    ContentService* content = nullptr;
    PageScheduler*  sched = nullptr;

    Document                 engine;      // 仅工作线程访问
    DocState                 doc;         // 发布给 UI 的文档快照
    DocumentInfo             info;        // 工作线程私有（缩略图按页尺寸算倍率用）
    std::vector<OutlineItem> outline;     // 目录快照（打开/解锁成功后一次性加载）
    std::uint64_t            fp = 0;      // 内容指纹（ADR-062）
    int                      rotation = 0;  // 视图旋转 0/90/180/270
    int                      scheme = 0;    // 页面配色 scheme（见 document.ixx）

    void open(const Command& c);
    void close(const Command& c);
    void auth(const Command& c);
    void publish(const DocState& s) { std::lock_guard lock(sync->mtx); doc = s; }
};

// ============================================================
//  ExportJob —— 导出为图片的后台作业（导出功能）
// ============================================================
//
// 与 SearchJob 同一形态：**每轮至多做一页**，做完必然回到主循环，渲染请求得以插队 ——
// 故导出期间阅读不受影响。作业跑在渲染工作线程上，复用 DocumentWorker 的 Document 实例
// （不新开线程、不重复解析；fz_* 全在 document 层）。
//
// 锁纪律：req / has_req 与全部进度字段受 Sync::mtx 保护；cur 是**工作线程私有**的执行副本
// （在锁内从 req 取走），此后只在工作线程访问，避免渲染一页的长时间里持锁。
struct ExportJob {
    Sync*           sync = nullptr;
    DocumentWorker* docw = nullptr;

    ExportRequest req;              // 受锁：UI 写入 / 工作线程取走
    bool          has_req = false;  // 受锁：有未开始（或被替换）的请求

    ExportRequest cur;              // 工作线程私有：取走后的执行副本
    bool          cap_logged = false;  // 工作线程私有：本轮是否已记过"倍率被上限下调"日志

    // 进度（受锁）
    bool          active = false;
    int           done = 0, total = 0, failed = 0;
    bool          finished = false;
    bool          cancelled = false;
    std::uint64_t id = 0, next_id = 0;
    std::wstring  last_written;
    std::string   error_text;

    void start(ExportRequest r);
    void cancel();
    [[nodiscard]] ExportStatus status() const;
    void clear_result();
    void cancel_locked() noexcept;   // 调用方须已持锁
    void fault_locked() noexcept;    // 工作线程故障：把在途导出收尾为失败
    void step() noexcept;
    void step_impl();
};

// ============================================================
//  PageScheduler —— 渲染/缩略图请求调度（ADR-085）
// ============================================================
struct PageScheduler {
    Sync*            sync = nullptr;
    ID3D11Device*    device = nullptr;
    TextureStore*    store = nullptr;
    DocumentWorker*  docw = nullptr;

    std::vector<RenderWant> wants;
    std::vector<RenderWant> last_wants;    // 已投递的最后一组请求，用于去重
    bool                    wants_dirty = false;

    std::vector<int> thumb_pages;          // 本帧需要的缩略图页
    std::vector<int> last_thumb_pages;     // 去重用
    int              thumb_target_px = 150;
    int              last_thumb_px = -1;
    bool             thumbs_dirty = false;

    ResourceTier    tier = ResourceTier::Balanced;
    ResourceProfile profile = lr::resource_profile(ResourceTier::Balanced);

    using Entry = TextureStore::Entry;

    // 单页 tile 构建期间的临时所有权（SrvHandle 析构自动释放）。
    struct BuiltTile {
        SrvHandle        handle;
        PageSlot::Tile   view;   // 仅在其 handle 被 release_raw 后有效
    };

    // 文档切换 / 关闭时复位投递簿记（last_wants / last_thumb*）。
    void reset_wants_bookkeeping() {
        last_wants.clear();
        last_thumb_pages.clear();
        last_thumb_px = -1;
    }

    void handle_wants(const std::vector<RenderWant>& w);
    void evict_to_budget(const std::vector<char>& wanted);

    // 渲染单页。失败时自动重试至多 kMaxAutoRetries 次，仍失败则定格为 Failed
    // （等待 UI 点击重试，见 retry_page）。加载/重试期间**保留旧纹理**（ADR-024）。
    void render_one(int page, float scale) noexcept;
    void render_one_impl(int page, float scale);
    void render_one_tiled(int page, float scale, int full_w, int full_h);
    void publish_loaded(int page, SrvHandle&& srv, const PageBitmap& bmp);
    void publish_loaded_tiles(int page, float requested_scale, int full_w, int full_h,
                              std::vector<BuiltTile> built);
    void mark_failed(int page, DocError err, float scale);

    void handle_thumbs(const std::vector<int>& list, int target_px);
    void render_thumb(int page, int target_px) noexcept;
    void render_thumb_impl(int page, int target_px);

    void retry_page(int page);
};

// ============================================================
//  Impl —— 工作线程 / 命令槽 / 任务分派（ADR-085）
// ============================================================
struct Renderer::Impl {
    explicit Impl(ID3D11Device* dev) noexcept : device(dev) {
        sched.device = dev;
        docw.sync = &sync;
        docw.store = &store;
        docw.search = &search;
        docw.content = &content;
        docw.sched = &sched;
        search.sync = &sync;
        search.engine = &docw.engine;
        search.info = &docw.info;
        search.doc = &docw.doc;
        content.sync = &sync;
        content.engine = &docw.engine;
        sched.sync = &sync;
        sched.store = &store;
        sched.docw = &docw;
        exp.sync = &sync;
        exp.docw = &docw;
    }

    ID3D11Device* device = nullptr;

    Sync                     sync;      // 唯一的互斥量 + 条件变量
    std::optional<Command>   cmd;
    std::uint64_t            next_id = 0;

    TextureStore    store;
    DocumentWorker  docw;
    ContentService  content;
    SearchJob       search;
    PageScheduler   sched;
    ExportJob       exp;

    std::jthread worker;

    void start() {
        // lambda 标 noexcept：run() 本身不抛，这里再钉一道，杜绝"入口逃逸异常"这条路径。
        worker = std::jthread([this](std::stop_token st) noexcept { run(st); });
        lr::log::info("render", "worker started");
    }

    void shutdown() {
        worker.request_stop();
        sync.cv.notify_all();
        if (worker.joinable()) worker.join();   // 必须先 join：此后才没有人再动这些纹理
        std::size_t released = 0;
        {
            std::lock_guard lock(sync.mtx);
            released = store.release_all();
        }
        lr::log::info("render", "worker stopped " + lr::log::kv("slots_released", released));
    }

    // 工作线程主循环。**入口是 noexcept 的**，任何异常都在这里被截住 ——
    // 若让异常逃出 std::jthread 的入口函数，运行库会直接 std::terminate（进程消失）。
    void run(std::stop_token st) noexcept {
        int consecutive_faults = 0;
        for (;;) {
            if (st.stop_requested()) return;
            bool keep_going = true;
            try {
                keep_going = step_once(st);
                consecutive_faults = 0;
            } catch (const std::exception& e) {
                consecutive_faults = on_worker_fault(e.what(), consecutive_faults);
            } catch (...) {
                consecutive_faults = on_worker_fault("non-std exception", consecutive_faults);
            }
            if (!keep_going) return;
            // 连续故障时节流：避免"每次唤醒都立刻再抛一次"把 CPU 打满、把日志刷爆。
            if (consecutive_faults >= 3) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    // 一轮：取任务 → 执行。返回 false = 请求停止。
    bool step_once(std::stop_token st) {
        inject_fault_for_tests("worker-step");
        std::optional<Command>                c;
        std::optional<ContentService::AuxReq> a;
        std::vector<RenderWant>               w;
        std::vector<int>                      tw;
        bool have_wants = false;
        bool have_thumbs = false;
        int  thumb_px = 150;
        {
            std::unique_lock lock(sync.mtx);
            sync.cv.wait(lock, st, [this] {
                return cmd.has_value() || content.aux_dirty || sched.wants_dirty ||
                       sched.thumbs_dirty || search.active || exp.has_req || exp.active;
            });
            if (st.stop_requested()) return false;
            // 优先级：显式命令 > 辅助请求 > 渲染请求 > 缩略图 > 检索。
            if (cmd) {                      // 命令优先，渲染请求留到下一轮
                c = std::move(cmd);
                cmd.reset();
            } else if (content.aux_dirty) {
                if (!content.aux.empty()) {
                    a = content.aux.front();
                    content.aux.pop_front();
                }
                content.aux_dirty = !content.aux.empty();
            } else if (sched.wants_dirty) {
                w = std::move(sched.wants);
                sched.wants.clear();
                sched.wants_dirty = false;
                have_wants = true;
            } else if (sched.thumbs_dirty) {
                tw = sched.thumb_pages;
                thumb_px = sched.thumb_target_px;
                sched.thumbs_dirty = false;
                have_thumbs = true;
            }
        }
        if (c) handle_command(*c);
        else if (a) content.handle(*a);
        else if (have_wants) sched.handle_wants(w);
        else if (have_thumbs) sched.handle_thumbs(tw, thumb_px);
        else search.step();
        // 渲染/缩略图之后捎带一轮检索：两者都不忙时才轮到它，天然与渲染交替。
        if (have_wants || have_thumbs) search.step();
        // 导出为图片：每轮至多一页，排在最后 —— 有渲染/检索时它只推进一页，不抢占，
        // 空闲时它连续推进。故导出期间阅读与检索都不受影响。
        exp.step();
        return true;
    }

    // 工作线程级故障：留痕 + 把文档打成失败态（比"整个进程消失"温和得多）。返回新的连续故障计数。
    int on_worker_fault(const char* what, int consecutive) noexcept {
        const std::string detail = what ? what : "internal error";
        lr::log::error("render", "worker fault " + lr::log::kv("what", detail) + " " +
                                     lr::log::kv("consecutive", consecutive + 1));
        try {
            // 用 try_to_lock：异常可能发生在临界区被破坏之后，宁可只留下日志，
            // 也不要在崩溃边缘死等一把可能永远不会释放的锁。
            std::unique_lock lock(sync.mtx, std::try_to_lock);
            if (lock.owns_lock()) {
                docw.engine.close();
                store.clear_pages();
                store.clear_thumbs();
                search.cancel_locked();
                content.clear_content_locked();
                exp.fault_locked();
                sched.reset_wants_bookkeeping();
                docw.info = DocumentInfo{};
                docw.outline.clear();
                DocState s;
                s.phase = DocPhase::Failed;
                s.error = DocError::Internal;
                s.detail_u8 = detail;
                s.id = docw.doc.id;   // 沿用当前请求序号：让 UI 认得出这是"本份文档"的失败
                s.file_fingerprint = docw.fp;
                docw.doc = s;
                docw.fp = 0;
            } else {
                lr::log::error("render", "worker fault: mutex busy, state left as-is");
            }
        } catch (...) {
        }
        return consecutive + 1;
    }

    void handle_command(const Command& c) {
        // 换文档 / 关文档：在途导出立即取消（它绑在旧文档的页上，继续导会导错或越界）。
        if (c.kind == Command::Kind::Open || c.kind == Command::Kind::Close) {
            std::lock_guard lock(sync.mtx);
            exp.cancel_locked();
        }
        switch (c.kind) {
        case Command::Kind::Open:         docw.open(c); break;
        case Command::Kind::Close:        docw.close(c); break;
        case Command::Kind::Authenticate: docw.auth(c); break;
        }
    }

    // ---- 测试专用故障注入（ADR-078 的验证手段）----
    //
    // 环境变量 LILITH_FAULT_INJECT=N → 前 N 轮工作任务主动抛异常，用来验证"工作线程入口
    // 逃出异常会 terminate"这条防线真的立住了（见 tests/render_fault_test.cpp）。
    // **Release 构建整段不编译**，发布版没有任何可触发的崩溃入口。
#ifndef NDEBUG
    int  fault_budget_ = 0;
    bool fault_checked_ = false;

    void inject_fault_for_tests(const char* where) {
        if (!fault_checked_) {
            fault_checked_ = true;
            char buf[32] = {};
            std::size_t n = 0;
            if (::getenv_s(&n, buf, sizeof buf, "LILITH_FAULT_INJECT") == 0 && n > 0)
                fault_budget_ = std::atoi(buf);
        }
        if (fault_budget_ > 0) {
            --fault_budget_;
            lr::log::warn("render", std::string("fault injected at ") + where);
            throw std::runtime_error("injected fault");
        }
    }
#else
    void inject_fault_for_tests(const char*) noexcept {}
#endif
};

// ============================================================
//  组件方法定义（需在全部组件类型完备之后）
// ============================================================

// ---- SearchJob ----

void SearchJob::start(std::string utf8_needle) {
    std::lock_guard lock(sync->mtx);
    cancel_locked();     // 旧查询立即作废（换 id，在途批次收工）
    // 只挡空关键字。**不挡"页表为空"**：那种情况下 step 会因为文档未打开而立刻收敛
    // （active=false），UI 如实显示"没有找到"。
    if (utf8_needle.empty()) {
        sync->cv.notify_all();
        return;
    }
    needle = std::move(utf8_needle);
    id = ++next_id;
    next_page = 0;
    active = true;
    sync->cv.notify_all();
}

void SearchJob::cancel() {
    std::lock_guard lock(sync->mtx);
    cancel_locked();
    sync->cv.notify_all();
}

SearchStatus SearchJob::status() const {
    SearchStatus s;
    std::lock_guard lock(sync->mtx);
    s.active = active;
    s.scanned = next_page;
    s.total = doc->info.page_count;   // 走 doc 快照：info 是工作线程私有、不加锁
    s.hits = hits;
    s.truncated = truncated;
    s.id = id;
    s.needle = needle;
    return s;
}

void SearchJob::take(std::vector<SearchHit>& out) {
    std::lock_guard lock(sync->mtx);
    if (pending.empty()) return;
    for (SearchHit& h : pending) out.push_back(std::move(h));
    pending.clear();
}

void SearchJob::step() noexcept {
    try {
        step_impl();
    } catch (const std::exception& e) {
        lr::log::error("render", "search step threw " + lr::log::kv("what", e.what()));
        fault_locked();
    } catch (...) {
        lr::log::error("render", "search step threw " + lr::log::kv("what", "non-std"));
        fault_locked();
    }
}

// 检索出错时**让检索收敛**而不是把文档打回失败态：把结果标记为已扫完并停止，
// 界面于是停在"共 N 处"上 —— 用户看得懂，也不会每帧再撞一次同一个异常。
void SearchJob::fault_locked() noexcept {
    try {
        std::lock_guard lock(sync->mtx);
        active = false;
        truncated = false;
    } catch (...) {
    }
}

void SearchJob::step_impl() {
    std::string   local_needle;
    std::uint64_t local_id = 0;
    int           page = 0;
    int           hits_seen = 0;
    bool          trunc = false;
    {
        std::lock_guard lock(sync->mtx);
        if (!active) return;
        local_needle = needle;
        local_id = id;
        page = next_page;
        hits_seen = hits;
        trunc = truncated;
    }
    if (!engine->is_open() || local_needle.empty()) {
        std::lock_guard lock(sync->mtx);
        if (id == local_id) active = false;
        return;
    }
    const int total = info->page_count;

    std::vector<SearchHit> found;
    std::vector<SearchHit> page_hits;
    for (int n = 0; n < kSearchBatchPages && page < total; ++n, ++page) {
        {
            // 每页前检查：是否已被新查询取代 / 已取消 / 文档已换
            std::lock_guard lock(sync->mtx);
            if (!active || id != local_id) return;
        }
        page_hits.clear();
        if (engine->search_page(page, local_needle, 200, page_hits) != DocError::Ok) continue;
        for (SearchHit& h : page_hits) {
            if (hits_seen >= kMaxSearchHits) { trunc = true; break; }
            found.push_back(std::move(h));
            ++hits_seen;
        }
        if (trunc) { ++page; break; }
    }

    std::lock_guard lock(sync->mtx);
    if (!active || id != local_id) return;
    for (SearchHit& h : found) pending.push_back(std::move(h));
    hits = hits_seen;
    next_page = page;
    truncated = trunc;
    if (trunc || page >= total) active = false;
}

// ---- ExportJob ----

void ExportJob::start(ExportRequest r) {
    std::lock_guard lock(sync->mtx);
    req = std::move(r);
    has_req = true;
    // 已有在途作业时不立刻清进度：工作线程下一轮取走新请求时才重置（避免 UI 进度闪一下）。
    sync->cv.notify_all();
}

void ExportJob::cancel() {
    std::lock_guard lock(sync->mtx);
    cancel_locked();
    sync->cv.notify_all();
}

// 取消：清掉未开始的请求；若正在导出则置 cancelled，工作线程在该页结束后收敛（finished）。
// 保持 active 为真，使工作线程的等待谓词仍成立、能被唤醒去收尾。
void ExportJob::cancel_locked() noexcept {
    has_req = false;
    if (!active) return;
    cancelled = true;
}

ExportStatus ExportJob::status() const {
    std::lock_guard lock(sync->mtx);
    ExportStatus s;
    s.active = active;
    s.done = done;
    s.total = total;
    s.failed = failed;
    s.finished = finished;
    s.cancelled = cancelled;
    s.id = id;
    s.last_written = last_written;
    s.error_text = error_text;
    return s;
}

void ExportJob::clear_result() {
    std::lock_guard lock(sync->mtx);
    finished = false;
    cancelled = false;
    error_text.clear();
}

// 工作线程故障：把在途导出收尾为失败（finished），让 UI 不至于永远停在"导出中"。
void ExportJob::fault_locked() noexcept {
    if (!has_req && !active) return;
    has_req = false;
    active = false;
    finished = true;
    cancelled = true;
}

void ExportJob::step() noexcept {
    try {
        step_impl();
    } catch (const std::exception& e) {
        lr::log::error("render", "export step threw " + lr::log::kv("what", e.what()));
        std::lock_guard lock(sync->mtx);
        active = false;
        finished = true;
        if (error_text.empty()) error_text = "导出过程出错";
    } catch (...) {
        lr::log::error("render", "export step threw " + lr::log::kv("what", "non-std"));
        std::lock_guard lock(sync->mtx);
        active = false;
        finished = true;
        if (error_text.empty()) error_text = "导出过程出错";
    }
}

void ExportJob::step_impl() {
    int page = -1;
    {
        std::lock_guard lock(sync->mtx);
        if (has_req) {                 // 取走（或被替换的）新请求：重置进度
            cur = std::move(req);
            has_req = false;
            active = true;
            done = 0;
            failed = 0;
            finished = false;
            cancelled = false;
            error_text.clear();
            last_written.clear();
            cap_logged = false;
            total = static_cast<int>(cur.pages.size());
            id = ++next_id;
        } else if (!active || finished) {
            return;
        }
        if (cancelled || total <= 0) {
            finished = true;
            active = false;
            return;
        }
        page = cur.pages[static_cast<std::size_t>(done)];
    }

    // 目标路径（cur 为工作线程私有，锁外读取安全）。
    std::wstring out_path;
    if (cur.single_file) out_path = cur.out_dir;
    else out_path = lr::join_path(cur.out_dir, page_file_name(cur.base_name, page, cur.format));

    float   eff = 0.0f;
    const DocError e = docw->engine.save_page_as_image(
        page, cur.scale, static_cast<ImageFormat>(cur.format), cur.quality,
        cur.max_dimension, cur.rotation_deg, static_cast<PageScheme>(cur.scheme),
        out_path, &eff);

    // 诊断：请求倍率被单边像素上限下调时留一次痕。对用户是**无感**的（页仍然导出成功，
    // 只是分辨率低于所选 DPI），但排查"某页导出为何不够清晰"时必须能看到这件事。
    // 每轮导出只记一次，避免整本导出把日志刷满。
    if (!cap_logged && eff > 0.0f && eff < cur.scale) {
        cap_logged = true;
        lr::log::info("render", "export scale capped " +
                                    lr::log::kv("want", static_cast<double>(cur.scale)) + " " +
                                    lr::log::kv("used", static_cast<double>(eff)));
    }

    std::lock_guard lock(sync->mtx);
    if (!active) return;               // 期间被取消 / 替换：丢弃本页结果
    ++done;
    if (e != DocError::Ok) {
        ++failed;
        if (error_text.empty()) error_text = std::string(lr::describe(e));
    }
    last_written = cur.single_file ? out_path : cur.out_dir;
    if (done >= static_cast<int>(cur.pages.size())) {
        finished = true;
        active = false;
    }
}

// ---- ContentService ----

// 异常边界：一次文本/图片提取失败只让这次请求没有结果，**不能**把整篇文档打回失败态 ——
// 用户只是把鼠标移到了一段排版奇怪的文字上而已。
void ContentService::handle(const AuxReq& a) noexcept {
    try {
        handle_impl(a);
    } catch (const std::exception& e) {
        lr::log::error("render", "aux request threw " + lr::log::kv("kind", static_cast<int>(a.kind)) +
                                     " " + lr::log::kv("page", a.page) + " " +
                                     lr::log::kv("what", e.what()));
    } catch (...) {
        lr::log::error("render", "aux request threw " +
                                     lr::log::kv("page", a.page) + " " +
                                     lr::log::kv("what", "non-std"));
    }
}

void ContentService::handle_impl(const AuxReq& a) {
    if (!engine->is_open()) return;
    switch (a.kind) {
    case AuxReq::Kind::PageContent: {
        PageContent pc;
        if (engine->page_content(a.page, pc) != DocError::Ok) return;
        std::lock_guard lock(sync->mtx);
        content_pub = std::move(pc);
        content_page = a.page;
        content_fresh = true;
        break;
    }
    case AuxReq::Kind::CopyText: {
        std::string text;
        // 失败不改变结果槽：UI 会一直等不到结果 —— 但复制失败没有别的补救，
        // 宁可让它"没反应"，也不要写入半截文本（半截更糟：用户以为复制成功）。
        if (engine->copy_text(a.page, a.ax, a.ay, a.bx, a.by, text) != DocError::Ok) return;
        std::lock_guard lock(sync->mtx);
        copy_text_res = std::move(text);
        break;
    }
    case AuxReq::Kind::CopyImage: {
        ImageData img;   // 无图片时保持空 → take_copy_image 返回 true 但 out 无效
        engine->image_at(a.page, a.px, a.py, img);
        std::lock_guard lock(sync->mtx);
        copy_image_res = std::move(img);
        break;
    }
    }
}

void ContentService::request_page_content(int page) {
    if (page < 0) return;
    std::lock_guard lock(sync->mtx);
    for (const AuxReq& a : aux)
        if (a.kind == AuxReq::Kind::PageContent && a.page == page) return;  // 已在队列
    AuxReq r;
    r.kind = AuxReq::Kind::PageContent;
    r.page = page;
    enqueue_locked(std::move(r));
}

bool ContentService::take_page_content(int page, PageContent& out) {
    std::lock_guard lock(sync->mtx);
    if (!content_fresh || content_page != page) return false;
    out = std::move(content_pub);
    content_pub = PageContent{};
    content_fresh = false;
    return true;
}

void ContentService::request_copy_text(int page, float ax, float ay, float bx, float by) {
    if (page < 0) return;
    std::lock_guard lock(sync->mtx);
    AuxReq r;
    r.kind = AuxReq::Kind::CopyText;
    r.page = page;
    r.ax = ax; r.ay = ay;
    r.bx = bx; r.by = by;
    enqueue_locked(std::move(r));
}

bool ContentService::take_copy_text(std::string& out) {
    std::lock_guard lock(sync->mtx);
    if (!copy_text_res.has_value()) return false;
    out = std::move(*copy_text_res);
    copy_text_res.reset();
    return true;
}

void ContentService::request_copy_image(int page, float x, float y) {
    if (page < 0) return;
    std::lock_guard lock(sync->mtx);
    AuxReq r;
    r.kind = AuxReq::Kind::CopyImage;
    r.page = page;
    r.px = x; r.py = y;
    enqueue_locked(std::move(r));
}

bool ContentService::take_copy_image(ImageData& out) {
    std::lock_guard lock(sync->mtx);
    if (!copy_image_res.has_value()) return false;
    out = std::move(*copy_image_res);
    copy_image_res.reset();
    return true;
}

// ---- DocumentWorker ----

void DocumentWorker::open(const Command& c) {
    engine.close();
    {
        std::lock_guard lock(sync->mtx);
        store->clear_pages();
        store->clear_thumbs();
        search->cancel_locked();   // 换文档：旧的检索结果一律作废（页号不再对应）
        content->clear_content_locked();
        sched->reset_wants_bookkeeping();
    }

    DocState s;
    s.id = c.id;
    fp = lr::file_fingerprint(c.path);   // 工作线程：不占 UI 时间（ADR-009）
    s.file_fingerprint = fp;
    const DocError err = engine.open(c.path);
    if (err == DocError::Ok) {
        DocumentInfo di;
        const DocError ierr = engine.info(di);
        // 页数上限：损坏或伪造的文档可能报出天文数字页数。先拦在这里，
        // 不让 resize 的分配失败去充当"边界检查"（那会升级成线程级故障）。
        if (ierr == DocError::Ok && di.page_count <= kMaxPageCount) {
            s.phase = DocPhase::Ready;
            s.info = di;
            info = di;  // 缩略图按页尺寸算倍率用（工作线程私有，无需加锁）
            std::vector<OutlineItem> ol;
            engine.outline(ol);  // 锁外加载（可能触及页树），避免长时间持锁
            std::lock_guard lock(sync->mtx);
            const std::size_t n =
                static_cast<std::size_t>(di.page_count > 0 ? di.page_count : 0);
            store->resize_pages(n);
            store->resize_thumbs(n);
            outline = std::move(ol);
        } else {
            s.phase = DocPhase::Failed;
            s.error = (ierr == DocError::Ok) ? DocError::TooLarge : ierr;
            s.detail_u8.assign(engine.last_error());
            engine.close();
        }
    } else {
        s.phase = DocPhase::Failed;
        s.error = err;
        s.detail_u8.assign(engine.last_error());
    }
    lr::log::info("render", std::string("open ") +
                               (s.phase == DocPhase::Ready ? "ok" : "failed") + " " +
                               lr::log::kv("path", c.path) + " " +
                               lr::log::kv("pages", s.info.page_count) + " " +
                               lr::log::kv("err", std::string(to_string(s.error))) +
                               (s.detail_u8.empty() ? "" : " " + lr::log::kv("detail", s.detail_u8)));
    publish(s);
}

void DocumentWorker::close(const Command& c) {
    engine.close();
    DocState s;
    s.phase = DocPhase::Idle;
    s.id = c.id;
    std::lock_guard lock(sync->mtx);
    store->clear_pages();
    store->clear_thumbs();
    search->cancel_locked();
    content->clear_content_locked();
    sched->reset_wants_bookkeeping();
    info = DocumentInfo{};
    outline.clear();
    fp = 0;
    doc = s;
}

void DocumentWorker::auth(const Command& c) {
    DocState s;
    s.id = c.id;
    s.file_fingerprint = fp;   // 解锁成功时沿用打开时算的指纹（密码错误也不重算）
    const DocError err = engine.authenticate(c.password);
    if (err == DocError::Ok) {
        DocumentInfo di;
        const DocError ierr = engine.info(di);
        if (ierr == DocError::Ok && di.page_count <= kMaxPageCount) {
            s.phase = DocPhase::Ready;
            s.info = di;
            info = di;
            std::vector<OutlineItem> ol;
            engine.outline(ol);
            std::lock_guard lock(sync->mtx);
            store->clear_pages();
            store->clear_thumbs();
            search->cancel_locked();
            content->clear_content_locked();
            const std::size_t n =
                static_cast<std::size_t>(di.page_count > 0 ? di.page_count : 0);
            store->resize_pages(n);
            store->resize_thumbs(n);
            outline = std::move(ol);
            sched->reset_wants_bookkeeping();
        } else {
            s.phase = DocPhase::Failed;
            s.error = (ierr == DocError::Ok) ? DocError::TooLarge : ierr;
            s.detail_u8.assign(engine.last_error());
        }
    } else {
        s.phase = DocPhase::Failed;
        s.error = err;
        s.detail_u8.assign(engine.last_error());
    }
    lr::log::info("render", std::string("auth ") +
                               (s.phase == DocPhase::Ready ? "ok" : "failed") + " " +
                               lr::log::kv("err", std::string(to_string(s.error))));
    publish(s);
}

// ---- PageScheduler ----

void PageScheduler::handle_wants(const std::vector<RenderWant>& w) {
    if (!docw->engine.is_open()) return;

    // 1) 标记本帧需要的页（pinned，不可逐出）并刷新其 LRU 序号
    std::vector<char> wanted;
    {
        std::lock_guard lock(sync->mtx);
        wanted.assign(store->pages.size(), 0);
        ++store->use_tick;
        for (const RenderWant& x : w) {
            if (x.page < 0 || static_cast<std::size_t>(x.page) >= store->pages.size()) continue;
            wanted[static_cast<std::size_t>(x.page)] = 1;
            store->pages[static_cast<std::size_t>(x.page)].last_use = store->use_tick;
        }
    }

    // 2) 按字节预算逐出（纯策略：lilithreader.page_cache::select_evictions）
    evict_to_budget(wanted);

    // 3) 渲染缺失/不够清晰/已因视图变换而 stale 的页（按 wants 顺序，可见页在前）
    for (const RenderWant& x : w) {
        if (x.page < 0 || !(x.scale > 0.0f)) continue;
        if (static_cast<std::size_t>(x.page) >= store->pages.size()) continue;
        PageSlot cur;
        bool stale = false;
        {
            std::lock_guard lock(sync->mtx);
            Entry& e = store->pages[static_cast<std::size_t>(x.page)];
            cur = e.slot;
            cur.status = e.state.status;
            stale = e.state.stale;
        }
        if (!stale && cur.status == PageStatus::Loaded && cur.scale >= x.scale * 0.999f)
            continue;  // 已够清晰且未过期
        render_one(x.page, x.scale);
    }
}

void PageScheduler::evict_to_budget(const std::vector<char>& wanted) {
    std::vector<CachePageView> views;
    std::size_t used = 0, budget = 0;
    {
        std::lock_guard lock(sync->mtx);
        views.resize(store->pages.size());
        for (std::size_t i = 0; i < store->pages.size(); ++i) {
            CachePageView v;
            v.bytes = store->pages[i].bytes;
            v.last_use = store->pages[i].last_use;
            v.pinned = i < wanted.size() && wanted[i] != 0;
            views[i] = v;
        }
        used = store->used_bytes;
        budget = store->budget_bytes;
    }
    const std::vector<int> victims = select_evictions(views, used, budget);
    if (victims.empty()) return;
    std::lock_guard lock(sync->mtx);
    for (int i : victims) store->evict_at(i);
}

// 单页渲染的**异常边界**（ADR-078）：这里的 try/catch 只把异常转成"这一页失败"，
// 而不是让整篇文档被打回失败态 —— 粒度越小，一次内存不足的波及面就越小。
void PageScheduler::render_one(int page, float scale) noexcept {
    try {
        render_one_impl(page, scale);
    } catch (const std::exception& e) {
        lr::log::error("render", "page render threw " + lr::log::kv("page", page) + " " +
                                     lr::log::kv("what", e.what()));
        mark_failed(page, DocError::Internal, scale);
    } catch (...) {
        lr::log::error("render", "page render threw " + lr::log::kv("page", page) + " " +
                                     lr::log::kv("what", "non-std"));
        mark_failed(page, DocError::Internal, scale);
    }
}

void PageScheduler::render_one_impl(int page, float scale) {
    {
        std::lock_guard lock(sync->mtx);
        if (page < 0 || static_cast<std::size_t>(page) >= store->pages.size()) return;
        Entry& e = store->pages[static_cast<std::size_t>(page)];
        if (!e.state.plan_render(scale)) return;
        // 只改状态，绝不动 texture/scale —— 重渲染期间 UI 继续显示旧纹理，不闪白。
        e.slot.error = DocError::Ok;
    }

    const DocumentInfo& di = docw->info;
    // 超过档位 tile 单边时，按整页输出像素拆分。每次只在 MuPDF 中保留一个
    // tile pixmap，避免巨型页面先分配完整 RGBA 缓冲；上传仍在本工作线程完成。
    int full_w = 0;
    int full_h = 0;
    if (page >= 0 && static_cast<std::size_t>(page) < di.page_sizes.size()) {
        const PageSize ps = di.page_sizes[static_cast<std::size_t>(page)];
        const bool swap = (docw->rotation % 180) != 0;
        const double pw = std::max(0.0, static_cast<double>(ps.width_pt) * scale);
        const double ph = std::max(0.0, static_cast<double>(ps.height_pt) * scale);
        const double fw = swap ? ph : pw;
        const double fh = swap ? pw : ph;
        full_w = fw > 2147483000.0 ? 2147483000 : static_cast<int>(std::lround(fw));
        full_h = fh > 2147483000.0 ? 2147483000 : static_cast<int>(std::lround(fh));
    } else if (di.page_sizes.empty()) {
        const bool swap = (docw->rotation % 180) != 0;
        const double pw = std::max(0.0, static_cast<double>(di.page_width_pt) * scale);
        const double ph = std::max(0.0, static_cast<double>(di.page_height_pt) * scale);
        full_w = static_cast<int>(std::lround(swap ? ph : pw));
        full_h = static_cast<int>(std::lround(swap ? pw : ph));
    }
    // 先算后分配：整页输出像素总量超限直接判 TooLarge（不让乘法溢出，也不让分配失败当边界检查）。
    if (full_w > 0 && full_h > 0) {
        const std::uint64_t pixels =
            static_cast<std::uint64_t>(full_w) * static_cast<std::uint64_t>(full_h);
        if (pixels > kMaxOutputPixels) {
            lr::log::warn("render", "page too large " + lr::log::kv("page", page) + " " +
                                        lr::log::kv("w", full_w) + " " + lr::log::kv("h", full_h));
            mark_failed(page, DocError::TooLarge, scale);
            return;
        }
    }
    if (full_w > profile.tile_size_px || full_h > profile.tile_size_px) {
        render_one_tiled(page, scale, full_w, full_h);
        return;
    }

    for (;;) {
        PageBitmap bmp;
        DocError err = docw->engine.render_page(page, scale, bmp, 8192, docw->rotation,
                                                static_cast<PageScheme>(docw->scheme));

        SrvHandle srv;   // 未发布即析构 → 自动释放，无需在每条失败分支手工 Release
        if (err == DocError::Ok &&
            !create_page_texture(device, bmp.samples(), bmp.width(), bmp.height(),
                                 bmp.stride(), srv)) {
            err = DocError::Internal;
        }
        if (err == DocError::Ok) {
            publish_loaded(page, std::move(srv), bmp);
            return;
        }

        // 失败：还有自动重试额度吗？
        bool again = false;
        {
            std::lock_guard lock(sync->mtx);
            if (page >= 0 && static_cast<std::size_t>(page) < store->pages.size()) {
                again = store->pages[static_cast<std::size_t>(page)].state.consume_retry();
            }
        }
        if (!again) { mark_failed(page, err, scale); return; }
    }
}

void PageScheduler::render_one_tiled(int page, float scale, int full_w, int full_h) {
    const int tile_size = std::max(256, profile.tile_size_px);
    if (full_w <= 0 || full_h <= 0) {
        mark_failed(page, DocError::Internal, scale);
        return;
    }
    // tile 个数先算后建：上限既防"页尺寸算错导致几万个 tile"的雪崩，
    // 也保证 built 这个 vector 的容量是有界的。
    const std::int64_t tiles_x = (static_cast<std::int64_t>(full_w) + tile_size - 1) / tile_size;
    const std::int64_t tiles_y = (static_cast<std::int64_t>(full_h) + tile_size - 1) / tile_size;
    if (tiles_x * tiles_y > kMaxTiles) {
        lr::log::warn("render", "too many tiles " + lr::log::kv("page", page) + " " +
                                    lr::log::kv("tiles", tiles_x * tiles_y));
        mark_failed(page, DocError::TooLarge, scale);
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
                const DocError err = docw->engine.render_page_tile(
                    page, scale, r, bmp, tile_size, docw->rotation,
                    static_cast<PageScheme>(docw->scheme));
                if (err != DocError::Ok) { failure = err; break; }
                BuiltTile t;
                if (!create_page_texture(device, bmp.samples(), bmp.width(), bmp.height(),
                                         bmp.stride(), t.handle)) {
                    failure = DocError::Internal;
                    break;
                }
                t.view.texture = t.handle.get();
                t.view.x = x; t.view.y = y;
                t.view.w = bmp.width(); t.view.h = bmp.height();
                // push_back 可能抛（扩容失败）：此时 built 里已建好的 tile 由各自的
                // SrvHandle 析构释放 —— 原来的"失败后手工 Release 一遍"因此可以删掉。
                built.push_back(std::move(t));
            }
        }
        if (failure == DocError::Ok) {
            publish_loaded_tiles(page, scale, full_w, full_h, std::move(built));
            return;
        }
        // 失败：built 整体析构即释放全部已建纹理（SrvHandle 的析构路径）。
        bool again = false;
        {
            std::lock_guard lock(sync->mtx);
            if (page >= 0 && static_cast<std::size_t>(page) < store->pages.size()) {
                again = store->pages[static_cast<std::size_t>(page)].state.consume_retry();
            }
        }
        if (!again) { mark_failed(page, failure, scale); return; }
    }
}

// 接管 srv 的所有权（发布给 UI 只读；被替换的旧纹理进退役队列）。
void PageScheduler::publish_loaded(int page, SrvHandle&& srv, const PageBitmap& bmp) {
    const std::size_t bytes =
        static_cast<std::size_t>(bmp.width()) * static_cast<std::size_t>(bmp.height()) * 4u;

    PageSlot loaded;
    loaded.texture = srv.get();
    loaded.pixel_w = bmp.width();
    loaded.pixel_h = bmp.height();
    loaded.full_pixel_w = bmp.width();
    loaded.full_pixel_h = bmp.height();
    loaded.scale = bmp.effective_scale();

    std::lock_guard lock(sync->mtx);
    if (!store->publish_page(page, std::move(loaded), bytes)) {
        // 文档已切换，结果作废：交还给退役队列（由 UI 帧首统一释放）。
        store->retired_pending.push_back(srv.release_raw());
        return;
    }
    srv.release_raw();   // 所有权已转入 store 的 Entry.slot
}

void PageScheduler::publish_loaded_tiles(int page, float requested_scale, int full_w, int full_h,
                                         std::vector<BuiltTile> built) {
    std::size_t bytes = 0;
    PageSlot loaded;
    loaded.scale = requested_scale;
    loaded.full_pixel_w = full_w;
    loaded.full_pixel_h = full_h;
    loaded.tiles.reserve(built.size());
    for (BuiltTile& t : built) {
        bytes += static_cast<std::size_t>(t.view.w) * static_cast<std::size_t>(t.view.h) * 4u;
        loaded.tiles.push_back(t.view);
        t.view.texture = nullptr;
        t.handle.release_raw();   // 所有权转入 loaded.tiles（交不入则入退役队列）
    }
    if (!loaded.tiles.empty()) {
        loaded.pixel_w = loaded.tiles.front().w;
        loaded.pixel_h = loaded.tiles.front().h;
    }
    std::lock_guard lock(sync->mtx);
    // 页码越界（文档已切换）：publish_page 未接管纹理，需逐 tile 退役。
    if (page < 0 || static_cast<std::size_t>(page) >= store->pages.size()) {
        for (const PageSlot::Tile& t : loaded.tiles)
            if (t.texture) store->retired_pending.push_back(t.texture);
        return;
    }
    store->publish_page(page, std::move(loaded), bytes);
}

void PageScheduler::mark_failed(int page, DocError err, float scale) {
    std::lock_guard lock(sync->mtx);
    if (page < 0 || static_cast<std::size_t>(page) >= store->pages.size()) return;
    Entry& e = store->pages[static_cast<std::size_t>(page)];
    e.state.set_failed(scale);
    e.slot.error = err;
    // 保留已有纹理（ADR-024）：宁可继续显示旧图，也不闪回失败占位。
}

// ---- 缩略图 ----

void PageScheduler::handle_thumbs(const std::vector<int>& list, int target_px) {
    if (!docw->engine.is_open()) return;
    if (target_px < 32) target_px = 32;
    if (target_px > 1024) target_px = 1024;
    for (int p : list) {
        if (p < 0 || static_cast<std::size_t>(p) >= store->thumbs.size()) continue;
        PageSlot cur;
        bool stale = false;
        {
            std::lock_guard lock(sync->mtx);
            Entry& e = store->thumbs[static_cast<std::size_t>(p)];
            cur = e.slot;
            cur.status = e.state.status;
            stale = e.state.stale;
        }
        if (!stale && cur.status == PageStatus::Loaded && cur.scale > 0.0f) continue;
        render_thumb(p, target_px);
    }
}

// 缩略图的异常边界（同 render_one）：失败只影响这一张缩略图。
void PageScheduler::render_thumb(int page, int target_px) noexcept {
    try {
        render_thumb_impl(page, target_px);
    } catch (const std::exception& e) {
        lr::log::error("render", "thumb render threw " + lr::log::kv("page", page) + " " +
                                     lr::log::kv("what", e.what()));
        std::lock_guard lock(sync->mtx);
        if (page >= 0 && static_cast<std::size_t>(page) < store->thumbs.size()) {
            store->thumbs[static_cast<std::size_t>(page)].state.set_failed(0.0f);
            store->thumbs[static_cast<std::size_t>(page)].slot.error = DocError::Internal;
        }
    } catch (...) {
    }
}

// 按"旋转后"页尺寸算目标倍率，使最长边 ≈ target_px。
// 缩略图失败**不自动重试**（侧栏是辅助视图，失败就留空占位）；
// 视图变换变化或重新打开文档会重置状态、自然重试。
void PageScheduler::render_thumb_impl(int page, int target_px) {
    float w = docw->info.page_width_pt;
    float h = docw->info.page_height_pt;
    if (page >= 0 && static_cast<std::size_t>(page) < docw->info.page_sizes.size()) {
        const PageSize& ps = docw->info.page_sizes[static_cast<std::size_t>(page)];
        if (ps.width_pt > 0.0f && ps.height_pt > 0.0f) { w = ps.width_pt; h = ps.height_pt; }
    }
    if (!(w > 0.0f) || !(h > 0.0f)) { w = 595.0f; h = 842.0f; }
    if (docw->rotation == 90 || docw->rotation == 270) { const float t = w; w = h; h = t; }
    const float longest = w > h ? w : h;
    float scale = longest > 0.0f ? static_cast<float>(target_px) / longest : 1.0f;
    if (!(scale > 0.0f)) scale = 1.0f;

    {
        std::lock_guard lock(sync->mtx);
        if (page < 0 || static_cast<std::size_t>(page) >= store->thumbs.size()) return;
        Entry& e = store->thumbs[static_cast<std::size_t>(page)];
        if (e.state.status == PageStatus::Failed) return;  // 已失败，不再重试
        e.state.stale = false;                            // 本次已开始重渲
        e.state.status = PageStatus::Loading;             // 缩略图不复用页面的重试额度语义
    }

    PageBitmap bmp;
    DocError err = docw->engine.render_page(page, scale, bmp, 4096, docw->rotation,
                                            static_cast<PageScheme>(docw->scheme));
    SrvHandle srv;   // 未发布即析构 → 自动释放
    if (err == DocError::Ok &&
        !create_page_texture(device, bmp.samples(), bmp.width(), bmp.height(),
                             bmp.stride(), srv)) {
        err = DocError::Internal;
    }

    std::lock_guard lock(sync->mtx);
    if (page < 0 || static_cast<std::size_t>(page) >= store->thumbs.size()) return;  // srv 自动释放
    Entry& e = store->thumbs[static_cast<std::size_t>(page)];
    if (err != DocError::Ok) {
        e.state.set_failed(scale);
        e.slot.error = err;
        return;
    }
    PageSlot loaded;
    loaded.texture = srv.get();
    loaded.pixel_w = bmp.width();
    loaded.pixel_h = bmp.height();
    loaded.scale = bmp.effective_scale();
    if (!store->publish_thumb(page, std::move(loaded),
                              static_cast<std::size_t>(bmp.width()) *
                              static_cast<std::size_t>(bmp.height()) * 4u)) {
        return;  // srv 自动释放
    }
    srv.release_raw();   // 所有权已转入 store
}

void PageScheduler::retry_page(int page) {
    std::lock_guard lock(sync->mtx);
    if (page < 0 || static_cast<std::size_t>(page) >= store->pages.size()) return;
    Entry& e = store->pages[static_cast<std::size_t>(page)];
    if (e.state.manual_retry) return;  // 已在待重试队列
    e.state.request_retry();
    // 重新武装一次调度：否则 set_wanted 的去重会拦住"同参数"的再次投递
    wants = last_wants;
    wants_dirty = true;
    sync->cv.notify_all();
}

// ============================================================
//  Renderer 对外接口
// ============================================================

Renderer::Renderer(void* device) noexcept
    : impl_(new (std::nothrow) Impl(static_cast<ID3D11Device*>(device))) {
    if (impl_) impl_->start();
}

Renderer::~Renderer() {
    if (impl_) impl_->shutdown();
}

std::uint64_t Renderer::open(std::wstring path) {
    if (!impl_) return 0;
    std::lock_guard lock(impl_->sync.mtx);
    const std::uint64_t id = ++impl_->next_id;
    Command c;
    c.kind = Command::Kind::Open;
    c.path = std::move(path);
    c.id = id;
    impl_->cmd = std::move(c);
    impl_->docw.doc.phase = DocPhase::Opening;
    impl_->docw.doc.error = DocError::Ok;
    impl_->docw.doc.detail_u8.clear();
    impl_->docw.doc.id = id;
    impl_->sync.cv.notify_all();
    return id;
}

std::uint64_t Renderer::close() {
    if (!impl_) return 0;
    std::lock_guard lock(impl_->sync.mtx);
    const std::uint64_t id = ++impl_->next_id;
    Command c;
    c.kind = Command::Kind::Close;
    c.id = id;
    impl_->cmd = std::move(c);
    impl_->docw.doc = DocState{};
    impl_->docw.doc.phase = DocPhase::Idle;
    impl_->docw.doc.id = id;
    impl_->sync.cv.notify_all();
    return id;
}

std::uint64_t Renderer::authenticate(std::string_view utf8_password) {
    if (!impl_) return 0;
    std::lock_guard lock(impl_->sync.mtx);
    const std::uint64_t id = ++impl_->next_id;
    Command c;
    c.kind = Command::Kind::Authenticate;
    c.password.assign(utf8_password);
    c.id = id;
    impl_->cmd = std::move(c);
    // 与 open() 一致地把 phase 置为 Opening：否则 UI 会在工作线程真正处理之前，
    // 读到上一次 open 失败时残留的 Failed，把"正在认证"误判成结果（时序竞态）。
    impl_->docw.doc.phase = DocPhase::Opening;
    impl_->docw.doc.error = DocError::Ok;
    impl_->docw.doc.detail_u8.clear();
    impl_->docw.doc.id = id;
    impl_->sync.cv.notify_all();
    return id;
}

DocState Renderer::doc_state() const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->sync.mtx);
    return impl_->docw.doc;
}

PageSlot Renderer::slot(int page) const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->sync.mtx);
    return impl_->store.page_slot(page);
}

std::vector<OutlineItem> Renderer::outline() const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->sync.mtx);
    return impl_->docw.outline;
}

void Renderer::set_wanted(std::vector<RenderWant> wants) {
    if (!impl_) return;
    std::lock_guard lock(impl_->sync.mtx);
    if (wants_equal(impl_->sched.last_wants, wants)) return;  // 与上次相同，避免空转唤醒
    impl_->sched.last_wants = wants;
    impl_->sched.wants = std::move(wants);
    impl_->sched.wants_dirty = true;
    impl_->sync.cv.notify_all();
}

void Renderer::drain_retired() {
    if (!impl_) return;
    std::vector<void*> local;
    {
        std::lock_guard lock(impl_->sync.mtx);
        impl_->store.drain_swap(local);   // 本帧释放"上一帧之前"退役的
    }
    for (void* p : local) TextureStore::release_srv(p);
}

void Renderer::set_cache_budget(std::size_t bytes) {
    if (!impl_) return;
    std::lock_guard lock(impl_->sync.mtx);
    impl_->store.budget_bytes = clamp_cache_budget(bytes);
    // 预算调小可能需立即逐出：用上一帧的请求重跑一次调度（无请求时无可逐出页）
    if (!impl_->sched.last_wants.empty()) {
        impl_->sched.wants = impl_->sched.last_wants;
        impl_->sched.wants_dirty = true;
        impl_->sync.cv.notify_all();
    }
}

void Renderer::set_resource_tier(ResourceTier tier) {
    if (!impl_) return;
    const ResourceProfile p = lr::resource_profile(tier);
    std::lock_guard lock(impl_->sync.mtx);
    impl_->sched.tier = tier;
    impl_->sched.profile = p;
    impl_->store.budget_bytes = clamp_cache_budget(p.cache_bytes);
    if (!impl_->sched.last_wants.empty()) {
        impl_->sched.wants = impl_->sched.last_wants;
        impl_->sched.wants_dirty = true;
        impl_->sync.cv.notify_all();
    }
}

ResourceTier Renderer::resource_tier() const {
    if (!impl_) return ResourceTier::Balanced;
    std::lock_guard lock(impl_->sync.mtx);
    return impl_->sched.tier;
}

ResourceProfile Renderer::resource_profile() const {
    if (!impl_) return lr::resource_profile(ResourceTier::Balanced);
    std::lock_guard lock(impl_->sync.mtx);
    return impl_->sched.profile;
}

CacheStats Renderer::cache_stats() const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->sync.mtx);
    CacheStats s;
    s.budget_bytes = impl_->store.budget_bytes;
    s.used_bytes = impl_->store.used_bytes;
    s.evictions = impl_->store.evictions;
    for (const TextureStore::Entry& e : impl_->store.pages)
        if (e.bytes > 0) ++s.resident_pages;
    return s;
}

void Renderer::retry_page(int page) {
    if (!impl_) return;
    impl_->sched.retry_page(page);
}

// ---- 文本 / 图片 / 链接 ----

void Renderer::request_page_content(int page) {
    if (!impl_) return;
    impl_->content.request_page_content(page);
}

bool Renderer::take_page_content(int page, PageContent& out) {
    if (!impl_) return false;
    return impl_->content.take_page_content(page, out);
}

void Renderer::request_copy_text(int page, float ax, float ay, float bx, float by) {
    if (!impl_) return;
    impl_->content.request_copy_text(page, ax, ay, bx, by);
}

bool Renderer::take_copy_text(std::string& out) {
    if (!impl_) return false;
    return impl_->content.take_copy_text(out);
}

void Renderer::request_copy_image(int page, float x, float y) {
    if (!impl_) return;
    impl_->content.request_copy_image(page, x, y);
}

bool Renderer::take_copy_image(ImageData& out) {
    if (!impl_) return false;
    return impl_->content.take_copy_image(out);
}

// ---- 全文搜索 ----

void Renderer::start_search(std::string utf8_needle) {
    if (!impl_) return;
    impl_->search.start(std::move(utf8_needle));
}

void Renderer::cancel_search() {
    if (!impl_) return;
    impl_->search.cancel();
}

SearchStatus Renderer::search_status() const {
    if (!impl_) return {};
    return impl_->search.status();
}

void Renderer::take_search_hits(std::vector<SearchHit>& out) {
    if (!impl_) return;
    impl_->search.take(out);
}

// ---- 导出为图片 ----

void Renderer::start_export(ExportRequest req) {
    if (!impl_) return;
    impl_->exp.start(std::move(req));
}

void Renderer::cancel_export() {
    if (!impl_) return;
    impl_->exp.cancel();
}

ExportStatus Renderer::export_status() const {
    if (!impl_) return {};
    return impl_->exp.status();
}

void Renderer::clear_export_result() {
    if (!impl_) return;
    impl_->exp.clear_result();
}

// ---- 视图变换 ----

void Renderer::set_view_transform(int rotation_deg, PageScheme scheme) {
    if (!impl_) return;
    int rot = rotation_deg % 360;
    if (rot < 0) rot += 360;
    rot = (rot / 90) * 90;
    const int mode = static_cast<int>(scheme);
    if (mode < 0 || mode > 2) return;  // 非法配色忽略

    std::lock_guard lock(impl_->sync.mtx);
    if (impl_->docw.rotation == rot && impl_->docw.scheme == mode) return;
    const bool rot_changed = (impl_->docw.rotation != rot);
    impl_->docw.rotation = rot;
    impl_->docw.scheme = mode;

    // 保留本帧请求，作废/标记全部纹理后原样重新投递（触发整篇重渲）。
    const std::vector<RenderWant> keep_wants = impl_->sched.last_wants;
    const std::vector<int>        keep_thumbs = impl_->sched.last_thumb_pages;
    const int                     keep_px = impl_->sched.last_thumb_px;

    if (rot_changed) {
        // 旋转会交换版面宽高：旧纹理贴进新框会拉伸错位，只能作废重渲（页码/滚动已在 UI 侧重定位）。
        impl_->store.invalidate_all();
        impl_->sched.last_wants = keep_wants;
        impl_->sched.last_thumb_pages = keep_thumbs;
        impl_->sched.last_thumb_px = keep_px;
    } else {
        // 仅配色变化：版面不变，**保留旧纹理继续显示**，只标记待重渲，避免整篇瞬间白屏（ADR-042）。
        impl_->store.mark_all_stale();
        // last_wants / last_thumb_pages 保持原值：它们仍是"已投递"记录，去重语义不变。
    }
    impl_->sched.wants = keep_wants;
    impl_->sched.wants_dirty = !keep_wants.empty();
    impl_->sched.thumb_pages = keep_thumbs;
    impl_->sched.thumb_target_px = keep_px > 0 ? keep_px : impl_->sched.thumb_target_px;
    impl_->sched.thumbs_dirty = !keep_thumbs.empty();
    impl_->sync.cv.notify_all();
}

// ---- 缩略图通道 ----

void Renderer::set_thumbs_wanted(std::vector<int> pages, int target_px) {
    if (!impl_) return;
    if (target_px < 32) target_px = 32;
    if (target_px > 1024) target_px = 1024;
    std::lock_guard lock(impl_->sync.mtx);
    if (impl_->sched.last_thumb_px == target_px && impl_->sched.last_thumb_pages == pages) return;
    impl_->sched.last_thumb_pages = pages;      // 先拷贝再移动
    impl_->sched.last_thumb_px = target_px;
    impl_->sched.thumb_pages = std::move(pages);
    impl_->sched.thumb_target_px = target_px;
    impl_->sched.thumbs_dirty = true;

    // 退役**请求范围之外**的缩略图：否则长时间滚动后缩略图会无限累积
    // （每张约 100KB，1000 页即上百 MB），与"内存有界"的纪律相悖。
    // 侧栏请求的是一段连续区间，取其 [min,max] 即可。
    if (!impl_->sched.thumb_pages.empty()) {
        int lo = impl_->sched.thumb_pages.front();
        int hi = impl_->sched.thumb_pages.front();
        for (int p : impl_->sched.thumb_pages) {
            if (p < lo) lo = p;
            if (p > hi) hi = p;
        }
        for (int i = 0; i < static_cast<int>(impl_->store.thumbs.size()); ++i) {
            if (i >= lo && i <= hi) continue;
            TextureStore::Entry& e = impl_->store.thumbs[static_cast<std::size_t>(i)];
            if (e.bytes == 0 && e.slot.texture == nullptr) continue;
            impl_->store.retire_thumb_pixels(e);
            e.state.on_evicted();
            e.slot.error = DocError::Ok;
        }
    }
    impl_->sync.cv.notify_all();
}

PageSlot Renderer::thumb_slot(int page) const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->sync.mtx);
    return impl_->store.thumb_slot(page);
}

}  // namespace lr