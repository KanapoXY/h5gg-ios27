//
//  makeWindow.h
//  h5gg
//
//  Created by admin on 24/4/2022.
//

#ifndef makeWindow_h
#define makeWindow_h

#pragma GCC diagnostic ignored "-Wnullability-completeness"

//iOS 27 scene 化适配: keyWindow 在 UIScene 环境下不可靠, 统一从这里取
static inline UIWindow* _h5ggKeyWindow(void)
{
    if (@available(iOS 13.0, *)) {
        UIWindow* fallback = nil;
        for (UIScene* scene in [UIApplication sharedApplication].connectedScenes) {
            if (![scene isKindOfClass:[UIWindowScene class]]) continue;
            UIWindowScene* ws = (UIWindowScene*)scene;
            if (scene.activationState == UISceneActivationStateForegroundActive) {
                for (UIWindow* w in ws.windows)
                    if (w.isKeyWindow) return w;
                if (!fallback && ws.windows.count) fallback = ws.windows.firstObject;
            } else if (scene.activationState == UISceneActivationStateForegroundInactive) {
                if (!fallback && ws.windows.count) fallback = ws.windows.firstObject;
            }
        }
        if (fallback) return fallback;
    }
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return [UIApplication sharedApplication].keyWindow;
    #pragma clang diagnostic pop
}

static inline NSArray<UIWindow*>* _h5ggAllWindows(void)
{
    if (@available(iOS 13.0, *)) {
        NSMutableArray* all = [[NSMutableArray alloc] init];
        for (UIScene* scene in [UIApplication sharedApplication].connectedScenes)
            if ([scene isKindOfClass:[UIWindowScene class]])
                [all addObjectsFromArray:((UIWindowScene*)scene).windows];
        if (all.count) return all;
    }
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return [UIApplication sharedApplication].windows;
    #pragma clang diagnostic pop
}

UIWindow* makeWindow(NSString* clazz)
{
    UIWindow* w = nil;
    
    if (@available(iOS 13.0, *)) {
        UIWindowScene* theScene=nil;
        for (UIWindowScene* windowScene in [UIApplication sharedApplication].connectedScenes) {
            NSLog(@"windowScene=%@ %@ state=%ld", windowScene, windowScene.windows, (long)windowScene.activationState);
            if(!theScene && windowScene.activationState==UISceneActivationStateForegroundInactive)
                theScene = windowScene;
            if (windowScene.activationState == UISceneActivationStateForegroundActive) {
                theScene = windowScene;
                break;
            }
        }
        w = [[NSClassFromString(clazz) alloc] initWithWindowScene:theScene];
    }else{
        CGRect frame = [UIScreen mainScreen].bounds; //在iPad分屏或浮动模式下, 后面会被resize成真实尺寸
        w = [[NSClassFromString(clazz) alloc] initWithFrame:frame];
        NSLog(@"makeWindow=frame=%@", NSStringFromCGRect(w.frame));
    }
    
    return w;
}



@implementation UIWindow(GVWindow)

//这个在ipad支持悬浮模式的app中会触发两次viewWillTransitionToSize
- (void)private_updateToInterfaceOrientation:(UIInterfaceOrientation)orientation animated:(BOOL)animated
{
    NSLog(@"private_updateToInterfaceOrientation=%ld %d %@", (long)orientation, animated, self);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundeclared-selector"
    SEL mySelector = @selector(_updateToInterfaceOrientation:animated:);
#pragma clang diagnostic pop
    NSMethodSignature * sig = [[self class] instanceMethodSignatureForSelector:mySelector];
    NSInvocation * myInvocation = [NSInvocation invocationWithMethodSignature: sig];
    [myInvocation setTarget:self];
    [myInvocation setSelector: mySelector];
    [myInvocation setArgument:&orientation atIndex: 2];
    [myInvocation setArgument:&animated atIndex: 3];
    [myInvocation retainArguments];
    [myInvocation invoke];
}

//- (void)private_updateToInterfaceOrientation:(UIInterfaceOrientation)orientation duration:(float)duration force:(BOOL)force
//{
//    NSLog(@"private_updateToInterfaceOrientation=%d %d %@", orientation, force, self);
//#pragma clang diagnostic push
//#pragma clang diagnostic ignored "-Wundeclared-selector"
//    SEL mySelector = @selector(_updateToInterfaceOrientation:duration:force:);
//#pragma clang diagnostic pop
//    NSMethodSignature * sig = [[self class] instanceMethodSignatureForSelector:mySelector];
//    NSInvocation * myInvocation = [NSInvocation invocationWithMethodSignature: sig];
//    [myInvocation setTarget:self];
//    [myInvocation setSelector: mySelector];
//    [myInvocation setArgument:&orientation atIndex: 2];
//    [myInvocation setArgument:&duration atIndex: 3];
//    [myInvocation setArgument:&force atIndex: 4];
//    [myInvocation retainArguments];
//    [myInvocation invoke];
//}

@end

#endif /* makeWindow_h */
