package dev.winapk.glthread;

/** {@link ForkActivity} with EGL14: a window surface on the holder's Surface. */
public class Fork14Activity extends ForkActivity {
    @Override boolean egl14() { return true; }
}
