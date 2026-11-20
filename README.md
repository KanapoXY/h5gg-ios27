# H5GG iOS 27.3 适配版

基于 [H5GG v7.9](https://github.com/H5GG/H5GG) 的第三方适配分支，目标是让 H5GG 在 **iOS 27.3 Dev Beta**（无越狱、企业证书签名侧载注入）环境下正常工作。

> 本分支只修改运行时兼容层，引擎核心（内存搜索 / JSExport API / 跨进程逻辑）保持上游原实现。仅供个人在自己的设备上调试单机游戏使用，请勿用于联机游戏或其他违规用途。

## 为什么需要适配

iOS 27 彻底废弃了 UIWebView。原版 H5GG 的菜单界面基于 UIWebView + `webView:didCreateJavaScriptContext:forFrame:` swizzle 拿页面 JSContext，在 iOS 27 上**调用即闪退**（App 本体与注入均正常，崩点锁定 UIWebView 层）。而 WKWebView 出于安全模型设计**不再暴露 JSContext**，这是整个适配的核心矛盾。

## 架构（v6）

```
┌─ 菜单页面 (WKWebView) ─────────────────────────────┐
│  window.h5gg  — 同步语义 shim                       │
│  内存/窗口 API → prompt("__h5gg_sync__"+JSON)        │
│      ↓ 同步阻塞                                     │
│  WKUIDelegate runJavaScriptTextInputPanelWithPrompt │
│      → 引擎队列执行 → 返回值直接参与页面运算          │
│                                                     │
│  事件类 API (setButtonAction/pickScriptFile 等)      │
│      → messageHandler 异步通道 (Promise)             │
├─ 脚本引擎 (原生 JSContext, 串行队列) ────────────────┤
│  用户 .js 脚本 + h5gg JSExport 对象                  │
│  旧式同步脚本 API 100% 兼容                          │
└─────────────────────────────────────────────────────┘
```

关键设计：

- **v6 同步桥**：页面 `prompt()` 被原生 delegate 同步拦截，实现 JS↔原生同步调用。市面主流同步混淆菜单（如 2K 系列指针链菜单）**零改动可运行**。3 秒超时保护防止引擎队列被长脚本占用时卡死 UI。
- **双通道**：只有必须 UI 交互的 API（setButtonAction / pickScriptFile / loadScript）保留异步 messageHandler。
- **file:// 支持**：WKWebView 禁止本地文件导航，已改走 `loadFileURL:allowingReadAccessToURL:`（包内授权整个 .app 目录，包外授权所在目录）。
- **旧 .js 脚本**：走原生 JSContext 引擎，`h5gg.*` API 同步语义完全不变。

## 版本历史

| 版本 | 主要变更 |
|------|----------|
| v1 | UIWebView → WKWebView 重写，双通道桥接，解决 iOS 27.3 闪退 |
| v2 | 补 setWindowRect/setWindowDrag 等裸全局 shim，修复菜单窗口卡 370×370 |
| v3 | 修复点击悬浮图标无法开关菜单（gHasButtonAction 分流逻辑） |
| v4 | getRangesList 等可选参数过桥修复；file:// 导航放行 |
| v5 | 内存 API 改原生 block 转发，绕开 JSC 对 JSExport 实例方法提取的 self 类型校验限制 |
| v6 | **prompt 同步桥**，页面侧 h5gg.* 全同步语义，第三方同步混淆菜单零改动支持 |

## 云编译

`.github/workflows/build.yml` 基于 GitHub Actions macOS runner + Theos 自动构建：

1. 手动触发 Actions 中的 `Build H5GG (iOS 27 adaptation)`
2. 构建完成后在 Artifacts 下载 `H5GG-iOS27-adapted`（内含 `H5GG.dylib` 与 deb）

产物为 ARM64 Mach-O，最低支持 iOS 11.4，链接 WebKit + JavaScriptCore（不含 UIWebView）。

## 使用

1. 将 `H5GG.dylib` 注入目标 ipa（TrollFools / 预签注入工具均可）
2. 用企业证书（如 lcsign）对注入后的 ipa 重签并侧载
3. 首次信任证书后启动，游戏内出现 H5GG 悬浮图标
4. 菜单支持加载本地 HTML 或在线 URL；跨进程功能需按菜单引导授权

已知注意点：

- 页面若使用 FastClick 会被检测并提示移除（UI 假死风险）
- .html 脚本文件建议使用 LF 或 CRLF 换行符，否则错误行号提示不准
- iOS 27 企业签存在已知启动 bug（论坛 836007），遇到 App 打不开可尝试卸载重装并重新信任

## 自带资源

- `Index.html` / `Index-en.html`：默认主菜单（已同步化）
- `h5gg-js-doc.js` / `h5gg-js-doc-en.js`：引擎 API 文档
- 上游仓库的 `examples-HTML5/`、`examples-JavaScript/` 仍可作为菜单模板参考

## 致谢

上游项目：[H5GG](https://github.com/H5GG/H5GG)（社区：Discord FAs4MH7HMc / iosgods.com）
本分支适配工作：UIWebView→WKWebView 迁移、同步桥设计与实现。
