//
//  FloatMenu.h
//  menutweak
//
//  Created by admin on 4/3/2022.
//  iOS 27 adaptation: UIWebView -> WKWebView + messageHandler bridge
//

#undef WEBVIEW_HOOK

#ifndef FloatMenu_h
#define FloatMenu_h

#include "TopShow.h"
#import <JavaScriptCore/JavaScriptCore.h>
#import <WebKit/WebKit.h>
#include <objc/runtime.h>
#include "ModalShow.h"
#include "version.h"

//嵌入 shim 与 initial.js (INCTXT 由 incbin.h 提供, 该头文件在 Tweak.mm 中先于本文件引入)
INCTXT(INITIAL_JS, "initial.js");
INCTXT(H5GG_SHIM, "h5gg-shim.js");

static NSHashTable* g_webViews = nil;

//引擎串行队列: JSContext 只要求不并发访问, 用串行队列简化线程模型
static dispatch_queue_t _h5ggEngineQueue = nil;
static inline dispatch_queue_t H5EngineQueue(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        _h5ggEngineQueue = dispatch_queue_create("h5gg.engine", DISPATCH_QUEUE_SERIAL);
    });
    return _h5ggEngineQueue;
}

#define H5GG_WEAKSELF __weak typeof(self) weakSelf = self;

@class FloatMenu;

//弱引用代理, 避免 WKUserContentController -> self -> config -> userContentController 的循环引用
@interface H5GGMessageProxy : NSObject <WKScriptMessageHandler>
@property (weak) FloatMenu* menu;
@end

@interface FloatMenu : UIView <WKNavigationDelegate, WKUIDelegate>
@property (nonatomic, strong) WKWebView* webView;
@property NSTimer* frontTimer;

@property BOOL touchableAll;
@property CGRect touchableRect;

@property CGRect dragableRect;
@property CGPoint startLocation;

//原生脚本引擎: 同步 JSContext, 用户 .js 脚本与 h5ggEngine JSExport 均在此运行, 旧脚本 API 完全兼容
//仅在 engineThread 上访问
@property JSContext* jscontext;
@property NSMutableDictionary* actions;

@property BOOL usingCustomDialog;
@property void(^reloadAction)(void);

//file:// 二次导航放行标志 (防止 loadFileURL 触发的导航被 delegate 再次拦截造成死循环)
@property BOOL loadingFileURL;

-(void)handleMessage:(WKScriptMessage*)message;
-(void)setAction:(NSString*)name callback:(id)block;
-(void)loadRequest:(NSURLRequest*)request;
-(void)loadHTMLString:(NSString*)string baseURL:(NSURL*)baseURL;
-(void)setDragRect:(CGRect)rect;
-(void)evalJS:(NSString*)code;                 //页面: 异步
-(void)evalEngineJS:(NSString*)code;           //原生脚本引擎: 异步(内部串行)
-(NSString*)getValueByName:(NSString*)name;
-(void)firePageEvent:(NSString*)name args:(NSArray*)args;
-(void)alert:(NSString*)message;
-(void)loadScriptFile:(NSString*)path cbid:(long)cbid;
-(NSDictionary*)engineExecute:(NSString*)api args:(NSArray*)args;
-(NSString*)engineSyncBridge:(NSString*)payload;

@end

@implementation H5GGMessageProxy
- (void)userContentController:(WKUserContentController *)userContentController
      didReceiveScriptMessage:(WKScriptMessage *)message
{
    [self.menu handleMessage:message];
}
@end

@implementation FloatMenu

-(instancetype)initWithFrame:(CGRect)frame {
    static dispatch_once_t predicate;
    dispatch_once(&predicate, ^{
        g_webViews = [NSHashTable weakObjectsHashTable];
    });

    self = [super initWithFrame:frame];
    if (self) {
        [g_webViews addObject:self];

        float version = [UIDevice currentDevice].systemVersion.floatValue;
        self.usingCustomDialog = version>13.0 && version<13.4;

        self.touchableAll = YES;

        WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
        WKUserContentController* ucc = [[WKUserContentController alloc] init];

        //注入桥接 shim (document start)
        //前置版本号: WKWebView 页面无 JSContext, 拿不到引擎 ctx 的 h5gg_internel_version, 在 document start 直接写入页面全局
        NSString* shim = [NSString stringWithFormat:@"window.h5gg_internel_version=\"%s\";\n", H5GG_VERSION_S];
        shim = [shim stringByAppendingString:[NSString stringWithUTF8String:(char*)gH5GG_SHIMData]];
        WKUserScript* userScript = [[WKUserScript alloc] initWithSource:shim
                                                         injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                      forMainFrameOnly:YES];
        [ucc addUserScript:userScript];

        H5GGMessageProxy* proxy = [[H5GGMessageProxy alloc] init];
        proxy.menu = self;
        [ucc addScriptMessageHandler:proxy name:@"h5ggBridge"];

        config.userContentController = ucc;

        self.webView = [[WKWebView alloc] initWithFrame:self.bounds configuration:config];
        self.webView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        self.webView.navigationDelegate = self;
        self.webView.UIDelegate = self;
        self.webView.opaque = NO;
        self.webView.backgroundColor = [UIColor clearColor];
        self.webView.scrollView.bounces = NO;
        self.webView.scrollView.scrollEnabled = NO;
        [self.webView.scrollView setShowsVerticalScrollIndicator:NO];
        [self.webView.scrollView setShowsHorizontalScrollIndicator:NO];
        if (@available(iOS 11.0, *))
            [self.webView.scrollView setContentInsetAdjustmentBehavior:UIScrollViewContentInsetAdjustmentNever];

        [self addSubview:self.webView];

        UIPanGestureRecognizer* drag = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(dragMe:)];
        [self addGestureRecognizer:drag];

        //初始化原生脚本引擎(串行队列上惰性创建 JSContext)
        dispatch_async(H5EngineQueue(), ^{
            [self setupEngineContext];
        });

        self.actions = [[NSMutableDictionary alloc] init];
    }
    return self;
}

#pragma mark - engine queue & JSContext

-(void)setupEngineContext
{
    if(!self.jscontext) {
        self.jscontext = [[JSContext alloc] init];
        self.jscontext.name = @"h5gg-engine";
    }
    JSContext* ctx = self.jscontext;

    ctx[@"window"] = ctx.globalObject;

    ctx[@"h5gg_internel_version"] = @H5GG_VERSION;

    for (id key in self.actions) {
        ctx[key] = self.actions[key];
    }

    //console 转发到页面
    ctx[@"console"] = @{@"log": ^(JSValue* msg){ [self pageConsole:[msg toString]]; },
                        @"error": ^(JSValue* msg){ [self pageConsole:[NSString stringWithFormat:@"ERROR: %@", [msg toString]]]; }};

    //alert: 异步转发到页面 window.alert, 不阻塞搜索线程
    ctx[@"alert"] = ^(JSValue* message) {
        NSString* s = [message isUndefined] ? @"" : [message toString];
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.webView evaluateJavaScript:[NSString stringWithFormat:@"window.h5gg_alert(%@);",
                [self jsString:s]] completionHandler:nil];
        });
    };

    //confirm/prompt: 同步 ModalShow (engineThread 为普通线程, 可安全阻塞等待)
    ctx[@"confirm"] = ^(JSValue* message) {
        return [self confirm:[message isUndefined] ? @"" : [message toString]];
    };
    ctx[@"prompt"] = ^(JSValue* text, JSValue* defaultText) {
        return [self prompt:[text isUndefined] ? @"" : [text toString]
                 defaultText:[defaultText isUndefined] ? @"" : [defaultText toString]];
    };

    //JS 异常 -> 页面提示
    [ctx setExceptionHandler:^(JSContext* exceptionCtx, JSValue* value) {
        NSLog(@"h5gg engine JS error: %@", value);
        NSString* msg = [NSString stringWithFormat:@"[engine] %@", [value valueForProperty:@"message"] ?: value];
        dispatch_async(dispatch_get_main_queue(), ^{
            [self alert:msg];
        });
    }];

    //initial.js: window.onerror 处理器
    [ctx evaluateScript:[NSString stringWithUTF8String:(char*)gINITIAL_JSData]];

    if(self.reloadAction) self.reloadAction();
}

-(NSString*)jsString:(NSString*)s
{
    NSData* data = [NSJSONSerialization dataWithJSONObject:@[s?:@""] options:0 error:nil];
    NSString* json = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    return [json substringWithRange:NSMakeRange(1, json.length-2)]; //去掉数组括号
}

-(void)pageConsole:(NSString*)msg
{
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.webView evaluateJavaScript:[NSString stringWithFormat:@"console.log(%@);", [self jsString:msg]]
                       completionHandler:nil];
    });
}

#pragma mark - message bridge

-(void)handleMessage:(WKScriptMessage*)message
{
    if(![message.name isEqualToString:@"h5ggBridge"]) return;
    if(![message.body isKindOfClass:[NSDictionary class]]) return;

    NSDictionary* payload = (NSDictionary*)message.body;
    NSString* api = payload[@"api"];
    NSArray* args = payload[@"args"] ?: @[];
    long cbid = [payload[@"cbid"] longValue];

    NSLog(@"h5gg bridge api=%@ cbid=%ld", api, cbid);

    dispatch_async(dispatch_get_main_queue(), ^{
        [self invokeApi:api args:args cbid:cbid];
    });
}

-(void)invokeApi:(NSString*)api args:(NSArray*)args cbid:(long)cbid
{
    if([api isEqualToString:@"pickScriptFile"]) {
        [self pickScriptFileWithArgs:args cbid:cbid];
        return;
    }

    if([api isEqualToString:@"loadScript"]) {
        [self loadScriptFile:args.firstObject cbid:cbid];
        return;
    }

    dispatch_async(H5EngineQueue(), ^{
        [self invokeEngineApi:api args:args cbid:cbid];
    });
}

//引擎 API 统一执行体 (调用方必须已在 H5EngineQueue 上)
//返回 @{ok:BOOL, data:NSString}, data 为返回值的 JSON 编码 (undefined -> NSNull)
-(NSDictionary*)engineExecute:(NSString*)api args:(NSArray*)args
{
    @try {
        JSValue* fn = self.jscontext[api];
        if(fn.isUndefined) {
            //内存引擎 API 是 h5gg JSExport 对象的方法 (h5gg.searchNumber 等), 全局未命中时回退到对象方法
            JSValue* engine = self.jscontext[@"h5gg"];
            if(!engine.isUndefined) fn = engine[api];
        }
        if(fn.isUndefined) {
            return @{@"ok": @NO, @"data": [NSString stringWithFormat:@"unknown api: %@", api]};
        }
        JSValue* ret = [fn callWithArguments:args ?: @[]];

        NSString* json = nil;
        if(!(ret.isUndefined || ret.isNull)) {
            id obj = ret.toObject;
            if([NSJSONSerialization isValidJSONObject:@[obj ?: [NSNull null]]]) {
                NSData* d = [NSJSONSerialization dataWithJSONObject:@[obj] options:0 error:nil];
                json = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
                json = [json substringWithRange:NSMakeRange(1, json.length-2)];
            } else {
                json = [ret toString];
            }
        }
        return @{@"ok": @YES, @"data": json ?: (id)[NSNull null]};
    } @catch(NSException* e) {
        return @{@"ok": @NO, @"data": e.reason ?: @"engine exception"};
    }
}

//通用 API: 转发到原生 JSContext (同步语义), 结果 JSON 回写页面
-(void)invokeEngineApi:(NSString*)api args:(NSArray*)args cbid:(long)cbid
{
    NSDictionary* r = [self engineExecute:api args:args];
    [self respond:cbid ok:[r[@"ok"] boolValue] data:r[@"data"]];
}

//v6 同步桥: 页面 prompt("__h5gg_sync__...") -> 引擎 API 同步执行并返回 JSON
//delegate 在主线程被调, 页面 JS 阻塞等待; 引擎调用在 H5EngineQueue 上完成
//3 秒超时保护: 引擎队列被长脚本占用时返回错误而不是永久卡死 UI
-(NSString*)engineSyncBridge:(NSString*)payload
{
    NSData* d = [payload dataUsingEncoding:NSUTF8StringEncoding];
    NSDictionary* req = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
    NSString* api = req[@"api"];
    if(![api isKindOfClass:[NSString class]])
        return @"{\"ok\":false,\"error\":\"bad sync request\"}";
    NSArray* args = req[@"args"];
    if(![args isKindOfClass:[NSArray class]]) args = @[];

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block NSDictionary* result = nil;
    dispatch_async(H5EngineQueue(), ^{
        if(!self.jscontext) [self setupEngineContext];
        result = [self engineExecute:api args:args];
        dispatch_semaphore_signal(sem);
    });
    long wait = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3 * NSEC_PER_SEC)));
    if(wait != 0 || !result)
        return @"{\"ok\":false,\"error\":\"h5gg sync bridge timeout (engine busy?)\"}";
    NSData* out = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
    return out ? [[NSString alloc] initWithData:out encoding:NSUTF8StringEncoding]
               : @"{\"ok\":false,\"error\":\"sync bridge encode failed\"}";
}

-(void)respond:(long)cbid ok:(BOOL)ok data:(NSString*)data
{
    NSString* escaped = data ? [self jsString:data] : @"null";
    NSString* js = [NSString stringWithFormat:@"window.__h5gg_resp(%ld, %@, %@)", cbid, ok?@"true":@"false", escaped];
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.webView evaluateJavaScript:js completionHandler:nil];
    });
}

-(void)pickScriptFileWithArgs:(NSArray*)args cbid:(long)cbid
{
    //fn 占位 {__fn: n} -> 文件选择结果回写页面函数
    long fnid = 0;
    if([args.firstObject isKindOfClass:[NSDictionary class]])
        fnid = [(NSNumber*)args.firstObject[@"__fn"] longValue];

    NSArray* types = nil;
    if(args.count > 1 && [args[1] isKindOfClass:[NSArray class]]) types = args[1];

    [TopShow filePicker:(types ?: @[@"public.executable", @"public.html"]) callback:^(NSString* path) {
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* js = [NSString stringWithFormat:@"window.__h5gg_fnresp(%ld, %@)", fnid,
                            path ? [self jsString:path] : @"null"];
            [self.webView evaluateJavaScript:js completionHandler:nil];
            [self respond:cbid ok:YES data:nil];
        });
    }];
}

-(void)loadScriptFile:(NSString*)path cbid:(long)cbid
{
    dispatch_async(dispatch_get_main_queue(), ^{
        if(!path) { [self respond:cbid ok:NO data:@"script path is empty"]; return; }

        //按 scheme 解析: file:// 与 http(s):// 走 URL 解析, 其余视为本地路径
        //v7.99 回归修复: 此前 http(s) 链接被当作本地路径做 fileExistsAtPath 检查, 导致网络菜单报 script not found
        NSURL* url = nil;
        if([path hasPrefix:@"file://"] || [path hasPrefix:@"http://"] || [path hasPrefix:@"https://"])
            url = [NSURL URLWithString:path];
        else
            url = [NSURL fileURLWithPath:path];
        if(!url || (url.isFileURL && ![[NSFileManager defaultManager] fileExistsAtPath:url.path])) {
            [self respond:cbid ok:NO data:[NSString stringWithFormat:@"script not found: %@", path]];
            return;
        }

        //HTML 脚本: 加载到菜单 WebView 作为界面 (与旧引擎行为一致)
        if([[url.pathExtension lowercaseString] isEqualToString:@"html"]) {
            if(url.isFileURL) {
                [self loadFileURLSafe:url];
            } else {
                //远程菜单: 直接导航, decidePolicyFor 对非 file 导航放行, didCommitNavigation 会启用滚动
                self.loadingFileURL = NO;
                [self.webView loadRequest:[NSURLRequest requestWithURL:url]];
            }
            [self respond:cbid ok:YES data:@"\"loaded\""];
            return;
        }

        //JS 脚本: 读取/下载后送引擎执行 (stringWithContentsOfURL 同时支持本地与远程)
        dispatch_async(H5EngineQueue(), ^{
            NSError* error = nil;
            NSString* code = [NSString stringWithContentsOfURL:url encoding:NSUTF8StringEncoding error:&error];
            if(!code) {
                [self respond:cbid ok:NO data:[error localizedDescription]];
                return;
            }
            [self.jscontext evaluateScript:code withSourceURL:url];
            [self respond:cbid ok:YES data:@"\"loaded\""];
        });
    });
}

-(void)firePageEvent:(NSString*)name args:(NSArray*)args
{
    NSMutableString* js = [NSMutableString stringWithFormat:@"window.__h5gg_evt(%@", [self jsString:name]];
    for(id arg in args ?: @[]) {
        NSData* d = [NSJSONSerialization dataWithJSONObject:@[arg ?: [NSNull null]] options:0 error:nil];
        NSString* json = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
        [js appendFormat:@", %@", [json substringWithRange:NSMakeRange(1, json.length-2)]];
    }
    [js appendString:@");"];
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.webView evaluateJavaScript:js completionHandler:nil];
    });
}

#pragma mark - page eval

-(void)evalJS:(NSString*)code
{
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.webView evaluateJavaScript:code completionHandler:nil];
    });
}

-(void)evalEngineJS:(NSString*)code
{
    dispatch_async(H5EngineQueue(), ^{
        [self setupEngineContext];
        [self.jscontext evaluateScript:code];
    });
}

#pragma mark - native actions

-(void)setAction:(NSString*)name callback:(id)block
{
    [self.actions setValue:block forKey:name];
    if(self.jscontext) self.jscontext[name] = block;
}

-(NSString*)getValueByName:(NSString*)name
{
    return nil; //原版遗留接口, 注入模式未使用
}

-(void)alert:(NSString*)message
{
    dispatch_async(dispatch_get_main_queue(), ^{ [self.superview sendSubviewToBack:self]; });
    [ModalShow alert:@"H5GG" message:message InWindow:self.window];
    dispatch_async(dispatch_get_main_queue(), ^{ [self.superview bringSubviewToFront:self]; });
}

-(BOOL)confirm:(NSString*)message
{
    BOOL result = NO;
    dispatch_async(dispatch_get_main_queue(), ^{ [self.superview sendSubviewToBack:self]; });
    result = [ModalShow confirm:message InWindow:self.window];
    dispatch_async(dispatch_get_main_queue(), ^{ [self.superview bringSubviewToFront:self]; });
    return result;
}

-(NSString*)prompt:(NSString*)text defaultText:(NSString*)defaultText
{
    NSString* result;
    dispatch_async(dispatch_get_main_queue(), ^{ [self.superview sendSubviewToBack:self]; });
    result = [ModalShow prompt:text defaultText:defaultText InWindow:self.window];
    dispatch_async(dispatch_get_main_queue(), ^{ [self.superview bringSubviewToFront:self]; });
    return result;
}

#pragma mark - touch & drag

-(nullable UIView *)hitTest:(CGPoint)point withEvent:(nullable UIEvent *)event
{
    return [super hitTest:point withEvent:event];
}

- (BOOL)pointInside:(CGPoint)point withEvent:(nullable UIEvent *)event
{
    if(self.touchableAll || CGRectContainsPoint(self.touchableRect, point))
        return [super pointInside:point withEvent:event];
    return NO;
}

-(void)setDragRect:(CGRect)rect {
    self.dragableRect = rect;
}

#pragma mark - file:// 加载支持
//WKWebView 禁止 loadRequest 与页面 location.href 加载本地文件, 必须用 loadFileURL 显式授权读取范围

-(NSURL*)_h5ggReadAccessRootFor:(NSURL*)fileURL
{
    NSURL* dir = [fileURL URLByDeletingLastPathComponent];
    //位于 .app 包内的页面, 授权整个包目录以便引用包内相对资源 (css/js/图片)
    NSString* bundlePath = [[NSBundle mainBundle] bundlePath];
    if([dir.path hasPrefix:bundlePath])
        return [NSURL fileURLWithPath:bundlePath];
    //包外文件 (用户选择的脚本等) 授权其所在目录
    return dir;
}

-(void)loadFileURLSafe:(NSURL*)url
{
    NSLog(@"h5gg loadFileURLSafe: %@", url);
    self.loadingFileURL = YES;
    if(@available(iOS 9.0, *)) {
        [self.webView loadFileURL:url allowingReadAccessToURL:[self _h5ggReadAccessRootFor:url]];
    } else {
        [self.webView loadRequest:[NSURLRequest requestWithURL:url]];
    }
}

-(void)loadRequest:(NSURLRequest*)request
{
    if(request.URL.isFileURL) {
        [self loadFileURLSafe:request.URL];
        return;
    }
    [self.webView loadRequest:request];
}

-(void)loadHTMLString:(NSString*)string baseURL:(NSURL*)baseURL
{
    [self.webView loadHTMLString:string baseURL:baseURL];
}

-(void)dragMe:(UIPanGestureRecognizer *)sender {
    CGPoint translation = [sender translationInView:sender.view];
    CGPoint locationPoint = [sender locationInView:sender.view];

    if(sender.state==UIGestureRecognizerStateBegan) {
        self.startLocation = locationPoint;
    }

    if(sender.state==UIGestureRecognizerStateChanged) {
        if(!CGRectContainsPoint(self.dragableRect, self.startLocation))
            return;

        CGPoint pt = locationPoint;
        float dx = pt.x - self.startLocation.x;
        float dy = pt.y - self.startLocation.y;

        CGPoint newcenter = CGPointMake(self.center.x + dx, self.center.y + dy);

        float halfy = CGRectGetMidY(self.bounds);
        newcenter.y = MAX(halfy, newcenter.y);

        self.center = newcenter;
        PGVSharedData->floatMenuRect = self.frame;
    }
}

#pragma mark - WKNavigationDelegate

- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)navigationAction decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    //loadFileURL 触发的二次导航直接放行, 防止拦截循环
    if(self.loadingFileURL) {
        self.loadingFileURL = NO;
        decisionHandler(WKNavigationActionPolicyAllow);
        return;
    }

    if(navigationAction.request.URL.isFileURL)
    {
        NSError* error=nil;
        NSString* html = [NSString stringWithContentsOfURL:navigationAction.request.URL encoding:NSASCIIStringEncoding error:&error];
        if(html)
        {
            NSInteger CR_count = ([html length] - [[html stringByReplacingOccurrencesOfString:@"\r" withString:@""] length]) / 1;
            NSInteger CRLF_count = ([html length] - [[html stringByReplacingOccurrencesOfString:@"\r\n" withString:@""] length]) / 2;

            if(CR_count>0 && CR_count!=CRLF_count) {
                [TopShow alert:Localized(@"提示") message:Localized(@"该页面为CR换行符格式, 请修改为LF或CRLF换行符格式, 否则JS错误提示无法显示准确的行数!")];
            }
        }

        //页面 location.href 或 loadRequest 发起的 file:// 导航被 WKWebView 拦截, 取消后改用 loadFileURL 重新加载
        //注意: decidePolicyFor 回调内同步发起新导航会被 WKWebView 静默忽略 (旧导航未终结),
        //必须延迟到下一个 runloop 再加载, 否则表现为无跳转无报错
        decisionHandler(WKNavigationActionPolicyCancel);
        dispatch_async(dispatch_get_main_queue(), ^{
            [self loadFileURLSafe:navigationAction.request.URL];
        });
        return;
    }
    decisionHandler(WKNavigationActionPolicyAllow);
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    NSString* scheme = [[error.userInfo[NSURLErrorFailingURLErrorKey] scheme] lowercaseString];
    if([scheme isEqualToString:@"file"] || [scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"])
        [TopShow alert:Localized(@"H5加载失败") message:[NSString stringWithFormat:@"%@",error]];
}

//临时阶段失败 (文件不可读/越权访问等): 原版未实现导致静默卡在"执行中"弹窗, 这里补上明确报错
- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    NSLog(@"h5gg didFailProvisionalNavigation: %@", error);
    NSString* scheme = [[error.userInfo[NSURLErrorFailingURLErrorKey] scheme] lowercaseString];
    if([scheme isEqualToString:@"file"] || [scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"])
        [TopShow alert:Localized(@"H5加载失败") message:[NSString stringWithFormat:@"%@", error.userInfo[NSLocalizedDescriptionKey] ?: error]];
}

- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)navigation {
    NSLog(@"webView didStartProvisionalNavigation=%@", webView);
}

//滚动自适应: 包内自带 Index 菜单保持禁用滚动 (原版行为, 兼容窗口拖动);
//外部加载的页面 (用户脚本/网络菜单) 启用整页滚动, 否则依赖 body 滚动的第三方菜单无法滑动
- (void)webView:(WKWebView *)webView didCommitNavigation:(WKNavigation *)navigation {
    NSURL* url = webView.URL;
    BOOL bundled = url.isFileURL && [url.path hasPrefix:[[NSBundle mainBundle] bundlePath]];
    webView.scrollView.scrollEnabled = bundled ? NO : YES;
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    NSLog(@"webView didFinishNavigation=%@", webView);

    //FastClick 检查 (原版逻辑保留)
    [self.webView evaluateJavaScript:@"typeof FastClick" completionHandler:^(id result, NSError* error) {
        if(!error && result && ![result isEqual:@"undefined"]) {
            [self alert:Localized(@"发现FastClick模块!\n\n请将其从html中移除, 否则界面可能卡死!")];
        }
    }];

    if(self.reloadAction) self.reloadAction();
}

#pragma mark - WKUIDelegate (页面 alert/confirm/prompt 走系统弹窗)

- (void)webView:(WKWebView*)webView runJavaScriptAlertPanelWithMessage:(NSString*)message
    initiatedByFrame:(WKFrameInfo*)frame completionHandler:(void(^)(void))completionHandler
{
    UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"H5GG" message:message
                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:Localized(@"确定") style:UIAlertActionStyleDefault
        handler:^(UIAlertAction* action){ completionHandler(); }]];

    UIWindow* host = self.window ?: _h5ggKeyWindow();
    dispatch_async(dispatch_get_main_queue(), ^{
        [host.rootViewController presentViewController:alert animated:YES completion:nil];
    });
}

- (void)webView:(WKWebView*)webView runJavaScriptConfirmPanelWithMessage:(NSString*)message
    initiatedByFrame:(WKFrameInfo*)frame completionHandler:(void(^)(BOOL result))completionHandler
{
    UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"H5GG" message:message
                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:Localized(@"确定") style:UIAlertActionStyleDefault
        handler:^(UIAlertAction* action){ completionHandler(YES); }]];
    [alert addAction:[UIAlertAction actionWithTitle:Localized(@"取消") style:UIAlertActionStyleCancel
        handler:^(UIAlertAction* action){ completionHandler(NO); }]];

    UIWindow* host = self.window ?: _h5ggKeyWindow();
    dispatch_async(dispatch_get_main_queue(), ^{
        [host.rootViewController presentViewController:alert animated:YES completion:nil];
    });
}

- (void)webView:(WKWebView*)webView runJavaScriptTextInputPanelWithPrompt:(NSString*)prompt
    defaultText:(NSString*)defaultText initiatedByFrame:(WKFrameInfo*)frame
    completionHandler:(void(^)(NSString* result))completionHandler
{
    //v6 同步桥: 前缀 __h5gg_sync__ 的 prompt 拦截为引擎同步调用, 不弹 UI
    NSString* syncPrefix = @"__h5gg_sync__";
    if([prompt hasPrefix:syncPrefix]) {
        NSString* payload = [prompt substringFromIndex:syncPrefix.length];
        completionHandler([self engineSyncBridge:payload]);
        return;
    }

    UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"H5GG" message:prompt
                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField* textField){ textField.text = defaultText; }];
    [alert addAction:[UIAlertAction actionWithTitle:Localized(@"确定") style:UIAlertActionStyleDefault
        handler:^(UIAlertAction* action){ completionHandler(alert.textFields.firstObject.text); }]];
    [alert addAction:[UIAlertAction actionWithTitle:Localized(@"取消") style:UIAlertActionStyleCancel
        handler:^(UIAlertAction* action){ completionHandler(nil); }]];

    UIWindow* host = self.window ?: _h5ggKeyWindow();
    dispatch_async(dispatch_get_main_queue(), ^{
        [host.rootViewController presentViewController:alert animated:YES completion:nil];
    });
}

@end

#endif /* FloatMenu_h */
