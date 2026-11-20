// h5gg-shim.js — WKWebView 桥接层 v6 (iOS 27 适配, 同步语义)
// 由 Tweak.mm 以 WKUserScript 方式注入 document start
//
// v6 核心变化: 引擎内存 API 全面改为同步语义 (与原版 H5GG UIWebView+JSContext 行为一致)
//   通道: 页面 prompt("__h5gg_sync__"+JSON) -> WKUIDelegate 同步拦截 -> 引擎队列执行 -> JSON 返回
//   目的: 兼容第三方同步混淆菜单 (直接使用返回值做指针链运算, 如 2K 系列菜单)
// 事件类 API (setButtonAction/setLayoutAction/pickScriptFile) 与 loadScript 仍走异步 messageHandler
(function () {
    if (window.__h5gg_shim_installed) return;
    window.__h5gg_shim_installed = true;

    var seq = 0;
    var pending = {};   // cbid -> {resolve, reject} (异步通道)
    var fnTable = {};   // fnid -> function (页面回调函数)
    var evtTable = {};  // name  -> function (按钮/布局事件)

    function nextId() { return ++seq; }

    // ---- 同步桥 ----
    var SYNC_PREFIX = '__h5gg_sync__';
    function syncCall(api, args) {
        var resp;
        try {
            resp = window.prompt(SYNC_PREFIX + JSON.stringify({ api: api, args: args || [] }));
        } catch (e) {
            throw new Error('h5gg sync bridge call failed: ' + e);
        }
        if (resp === null || resp === undefined)
            throw new Error('h5gg sync bridge unavailable (native delegate missing?)');
        var r;
        try { r = JSON.parse(resp); } catch (e2) {
            throw new Error('h5gg sync bridge bad response');
        }
        if (!r || r.ok !== true) throw new Error((r && r.error) || 'h5gg sync bridge error');
        // 引擎返回值在 data 中以 JSON 编码二次封装, 需再解一层
        // 注意: undefined 与 null 过桥后统一为 null, 保持 Number(null)===0 的旧语义 (第三方菜单依赖此判断)
        if (r.data === null || r.data === undefined) return null;
        if (typeof r.data === 'string') {
            try { return JSON.parse(r.data); }
            catch (e3) { return r.data; } //不可 JSON 化的返回值已 toString
        }
        return r.data;
    }

    // ---- 异步通道 (仅事件注册与脚本加载使用) ----
    function callAsync(api, args) {
        return new Promise(function (resolve, reject) {
            var cbid = nextId();
            pending[cbid] = { resolve: resolve, reject: reject };
            try {
                var list = (args === undefined || args === null) ? [] : args;
                window.webkit.messageHandlers.h5ggBridge.postMessage({
                    api: api, args: JSON.parse(JSON.stringify(list)), cbid: cbid
                });
            } catch (e) {
                delete pending[cbid];
                reject(e);
            }
        });
    }

    // 原生回写结果 (异步通道)
    window.__h5gg_resp = function (cbid, ok, data) {
        var p = pending[cbid];
        if (!p) return;
        delete pending[cbid];
        if (ok) {
            try { p.resolve(data === null || data === undefined ? null : JSON.parse(data)); }
            catch (e) { p.resolve(data); }
        } else {
            p.reject(new Error(data || 'h5gg bridge error'));
        }
    };

    // 原生主动触发页面事件 (setButtonAction / setLayoutAction)
    window.__h5gg_evt = function (name) {
        var fn = evtTable[name];
        if (!fn) return;
        var args = Array.prototype.slice.call(arguments, 1);
        try { fn.apply(null, args); } catch (e) { console.error('h5gg evt error:', e); }
    };

    // 函数参数回调回写 (pickScriptFile 等)
    window.__h5gg_fnresp = function (fnid) {
        var fn = fnTable[fnid];
        if (!fn) return;
        delete fnTable[fnid];
        var args = Array.prototype.slice.call(arguments, 1);
        try { fn.apply(null, args); } catch (e) { console.error('h5gg fnresp error:', e); }
    };

    var shim = {};

    // ---- 引擎内存 API: 全部同步语义 (返回真实值, 第三方同步菜单可直接参与运算) ----
    shim.require = function (v) { return syncCall('require', [v]); };
    shim.setFloatTolerance = function (v) { return syncCall('setFloatTolerance', [v]); };
    shim.searchNumber = function (value, type, from, to) { return syncCall('searchNumber', [value, type, from, to]); };
    shim.searchNearby = function (value, type, range) { return syncCall('searchNearby', [value, type, range]); };
    shim.getValue = function (addr, type) { return syncCall('getValue', [addr, type]); };
    shim.setValue = function (addr, value, type) { return syncCall('setValue', [addr, value, type]); };
    shim.editAll = function (value, type) { return syncCall('editAll', [value, type]); };
    shim.getResults = function (maxCount, skip) { return syncCall('getResults', [maxCount, skip]); };
    shim.getResultsCount = function () { return syncCall('getResultsCount', []); };
    shim.clearResults = function () { return syncCall('clearResults', []); };
    shim.getLocalScripts = function () { return syncCall('getLocalScripts', []); };
    //可选参数 undefined 不能过桥 (会被 JSON 化成 null 使引擎侧 JSValue 判断失效), 空参直接省略
    shim.getRangesList = function (filter) { return syncCall('getRangesList', filter === undefined ? [] : [filter]); };
    shim.getProcList = function (filter) { return syncCall('getProcList', filter === undefined ? [] : [filter]); };
    shim.setTargetProc = function (pid) { return syncCall('setTargetProc', [pid]); };
    shim.makeTweak = function (icon, html) { return syncCall('makeTweak', [icon, html]); };

    // ---- 窗口/悬浮按钮控制: Tweak.mm actions 同样运行于引擎上下文, 同步语义贴合原版 ----
    // 同时注册为 window 全局, 保证官方示例模板 (直接调用 setWindowDrag 等裸全局) 零改动兼容
    ['setWindowRect', 'setWindowDrag', 'setWindowTouch', 'setWindowVisible',
     'setButtonImage', 'setButtonOffset', 'closeMenu'].forEach(function (name) {
        shim[name] = function () { return syncCall(name, Array.prototype.slice.call(arguments)); };
        if (window[name] === undefined) window[name] = shim[name];
    });

    // ---- 事件/加载类 API: 保留异步通道 (需要 UI 交互或页面回调, 无法同步) ----
    shim.setButtonAction = function (fn) {
        evtTable['buttonAction'] = fn;
        return callAsync('setButtonAction', []);
    };
    shim.setLayoutAction = function (fn) {
        evtTable['layoutAction'] = fn;
        return callAsync('setLayoutAction', []);
    };
    shim.pickScriptFile = function (fn, types) {
        var fnid = nextId();
        fnTable[fnid] = fn;
        return callAsync('pickScriptFile', [{ __fn: fnid }, types || undefined]);
    };

    // loadPlugin: 跨桥无法返回可交互的插件对象, HTML 界面下不再支持
    shim.loadPlugin = function () {
        return Promise.reject(new Error('loadPlugin is not supported in WKWebView UI mode; use .js scripts instead'));
    };

    // 旧引擎把这两个注册为页面裸全局, 第三方菜单普遍直接调用, 必须注册到 window
    if (window.setButtonAction === undefined) window.setButtonAction = shim.setButtonAction;
    if (window.setLayoutAction === undefined) window.setLayoutAction = shim.setLayoutAction;

    // ---- 脚本加载: .js 走原生 JSContext (同步 API 完全兼容), .html 加载为菜单界面 ----
    shim.loadScript = function (path) { return callAsync('loadScript', [path]); };

    window.h5gg = shim;

    // 供原生主动调用的 alert 转发 (原生 jscontext 里的 alert 走这里)
    window.h5gg_alert = function (msg) { window.alert(msg); };
})();
