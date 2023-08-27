// Copyright 2021 Cheng Zhao. All rights reserved.
// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE.chromium file.

#include "nativeui/appearance.h"

#import <Cocoa/Cocoa.h>

#include "base/apple/scoped_typeref.h"
#include "base/apple/scoped_nsobject.h"

namespace base::apple {

namespace internal {

template <typename B>
struct ScopedBlockTraits {
  static B InvalidValue() { return nullptr; }
  static B Retain(B block) { return Block_copy(block); }
  static void Release(B block) { Block_release(block); }
};

}  // namespace internal

// ScopedBlock<> is patterned after ScopedCFTypeRef<>, but uses Block_copy() and
// Block_release() instead of CFRetain() and CFRelease().
template <typename B>
using ScopedBlock = ScopedTypeRef<B, internal::ScopedBlockTraits<B>>;

}  // namespace base::apple

// Helper object to respond to light mode/dark mode changeovers.
@interface NUEffectiveAppearanceObserver : NSObject
@end

@implementation NUEffectiveAppearanceObserver {
  base::apple::ScopedBlock<void (^)()> handler_;
}

- (instancetype)initWithHandler:(void (^)())handler {
  self = [super init];
  if (self) {
    handler_.reset([handler copy]);
    if (@available(macOS 10.14, *)) {
      [NSApp addObserver:self
              forKeyPath:@"effectiveAppearance"
                 options:0
                 context:nullptr];
    }
  }
  return self;
}

- (void)dealloc {
  if (@available(macOS 10.14, *)) {
    [NSApp removeObserver:self forKeyPath:@"effectiveAppearance"];
  }
  [super dealloc];
}

- (void)observeValueForKeyPath:(NSString*)forKeyPath
                      ofObject:(id)object
                        change:(NSDictionary*)change
                       context:(void*)context {
  handler_.get()();
}

@end

namespace nu {

namespace internal {

class ColorSchemeObserverImpl : public ColorSchemeObserver {
 public:
  explicit ColorSchemeObserverImpl(Appearance* appearance) {
    appearance_observer_.reset(
        [[NUEffectiveAppearanceObserver alloc] initWithHandler:^{
          appearance->on_color_scheme_change.Emit();
        }]);
  }

 private:
  base::apple::scoped_nsobject<NUEffectiveAppearanceObserver> appearance_observer_;
};

// static
ColorSchemeObserver* ColorSchemeObserver::Create(Appearance* appearance) {
  return new ColorSchemeObserverImpl(appearance);
}

}  // namespace internal

bool Appearance::IsDarkScheme() const {
  if (@available(macOS 10.14, *)) {
    NSAppearanceName appearance =
        [[NSApp effectiveAppearance] bestMatchFromAppearancesWithNames:@[
          NSAppearanceNameAqua, NSAppearanceNameDarkAqua
        ]];
    return [appearance isEqual:NSAppearanceNameDarkAqua];
  }
  return false;
}

}  // namespace nu
