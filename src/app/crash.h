// crash.h — Lilith Reader 进程级崩溃防线
//
// 分层位置（架构文档 §3.4）：
//   · 模块边界（document 的 fz_try/catch）—— 已实现
//   · 线程边界（render 工作线程 try/catch）—— 见 render.cpp
//   · 帧边界（每块 UI 区域的 try/catch）—— 见 ui.cpp / main.cpp
//   · **进程边界（本文件）**—— 最后一道网：任何漏到顶层的异常/信号都留痕、留转储、不再静默消失
//
// 安装什么：
//   1. std::set_terminate            —— 未捕获的 C++ 异常（含线程入口逃逸、noexcept 违约）
//   2. SetUnhandledExceptionFilter   —— 访问违规/除零等 SEH 崩溃
//   3. _set_purecall_handler         —— 纯虚调用
//   4. _set_invalid_parameter_handler—— CRT 参数校验失败
//
// 产出（exe 同目录 crash\）：
//   · LilithReader_<时间戳>.dmp —— MiniDump（含异常上下文、线程信息、数据段）
//   · last_crash.txt            —— 人类可读摘要 + 日志尾部若干行（下次启动据此提示用户）
//   · running.flag              —— 运行中标记；正常退出时删除。若它残留却没有 last_crash.txt，
//                                  说明上次是被强杀/断电（没有机会执行处理器），同样会提示。
//
// 关键约束：
//   · 崩溃处理器**不使用** lr::log（堆与运行库可能已损坏），只用 Win32 原生 API + 栈缓冲；
//   · dbghelp.dll 动态加载并限定从 System32 加载（LOAD_LIBRARY_SEARCH_SYSTEM32），
//     避免 DLL 劫持，也不给正常路径增加任何依赖；
//   · 处理器**只记录、不尝试恢复**。use-after-free / 数据竞争 / 栈破坏属于未定义行为，
//     不可能可靠地"捕获后继续运行"—— 这类问题靠所有权约束、静态分析与这份转储去定位。

#pragma once

#include <string>

namespace lr::app::crash {

// 安装进程级崩溃防线。必须在入口尽早调用（早于任何可能抛异常的代码）。
// 可重复调用（第二次无操作）。任何失败都静默，不影响主流程。
void install() noexcept;

// 退出前调用：标记本次是正常退出（删除 running.flag）。
void mark_clean_exit() noexcept;

// 上次运行是否异常结束。若是，返回 true 并把摘要写入 summary_utf8（多行，UTF-8），
// 同时把标记归档为 last_crash.reported.txt，保证只提示一次。
// **必须在 install() 之前调用**：install 会写 running.flag，若排在它之后就会看到本次进程
// 自己的标记（即使顺序不当也不会误报——take_last_crash 内部有 PID 自检兜底，见实现）。
[[nodiscard]] bool take_last_crash(std::string& summary_utf8) noexcept;

// 崩溃报告目录（exe 同目录 crash\）。供 UI「打开报告文件夹」。
[[nodiscard]] std::wstring report_dir() noexcept;

// ---- 崩溃现场的"阶段"标记（纯诊断，零分配）----
//
// 传入的必须是**字面量或生命周期覆盖整个进程**的字符串（只存指针，不拷贝），
// 因为崩溃时可能在任何时刻读取它。用途：转储摘要里能看出"崩在哪一步"。
// 例：crash::set_phase("render"), crash::set_phase("open"), crash::set_phase("frame")。
void set_phase(const char* phase) noexcept;
[[nodiscard]] const char* phase() noexcept;

// 当前是否处于"崩溃处理器已接管"状态（仅自诊断/测试用）。
[[nodiscard]] bool handling() noexcept;

#ifndef NDEBUG
// 测试专用：主动触发一次崩溃，用来验证"留痕 + 转储 + 下次启动提示"整条链路。
//   kind 0 = 抛未捕获 C++ 异常（走 set_terminate）
//   kind 1 = 主动访问违规（走 SEH）
// Release 构建**不编译**本函数，故发布版没有任何可触发的崩溃入口。
// 注意 kind 0 会真的抛异常，调用方必须保证它**不在任何 try/catch 内**被调用。
void debug_trigger(int kind);
#endif

}  // namespace lr::app::crash
