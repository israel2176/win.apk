// The screens sweep on the real AOSP framework (Robolectric, native graphics):
// runtime/java/com/macapk/loader/LayoutSweep.java, compiled into this test, over
// an APK's own resources (test_config.properties, written by screens.sh) with
// the app's classes, converted to a jar, on the classpath. Writes the lines to
// screens.out.
import com.macapk.loader.LayoutSweep;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

import java.io.File;
import java.io.PrintWriter;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w360dp-h640dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class ScreensOracleTest {
    @Test
    public void sweep() throws Exception {
        String pictures = System.getProperty("screens.pictures", "");
        try (PrintWriter out = new PrintWriter(System.getProperty("screens.out"), "UTF-8")) {
            try {
                for (String line : LayoutSweep.runOn(RuntimeEnvironment.getApplication(),
                                                     System.getProperty("screens.theme", ""),
                                                     System.getProperty("screens.filter", ""),
                                                     pictures.isEmpty() ? null : new File(pictures))) {
                    out.println(line);
                }
            } catch (Throwable t) {
                out.println("sweep failed " + t);
                t.printStackTrace();
            }
            out.println("done");
        }
    }
}
