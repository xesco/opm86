// macOS only: what JUCE leaves unset on the Metal layers its windows draw to.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

namespace opm86
{

void matchWindowColours(); // called from MainComponent.cpp

// A Metal layer shows its colours unmatched unless told what they are, so on a
// wide-gamut display they come out more saturated than drawn. JUCE draws in
// sRGB: told so, each window's layer shows the colours a plain view does.
// Windows come and go, menus and tooltips too, so this is called each frame;
// a layer already told is left alone.
void matchWindowColours()
{
    for (NSWindow* window in [NSApp windows])
    {
        CALayer* layer = window.contentView.layer;
        if (![layer isKindOfClass:[CAMetalLayer class]] || ((CAMetalLayer*) layer).colorspace != nullptr)
            continue;
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        ((CAMetalLayer*) layer).colorspace = srgb;
        CGColorSpaceRelease(srgb);
    }
}

} // namespace opm86
