// render.cpp — lilithreader.render 的实现单元（Phase 3）
//
// 线程模型（架构文档 §3.1）：
//   · UI 线程：只投递命令/渲染请求、读取快照；零 fz_*、零阻塞。
//   · 工作线程（本文件）：独占 Document；渲染 → 建纹理 → 发布快照。
//   · 纹理生命周期：渲染线程建纹理（ID3D11Device 方法可多线程调用），
//     被替换/逐出的纹理进退役队列；UI 线程帧首 drain_retired() 释放，
//     保证"正在被本帧 draw list 引用的纹理"不会被提前删除。
//
// 本文件不含任何 fz_* 调用（只经 Document/PageBitmap 的公开接口），
// 故不受 ADR-009 的 setjmp 纪律约束。

module;

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <climits>
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

namespace lr {
namespace {

// 保留窗口：可见范围外扩这么多页的纹理暂不逐出，避免来回滚动反复重渲。
// Phase 4 将改为按字节预算的 LRU，本常量随之废弃。
constexpr int kRetainMargin = 4;

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

    DocState              doc;
    std::vector<PageSlot> slots;
    std::vector<void*>    retired;  // 待释放的 SRV

    Document     doc_engine;  // 仅工作线程访问
    std::jthread worker;

    void start() { worker = std::jthread([this](std::stop_token st) { run(std::move(st)); }); }

    void shutdown() {
        worker.request_stop();
        cv.notify_all();
        if (worker.joinable()) worker.join();
        // 工作线程已停，此后不会再有新退役项；统一释放全部纹理
        for (PageSlot& s : slots) release_srv(s.texture);
        slots.clear();
        for (void* p : retired) release_srv(p);
        retired.clear();
    }

    static void release_srv(void* p) {
        if (p) static_cast<ID3D11ShaderResourceView*>(p)->Release();
    }

    // ---- 加锁访问辅助 ----

    // 把 tex 移入退役队列（调用者须持锁）
    void retire_locked(void*& tex) {
        if (tex) {
            retired.push_back(tex);
            tex = nullptr;
        }
    }

    void clear_slots_locked() {
        for (PageSlot& s : slots) retire_locked(s.texture);
        slots.clear();
    }

    void publish_doc(const DocState& s) {
        std::lock_guard lock(mtx);
        doc = s;
    }

    // 标记某页进入 Loading：**只改状态，绝不动 texture / scale**。
    //
    // 这里是"缩放闪烁"的根因所在（第三轮调试实测）：旧实现在此用 PageSlot{} 覆盖
    // 整个槽位，于是重渲染一开始旧纹理就从快照里消失 —— UI 先退回"载入中…"占位框，
    // 等新纹理就绪再换入，肉眼就是闪一下。保留旧纹理后，UI 在重渲染期间继续显示它
    // （双线性放大，略糊但不闪），新纹理就绪时在同一把锁内原子替换。
    // 顺带修掉一个更严重的隐患：旧实现把旧 SRV 指针直接覆盖掉、且不入退役队列，
    // **每重渲染一次就泄漏一个纹理**（缩放越频繁，显存涨得越快）。
    void mark_loading(int page) {
        std::lock_guard lock(mtx);
        if (page >= 0 && static_cast<std::size_t>(page) < slots.size())
            slots[page].status = PageStatus::Loading;
    }

    // 标记某页渲染失败：**保留已有纹理**（若曾成功渲染过），宁可继续显示旧图，
    // 也不要闪回"渲染失败"占位框；从未渲染成功的页 texture 为空，UI 自然显示失败占位。
    // 下一次 wants 变化会再尝试（Phase 4 补退避与重试次数上限）。
    void mark_failed(int page, DocError err) {
        std::lock_guard lock(mtx);
        if (page < 0 || static_cast<std::size_t>(page) >= slots.size()) return;
        slots[page].status = PageStatus::Failed;
        slots[page].error = err;
    }

    PageSlot slot_snapshot(int page) const {
        std::lock_guard lock(mtx);
        if (page >= 0 && static_cast<std::size_t>(page) < slots.size()) return slots[page];
        return {};
    }

    // ---- 工作线程主循环 ----

    void run(std::stop_token st) {
        for (;;) {
            std::optional<Command>  c;
            std::vector<RenderWant> w;
            bool have_wants = false;
            {
                std::unique_lock lock(mtx);
                cv.wait(lock, st, [this] { return cmd.has_value() || wants_dirty; });
                if (st.stop_requested()) return;
                if (cmd) {                      // 命令优先，渲染请求留到下一轮
                    c = std::move(cmd);
                    cmd.reset();
                } else if (wants_dirty) {
                    w = std::move(wants);
                    wants.clear();
                    wants_dirty = false;
                    have_wants = true;
                }
            }
            if (c) handle_command(*c);
            else if (have_wants) handle_wants(w);
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
            clear_slots_locked();
            last_wants.clear();
        }

        DocState s;
        s.id = c.id;
        const DocError err = doc_engine.open(c.path);
        if (err == DocError::Ok) {
            DocumentInfo info;
            const DocError ierr = doc_engine.info(info);
            if (ierr == DocError::Ok) {
                s.phase = DocPhase::Ready;
                s.info = info;
                std::lock_guard lock(mtx);
                slots.assign(static_cast<std::size_t>(info.page_count > 0 ? info.page_count : 0),
                             PageSlot{});
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
        clear_slots_locked();
        last_wants.clear();
        doc = s;
    }

    void do_auth(const Command& c) {
        DocState s;
        s.id = c.id;
        const DocError err = doc_engine.authenticate(c.password);
        if (err == DocError::Ok) {
            DocumentInfo info;
            const DocError ierr = doc_engine.info(info);
            if (ierr == DocError::Ok) {
                s.phase = DocPhase::Ready;
                s.info = info;
                std::lock_guard lock(mtx);
                clear_slots_locked();
                slots.assign(static_cast<std::size_t>(info.page_count > 0 ? info.page_count : 0),
                             PageSlot{});
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

        // 保留窗口外的已加载页逐出（Phase 3 策略；Phase 4 换字节预算 LRU）
        int lo = INT_MAX, hi = INT_MIN;
        for (const RenderWant& x : w) {
            if (x.page < 0) continue;
            if (x.page < lo) lo = x.page;
            if (x.page > hi) hi = x.page;
        }
        if (lo != INT_MAX) {
            lo -= kRetainMargin;
            hi += kRetainMargin;
        }
        evict_outside(lo, hi);

        for (const RenderWant& x : w) {
            if (x.page < 0 || !(x.scale > 0.0f)) continue;
            const PageSlot cur = slot_snapshot(x.page);
            if (cur.status == PageStatus::Loading) continue;      // 已在渲染
            if (cur.status == PageStatus::Loaded && cur.scale >= x.scale * 0.999f)
                continue;                                         // 已够清晰
            render_one(x.page, x.scale);
        }
    }

    void evict_outside(int lo, int hi) {
        std::lock_guard lock(mtx);
        for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
            if (i >= lo && i <= hi) continue;
            if (slots[i].texture) {
                retire_locked(slots[i].texture);
                slots[i] = PageSlot{};
            }
        }
    }

    void render_one(int page, float scale) {
        mark_loading(page);  // 保留旧纹理，见 mark_loading 注释

        PageBitmap bmp;
        const DocError err = doc_engine.render_page(page, scale, bmp);
        if (err != DocError::Ok) {
            mark_failed(page, err);
            return;
        }

        ID3D11ShaderResourceView* srv = nullptr;
        if (!create_page_texture(device, bmp.samples(), bmp.width(), bmp.height(),
                                 bmp.stride(), &srv)) {
            mark_failed(page, DocError::Internal);
            return;
        }

        PageSlot loaded;
        loaded.status = PageStatus::Loaded;
        loaded.texture = srv;
        loaded.pixel_w = bmp.width();
        loaded.pixel_h = bmp.height();
        loaded.scale = bmp.effective_scale();

        std::lock_guard lock(mtx);
        if (page >= 0 && static_cast<std::size_t>(page) < slots.size()) {
            retire_locked(slots[page].texture);  // 旧纹理入退役队列（由 UI 帧首释放）
            slots[page] = loaded;
        } else {
            retired.push_back(srv);  // 文档已切换，结果作废
        }
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
    impl_->doc.id = id;
    impl_->cv.notify_all();
    return id;
}

DocState Renderer::doc_state() const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->mtx);
    return impl_->doc;
}

int Renderer::page_count() const {
    if (!impl_) return 0;
    std::lock_guard lock(impl_->mtx);
    return static_cast<int>(impl_->slots.size());
}

PageSlot Renderer::slot(int page) const {
    if (!impl_) return {};
    return impl_->slot_snapshot(page);
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
        local.swap(impl_->retired);
    }
    for (void* p : local) Impl::release_srv(p);
}

}  // namespace lr
