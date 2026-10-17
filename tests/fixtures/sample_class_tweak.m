/* Class-based tweak (SampleTweak style) — Path C E2E fixture */
#import <Foundation/Foundation.h>

@interface SampleTweak : NSObject
+ (NSString *)greeting;
- (id)init;
@end

@implementation SampleTweak
+ (NSString *)greeting {
    return @"Hello from SampleTweak";
}
- (id)init {
    self = [super init];
    NSLog(@"SampleTweak init");
    return self;
}
@end

__attribute__((visibility("default")))
const char *sample_tweak_version(void) {
    return "1.0.0";
}
