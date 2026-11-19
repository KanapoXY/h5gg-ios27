// h5gg-shim.js — WKWebView 桥接层 (iOS 27 适配)
// 由 Tweak.mm 以 WKUserScript 方式注入 document start
// 原生侧通过 evaluateJavaScript 回写结果: __h5gg_resp / __h5gg_evt / __h5gg_fnresp
(function () {
    if (window.__h5gg_shim_installed) return;
    window.__h5gg_shim_installed = true;

    var seq = 0;
    var pending = {};   // cbid -> {resolve, reject}
    var fnTable = {};   // fnid -> function (页面回调函数)
    var evtTable = {};  // name  -> function (按钮/布局事件)

    function nextId() { return ++seq; }

    function serialize(args) {
        // 允许传普通值: [a, b, ...]; 内部把函数参数转为 {__fn:id} 占位
        var list = (args === undefined || args === null) ? [] :
                   (Array.isArray(args) ? args : [args]);
        return JSON.parse(JSON.stringify(list, function (k, v) {
            if (typeof v === 'function') {
                var id = nextId();
                fnTable[id] = v;
                return { __fn: id };
            }
            return v;
        }));
    }

    function call(api, args) {
        return new Promise(function (resolve, reject) {
            var cbid = nextId();
            pending[cbid] = { resolve: resolve, reject: reject };
            try {
                window.webkit.messageHandlers.h5ggBridge.postMessage({
                    api: api, args: serialize(args), cbid: cbid
                });
            } catch (e) {
                delete pending[cbid];
                reject(e);
            }
        });
    }

    // 原生回写结果
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

    // ---- 同步语义 -> Promise 的 API ----
    shim.require = function (v) { return call('require', [v]); };
    shim.setFloatTolerance = function (v) { return call('setFloatTolerance', [v]); };
    shim.searchNumber = function (value, type, from, to) { return call('searchNumber', [value, type, from, to]); };
    shim.searchNearby = function (value, type, range) { return call('searchNearby', [value, type, range]); };
    shim.getValue = function (addr, type) { return call('getValue', [addr, type]); };
    shim.setValue = function (addr, value, type) { return call('setValue', [addr, value, type]); };
    shim.editAll = function (value, type) { return call('editAll', [value, type]); };
    shim.getResults = function (maxCount, skip) { return call('getResults', [maxCount, skip]); };
    shim.getResultsCount = function () { return call('getResultsCount', []); };
    shim.clearResults = function () { return call('clearResults', []); };
    shim.getLocalScripts = function () { return call('getLocalScripts', []); };
    //可选参数 undefined 不能过桥 (会被 JSON 化成 null 使引擎侧 JSValue 判断失效), 空参直接省略
    shim.getRangesList = function (filter) { return call('getRangesList', filter === undefined ? [] : [filter]); };
    shim.getProcList = function (filter) { return call('getProcList', filter === undefined ? [] : [filter]); };
    shim.setTargetProc = function (pid) { return call('setTargetProc', [pid]); };
    shim.makeTweak = function (icon, html) { return call('makeTweak', [icon, html]); };

    // loadPlugin: 跨桥无法返回可交互的插件对象, HTML 界面下不再支持
    shim.loadPlugin = function () {
        return Promise.reject(new Error('loadPlugin is not supported in WKWebView UI mode; use .js scripts instead'));
    };

    // ---- 函数参数类 API (事件回调通道) ----
    shim.setButtonAction = function (fn) {
        evtTable['buttonAction'] = fn;
        return call('setButtonAction', []);
    };
    shim.setLayoutAction = function (fn) {
        evtTable['layoutAction'] = fn;
        return call('setLayoutAction', []);
    };
    shim.pickScriptFile = function (fn, types) {
        var fnid = nextId();
        fnTable[fnid] = fn;
        return call('pickScriptFile', [{ __fn: fnid }, types || undefined]);
    };

    // 旧引擎把这两个注册为页面裸全局, 第三方菜单普遍直接调用, 必须注册到 window
    if (window.setButtonAction === undefined) window.setButtonAction = shim.setButtonAction;
    if (window.setLayoutAction === undefined) window.setLayoutAction = shim.setLayoutAction;

    // ---- 脚本加载: .js 走原生 JSContext (保持旧脚本同步 API 完全兼容) ----
    shim.loadScript = function (path) { return call('loadScript', [path]); };

    // ---- 窗口/悬浮按钮控制类 API: 转发到原生 actions ----
    // 同时注册为 window 全局, 保证官方示例模板 (直接调用 setWindowDrag 等裸全局) 零改动兼容
    ['setWindowRect', 'setWindowDrag', 'setWindowTouch', 'setWindowVisible',
     'setButtonImage', 'setButtonOffset', 'closeMenu'].forEach(function (name) {
        shim[name] = function () { return call(name, Array.prototype.slice.call(arguments)); };
        if (window[name] === undefined) window[name] = shim[name];
    });

    window.h5gg = shim;

    // 供原生主动调用的 alert 转发 (原生 jscontext 里的 alert 走这里)
    window.h5gg_alert = function (msg) { window.alert(msg); };
})();
