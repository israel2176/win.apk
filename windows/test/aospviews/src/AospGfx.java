// The redirect agent's (windows/test/aospgfx/redirect_agent.c) two entry points are named for the
// graphics test's class; this test reaches them through a class of that name.
final class AospGfx {
    private AospGfx() {}
    static native int[] agentCounts();
    static native boolean agentRestore();
}
