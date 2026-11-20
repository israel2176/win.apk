// The PermissionEnforcer of the in-process services whose AIDL Stub takes one (IInputMethodManager's:
// on Android its no-argument constructor asks ActivityThread for the system context). Its checks run
// only on a call from another process (onTransact); these services are called in-process only.
package com.winapk.aospviews;

public final class LocalEnforcer extends android.os.PermissionEnforcer {
    public LocalEnforcer() { super(); }
}
