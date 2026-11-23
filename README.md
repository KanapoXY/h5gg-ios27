# H5GG iOS 27.3 改装版

基于 H5GG v7.9 的第三方设备分支，目标是让 H5GG 在 iOS 27.3 Dev Beta1（无越狱、企业证书签名侧加载注入）环境下正常工作。

本分支仅修改运行时兼容层，引擎核心（内存搜索/JSExport API/跨进程逻辑）保持上游原实现。仅供个人在自己的设备上调试单机游戏使用，请勿用于联机游戏或其他违规用途。

## 致谢

- **上游项目**：H5GG（社区：Discord FAs4MH7HMc / iosgods.com）
- **本分支架构工作**：UIWebView→WKWebView 迁移、同步桥设计与实现
