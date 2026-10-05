此目录存放全新下载的第三方库源码（随仓库提交，锁定版本）：

- `imgui/` —— 最新版 ImGui（ocornut/imgui），使用 DX11 + Win32 后端 + freetype
- `implot/` —— 最新版 ImPlot（epezent/implot），最终产品以自研画布为主，
  ImPlot 作为交互手感参考基准与调试可视化备用

下载方式（示例）：
  git clone --depth 1 https://github.com/ocornut/imgui.git third_party/imgui
  git clone --depth 1 https://github.com/epezent/implot.git third_party/implot

注意：clone 后移除其内部 .git 目录再提交，避免嵌套仓库。
