/* C-function / constructor style (iOS Gods Popup Gone pattern) — Path C E2E */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

static IMP orig_present = NULL;

static id hooked_present(id self, SEL _cmd, id vc, int animated, id completion) {
    NSLog(@"[PopupGone] present intercepted");
    if (orig_present) {
        return ((id(*)(id, SEL, id, int, id))orig_present)(self, _cmd, vc, animated, completion);
    }
    return nil;
}

__attribute__((constructor))
static void popup_gone_init(void) {
    Class cls = objc_getClass("UIViewController");
    if (!cls) {
        NSLog(@"[PopupGone] UIViewController missing at load");
        return;
    }
    MSHookMessageEx(cls, sel_registerName("presentViewController:animated:completion:"),
                    (IMP)hooked_present, &orig_present);
    NSLog(@"[PopupGone] loaded");
}
