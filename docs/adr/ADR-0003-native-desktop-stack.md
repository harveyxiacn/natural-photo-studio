# ADR-0003：原生桌面与隔离 Worker 技术路线

- 状态：Proposed
- 日期：2026-07-25
- 关联：CORE-001 至 CORE-012、SEC-004、SEC-006

## 背景

专业照片编辑要求：

- 24–100MP、高位深和广色域；
- 低延迟画笔和多显示器；
- GPU 纹理、压感笔、ICC/HDR；
- 大型项目、后台导出和崩溃恢复；
- 不可信编解码、模型和插件隔离；
- Windows/macOS。

纯 Web UI 开发快，但原生纹理共享、颜色、输入和内存控制风险更高；纯自绘 UI 会显著增加无障碍与常规桌面交互成本。

## 决定

当前候选：

- C++23 核心；
- Qt 6/QML 桌面 UI；
- CMake/Ninja 与锁定依赖；
- 自有 `ComputeBackend`，技术尖峰选择 D3D12/Metal/Vulkan 或 Dawn；
- OpenColorIO/LittleCMS 类库封装颜色；
- RAW/编解码器通过应用接口；
- ONNX Runtime 类模型抽象；
- Codec、AI、Plugin 和 Updater 为隔离进程；
- IPC 使用版本化协议；
- 第三方插件优先 WASM/WASI。

第三方库不是工程格式的一部分，可替换。

## 结果

正面：

- 对 GPU、内存、颜色和输入有直接控制；
- Qt 提供成熟桌面、多语言和无障碍基础；
- C/C++ 图像生态丰富；
- Worker 隔离降低未知输入风险；
- 能提供无 GPU 的 CPU 参考路径。

代价：

- C++ 内存安全要求严格工具链和评审；
- Qt 与自有 GPU 纹理互操作有技术风险；
- Windows/macOS 后端与打包成本高；
- 相比 Web UI 迭代速度较慢。

## 被保留的替代方案

### Rust 核心 + Web UI/Tauri

内存安全和 UI 开发友好，但专业画布、无障碍、颜色和原生纹理共享需尖峰验证。若 C++/Qt 核心风险不达标，可重新评审。

### Electron + 原生引擎

生态成熟，但进程内存、色彩链路和纹理互操作成本较高。

### 纯 C++ 自绘 UI

性能可控，但无障碍、文字输入、国际化和面板系统成本不可接受。

## 验证门槛

在接受前必须完成：

- 45MP 16 位画布共享纹理；
- 20ms 笔刷反馈和 60/30fps 视口；
- 双显示器 ICC 与 SDR/HDR；
- Windows 三家 GPU + Apple Silicon；
- 100MP 分块；
- Worker IPC 吞吐；
- GPU reset 恢复；
- Qt 200% UI 缩放和键盘/读屏基础。

任一关键门槛失败时重新评估 UI 或 GPU 桥，而不改变项目和命令契约。
