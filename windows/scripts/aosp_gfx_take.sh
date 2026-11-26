# aosp_gfx_take.sh -- sourced, not run: which of android-all 14's classes are AOSP's graphics in
# Win.apk (windows/README.md, "Milestone 2"). build_aosp_gfx.sh puts them (and what they need) into
# lib/aosp-graphics.jar; build_java.sh compiles the framework against them, since the framework has
# no android.graphics of its own any more, so that both agree on one list.
#
# The text classes go with Paint (measuring and drawing text must agree), FontConfig with
# SystemFonts, and the platform's one drawable class of its own (AnimationScaleListDrawable, the
# progress bars' indeterminate drawable, whose XML names it) with the DrawableContainer it extends;
# GLSurfaceView (android.opengl) is AOSP's too: its GLThread draws through EGL10 into a window surface
# on the view's holder, as on a device (windows/test/glthread). SurfaceTexture is left out (Win.apk
# keeps its own: camera, video and TextureView frames). .uau
# files are Robolectric's annotations, not classes.
#
#   aosp_gfx_take <android-all's entry list>    prints the class files to take, one per line

aosp_gfx_take() {
    grep -E '^(android/graphics/.*|android/opengl/GLSurfaceView([$].*)?|android/text/(Layout|StaticLayout|BoringLayout|DynamicLayout|MeasuredParagraph|PrecomputedText|TextLine|Hyphenator|FontConfig)([$].*)?|android/util/PathParser([$].*)?|com/android/internal/graphics/drawable/AnimationScaleListDrawable([$].*)?)\.class$' \
        "$1" | grep -v '^android/graphics/SurfaceTexture'
}
