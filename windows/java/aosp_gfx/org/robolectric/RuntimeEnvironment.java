package org.robolectric;
// The native runtime's JNI_OnLoad asks this for the API level it binds for.
public class RuntimeEnvironment { public static int getApiLevel() { return 34; } }
