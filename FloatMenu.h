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

-(void)handleMessage:(WKScriptMessage*)message;
-(void)setAction:(NSString*)name callback:(id)block;
-(void)evalJS:(NSString*)code;                 //页面: 异步
-(void)evalEngineJS:(NSString*)code;           //原生脚本引擎: 异步(内部串行)
-(NSString*)getValueByName:(NSString*)name;
-(void)firePageEvent:(NSString*)name args:(NSArray*)args;
-(void)alert:(NSString*)message;
-(void)loadScriptFile:(NSString*)path cbid:(long)cbid;

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
        NSString* shim = [NSString stringWithUTF8String:(char*)gH5GG_SHIMData];
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

//通用 API: 转发到原生 JSContext (同步语义), 结果 JSON 回写页面
-(void)invokeEngineApi:(NSString*)api args:(NSArray*)args cbid:(long)cbid
{
    @try {
        JSValue* fn = self.jscontext[api];
        if(fn.isUndefined) {
            [self respond:cbid ok:NO data:[NSString stringWithFormat:@"unknown api: %@", api]];
            return;
        }
        JSValue* ret = [fn callWithArguments:args];

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
        [self respond:cbid ok:YES data:json];
    } @catch(NSException* e) {
        [self respond:cbid ok:NO data:e.reason];
    }
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
        fnid = [(NSDictionary*)args.firstObject[@"__fn"] longValue];

    NSArray* types = nil;
    if(args.count > 1 && [args[1] isKindOfClass:[NSArray class]]) types = args[1];

    H5GG_WEAKSELF
    [TopShow filePicker:(types ?: @[@"public.executable", @"public.html"]) callback:^(NSString* path) {
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* js = [NSString stringWithFormat:@"window.__h5gg_fnresp(%ld, %@)", fnid,
                            path ? [weakSelf jsString:path] : @"null"];
            [weakSelf.webView evaluateJavaScript:js completionHandler:nil];
            [weakSelf respond:cbid ok:YES data:nil];
        });
    }];
}

-(void)loadScriptFile:(NSString*)path cbid:(long)cbid
{
    H5GG_WEAKSELF
    dispatch_async(dispatch_get_main_queue(), ^{
        if(!path) { [weakSelf respond:cbid ok:NO data:@"script path is empty"]; return; }

        NSURL* url = [path hasPrefix:@"file://"] ? [NSURL URLWithString:path] : [NSURL fileURLWithPath:path];
        if(!url || ![[NSFileManager defaultManager] fileExistsAtPath:url.path]) {
            [weakSelf respond:cbid ok:NO data:[NSString stringWithFormat:@"script not found: %@", path]];
            return;
        }

        dispatch_async(H5EngineQueue(), ^{
            NSError* error = nil;
            NSString* code = [NSString stringWithContentsOfURL:url encoding:NSUTF8StringEncoding error:&error];
            if(!code) {
                [weakSelf respond:cbid ok:NO data:[error localizedDescription]];
                return;
            }
            [weakSelf.jscontext evaluateScript:code withSourceURL:url];
            [weakSelf respond:cbid ok:YES data:@"\"loaded\""];
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
    if([navigationAction.request.URL.scheme isEqualToString:@"file"])
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
    }
    decisionHandler(WKNavigationActionPolicyAllow);
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    NSString* scheme = [[error.userInfo[NSURLErrorFailingURLErrorKey] scheme] lowercaseString];
    if([scheme isEqualToString:@"file"] || [scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"])
        [TopShow alert:Localized(@"H5加载失败") message:[NSString stringWithFormat:@"%@",error]];
}

- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)navigation {
    NSLog(@"webView didStartProvisionalNavigation=%@", webView);
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
