// android.net.LocalSocketAddress
//
// Names a Unix-domain (local) socket endpoint. Android supports three
// namespaces: FILESYSTEM is a JDK UnixDomainSocketAddress, ABSTRACT a socket
// file LocalSocket.abstractPath picks for the name, RESERVED is recorded but
// cannot be bound or connected. Pure value type. Mirrors
// frameworks/base/core/java/android/net/LocalSocketAddress.java.
package android.net;

public class LocalSocketAddress {

    public enum Namespace {
        ABSTRACT,
        RESERVED,
        FILESYSTEM
    }

    private final String mName;
    private final Namespace mNamespace;

    public LocalSocketAddress(String name, Namespace namespace) {
        mName = name;
        mNamespace = namespace;
    }

    public LocalSocketAddress(String name) {
        this(name, Namespace.ABSTRACT);
    }

    public String getName() { return mName; }
    public Namespace getNamespace() { return mNamespace; }
}
