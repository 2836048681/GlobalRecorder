# 来源与依赖

本项目起源于 QuickAudioRecorder：

- 上游：https://github.com/lukmay/QuickAudioRecorder
- 作者：Lukas Mayr
- 上游许可：MIT，完整版权及许可保留在 `LICENSE`。

此源码包保留后续原生 C++ 实现、可选本地转写桥接和对应合成测试，不包含原 Python/Qt 桌面应用、其历史工作流或本机运行数据。

原生程序调用 Windows 系统 API；使用 Zig 0.15.2 编译，不在源码包中分发编译器或 Windows SDK。

可选转写依赖 faster-whisper 1.2.1，其依赖及模型须单独获取并遵守各自许可。首次下载功能由用户确认后获取 Python 3.11.9 嵌入式运行组件、pip 25.0.1、转写依赖与固定版本的 tiny 模型；固定来源及校验值见 `native/asr_downloads.json`。本仓库和便携包不捆绑模型权重、第三方 Python 环境或用户音频。

`native/assets/` 中的声波图标由本项目代码绘制，生成逻辑位于 `native/shell_support.cpp`。
