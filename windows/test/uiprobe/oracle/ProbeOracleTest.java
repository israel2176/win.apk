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
        for (String line : Probes.runAllOn(app)) System.out.println("PROBE " + line);
        System.out.println("PROBE done");
    }
}
