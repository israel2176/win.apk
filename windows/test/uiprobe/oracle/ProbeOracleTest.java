// The same probes on the real AOSP framework (Robolectric, native graphics),
// with the probe APK's own compiled resources. Prints "PROBE <line>" as the
// activity does.
import dev.winapk.probe.Probes;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 35, qualifiers = "w360dp-h640dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class ProbeOracleTest {
    @Test
    public void probes() {
        Object app = RuntimeEnvironment.getApplication();
        String pictures = System.getProperty("probe.pictures");
        for (String line : Probes.runAllOn(app, pictures == null ? null : new java.io.File(pictures))) {
            System.out.println("PROBE " + line);
        }
        System.out.println("PROBE done");
    }
}
