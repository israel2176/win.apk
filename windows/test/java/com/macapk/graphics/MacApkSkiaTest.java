// MacApkSkiaTest -- RenderEffect and RuntimeShader with no Skia bridge, on a plain JVM.
//
// Upstream's RuntimeShader and RenderEffect run through a Skia library only the
// Mac build has (libskiko-macos-arm64.dylib), which Windows never has, as a
// plain JVM does not. MacApkSkia.isAvailable() threw on its first call there
// ("Skia Metal backend unavailable; no CPU shader fallback"), into the app code
// that made the effect (Bank Leumi's react-native-skia view, in onSizeChanged),
// where Android returns one. It must answer false, every time, and the effects
// must be made.
//
//   javac -cp <android-framework.jar> -d out windows/test/java/com/macapk/graphics/MacApkSkiaTest.java
//   java -cp <android-framework.jar>:out com.macapk.graphics.MacApkSkiaTest
package com.macapk.graphics;

import android.graphics.RenderEffect;
import android.graphics.RuntimeShader;
import android.graphics.Shader;

public final class MacApkSkiaTest {
    private static int failures;

    private static void check(String what, boolean ok, Object detail) {
        System.out.println((ok ? "  [PASS] " : "  [FAIL] ") + what + (detail != null ? ": " + detail : ""));
        if (!ok) failures++;
    }

    public static void main(String[] args) {
        System.out.println("MacApkSkia without the Skia bridge");
        try {
            RenderEffect blur = RenderEffect.createBlurEffect(8f, 8f, Shader.TileMode.CLAMP);
            check("RenderEffect.createBlurEffect, the first Skia call, returns an effect", blur != null, null);
        } catch (Throwable t) {
            check("RenderEffect.createBlurEffect, the first Skia call, returns an effect", false, t);
        }
        for (int i = 1; i <= 2; ++i) {
            try {
                check("isAvailable() answers false (call " + i + ")", !MacApkSkia.isAvailable(), null);
            } catch (Throwable t) {
                check("isAvailable() answers false (call " + i + ")", false, t);
            }
        }
        try {
            RuntimeShader shader = new RuntimeShader("half4 main(float2 p) { return half4(1); }");
            check("new RuntimeShader(agsl) is made", shader != null, null);
        } catch (Throwable t) {
            check("new RuntimeShader(agsl) is made", false, t);
        }
        try {
            check("RenderEffect.createOffsetEffect is made", RenderEffect.createOffsetEffect(1f, 2f) != null, null);
        } catch (Throwable t) {
            check("RenderEffect.createOffsetEffect is made", false, t);
        }
        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
