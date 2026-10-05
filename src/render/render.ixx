// render.ixx — Lilith Reader 渲染调度层（Phase 3）
//
// 职责：衔接 document 与 canvas。架构文档 §1 的依赖方向要求
//       document 与 canvas **互不 import**，由本层居中调度，为将来替换
//       MuPDF 为 PDFium 预留解耦（ADR-003）。
//
// 本层做三件事：
//   1. **线程归属**：单工作线程独占唯一的 Document；UI 线程零 fz_* 调用、
//      零阻塞（延续 ADR-006/ADR-012）。
//   2. **页状态机**：Unloaded → Loading → Loaded / Failed（架构文档 §3.2）。
//      Phase 2 的 DocSession 临时线程在本阶段被本层取代。
//   3. **纹理上传与生命周期**：渲染线程用 ID3D11Device 直接建纹理
//      （D3D11 的 device 方法可多线程调用），产出即释放 PageBitmap；
//      被替换/逐出的纹理进**退役队列**，由 UI 线程在帧首 drain_retired() 释放
//      —— 杜绝"删除正在被本帧 draw list 引用的纹理"（架构文档 §3.3）。
//
// 纪律：
//   · 本模块接口不出现 ImGui/D3D 类型：纹理以不透明 void* 暴露，UI 侧自行转
//     ImTextureID（= ID3D11ShaderResourceView*）。UI **只读**该句柄，绝不释放。
//   · 缓存策略：Phase 3 采用"保留窗口"（可见 ± 预加载）逐出窗口外页；
//     按**字节预算**的 LRU 在 Phase 4 替换本策略，接口不变。

module;

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

export module lilithreader.render;

// 重新导出 document：本模块的公开类型（DocState/PageSlot）含 DocumentInfo/DocError，
// 让 import render 的翻译单元一并可见（避免"导出声明引用非导出模块类型"）。
export import lilithreader.document;

export namespace lr {

// ---- 单页状态机 ----
enum class PageStatus : int {
    Unloaded = 0,  // 无纹理（尚未请求，或已被逐出）
    Loading,       // 渲染中
    Loaded,        // 纹理可用
    Failed,        // 渲染失败（失败占位 UI 已实现；点击重试/自动重试为 Phase 4 计划）
};

// UI 线程读取的页快照（值拷贝，不持有所有权）
struct PageSlot {
    PageStatus status = PageStatus::Unloaded;
    void*      texture = nullptr;  // ID3D11ShaderResourceView*，不透明；UI 只读
    int        pixel_w = 0;
    int        pixel_h = 0;
    float      scale = 0.0f;       // 实际使用的渲染倍率（可能因尺寸上限被下调）
    DocError   error = DocError::Ok;
};

// ---- 文档级状态 ----
enum class DocPhase : int { Idle, Opening, Ready, Failed };

struct DocState {
    DocPhase      phase = DocPhase::Idle;
    DocError      error = DocError::Ok;
    DocumentInfo  info{};
    std::string   detail_u8;   // 失败时的原始错误信息（UTF-8）
    std::uint64_t id = 0;      // 请求序号，UI 用它丢弃过期结果
};

// 一次渲染请求：把某页渲染到目标倍率（倍率 1.0 = 72dpi）
struct RenderWant {
    int   page = -1;
    float scale = 1.0f;
};

// ---- 渲染调度器 ----
class Renderer {
public:
    // device 为 ID3D11Device*（不透明：接口不引入 d3d11.h，保持后端解耦）。
    // 设备必须比本对象活得久（约定：UI 在释放 D3D 设备之前先销毁本对象）。
    explicit Renderer(void* device) noexcept;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // 投递命令（UI 线程）。返回请求序号。
    std::uint64_t open(std::wstring path);
    std::uint64_t close();
    std::uint64_t authenticate(std::string_view utf8_password);

    // 读取快照（UI 线程）
    [[nodiscard]] DocState doc_state() const;
    [[nodiscard]] int      page_count() const;
    [[nodiscard]] PageSlot slot(int page) const;

    // 声明本帧需要的页（可见 + 预加载，按优先级排序）。渲染线程去重、
    // 排队、换入；已在足够倍率上 Loaded 的页会被跳过。
    void set_wanted(std::vector<RenderWant> wants);

    // 帧首调用：释放上一帧退役的纹理。必须在构建本帧 draw list 之前调用。
    void drain_retired();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lr
