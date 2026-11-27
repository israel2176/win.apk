// HandlerCallbackTest -- android.os.Handler's callback is AOSP's mCallback, as apps reach it by reflection.
//
//   java -cp android-framework.jar;<classes> HandlerCallbackTest
//
// AliExpress (and SDKs like it) replace ActivityThread.mH's Handler.mCallback in attachBaseContext with
// one that sees each message first and then hands it to the one it replaced; on API 35 and up its SDKs
// start from that callback (#210). Checks, on a handler shaped like ActivityThread.H: the field is
// there under AOSP's name and holds the constructor's callback; a callback set through it is asked
// first; one that answers false lets handleMessage run, one that answers true stops it; and the
// replaced callback still runs when the new one delegates to it.
// Prints PASS/FAIL per check and "ALL PASSED" at the end; exits 1 on a failure.
import android.os.Handler;
import android.os.Looper;
import android.os.Message;

import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.List;

public class HandlerCallbackTest {
    static int failures;

    static void check(String name, boolean ok) {
        System.out.println((ok ? "PASS " : "FAIL ") + name);
        if (!ok) failures++;
    }

    /** ActivityThread.H's shape: a Handler subclass with its own handleMessage. */
    static final class H extends Handler {
        final List<Integer> handled = new ArrayList<>();
        H(Callback cb) { super((Looper) null, cb); }
        @Override public void handleMessage(Message msg) { handled.add(msg.what); }
    }

    public static void main(String[] args) throws Exception {
        Field f;
        try {
            f = Handler.class.getDeclaredField("mCallback");
        } catch (NoSuchFieldException e) {
            check("Handler.mCallback exists", false);
            System.out.println("FAILED");
            System.exit(1);
            return;
        }
        check("Handler.mCallback exists", true);
        check("Handler.mCallback is a Handler.Callback", f.getType() == Handler.Callback.class);
        f.setAccessible(true);

        // No callback: the field is null and handleMessage gets the message.
        H h = new H(null);
        check("no callback: mCallback null", f.get(h) == null);

        // The app's hook, as AliExpress installs it on ActivityThread.mH.
        List<Integer> seen = new ArrayList<>();
        Handler.Callback hook = msg -> { seen.add(msg.what); return false; };
        f.set(h, hook);
        check("set: mCallback reads back the hook", f.get(h) == hook);
        h.dispatchMessage(Message.obtain(h, 159));
        check("hook sees EXECUTE_TRANSACTION first", seen.size() == 1 && seen.get(0) == 159);
        check("hook answered false: handleMessage ran", h.handled.size() == 1 && h.handled.get(0) == 159);

        // A hook that consumes the message stops handleMessage, as Handler.dispatchMessage does.
        f.set(h, (Handler.Callback) msg -> true);
        h.dispatchMessage(Message.obtain(h, 110));
        check("hook answered true: handleMessage not run", h.handled.size() == 1);

        // A handler built with a callback: the field holds it, and a hook delegating to it keeps it working.
        List<Integer> original = new ArrayList<>();
        Handler.Callback orig = msg -> { original.add(msg.what); return false; };
        H h2 = new H(orig);
        check("constructor's callback is mCallback", f.get(h2) == orig);
        Handler.Callback prev = (Handler.Callback) f.get(h2);
        List<Integer> seen2 = new ArrayList<>();
        f.set(h2, (Handler.Callback) msg -> { seen2.add(msg.what); return prev.handleMessage(msg); });
        h2.dispatchMessage(Message.obtain(h2, 159));
        check("delegating hook: hook, replaced callback and handleMessage each ran once",
                seen2.size() == 1 && original.size() == 1 && h2.handled.size() == 1);

        // A posted Runnable never reaches the callback, as on Android.
        int before = seen2.size();
        boolean[] ran = {false};
        Message r = Message.obtain(h2, () -> ran[0] = true);
        h2.dispatchMessage(r);
        check("a Runnable message runs and skips the callback", ran[0] && seen2.size() == before);

        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
