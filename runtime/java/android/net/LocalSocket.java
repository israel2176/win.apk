// android.net.LocalSocket
//
// A Unix-domain stream socket, on the JDK's SocketChannel with
// StandardProtocolFamily.UNIX. A FILESYSTEM address is its path. Android's
// default ABSTRACT namespace exists only on Linux, so an abstract name is a
// socket file in the android root (abstractPath, below), which
// LocalServerSocket(String) binds. RESERVED (/dev/socket) is not supported.
// Mirrors frameworks/base/core/java/android/net/LocalSocket.java.
package android.net;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.SocketAddress;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.channels.Channels;
import java.nio.channels.SocketChannel;
import java.nio.file.Path;

public class LocalSocket implements java.io.Closeable {

    public static final int SOCKET_DGRAM = 1;
    public static final int SOCKET_STREAM = 2;
    public static final int SOCKET_SEQPACKET = 3;

    private SocketChannel mChannel;
    private boolean mBound;
    private boolean mConnected;
    private boolean mInputShutdown;
    private boolean mOutputShutdown;
    private LocalSocketAddress mRemoteAddress;

    public LocalSocket() {}
    public LocalSocket(int sockType) {}

    /** Package-private ctor used by LocalServerSocket.accept to wrap an
     *  already-connected channel. */
    LocalSocket(SocketChannel channel) {
        mChannel = channel;
        mConnected = channel != null && channel.isConnected();
    }

    /**
     * The socket file that stands for a name of the ABSTRACT namespace:
     * {@code <android root>/dev/socket/a-<hash>}. The android root is shared by
     * the apps of an install and private to the Windows user, which is the
     * reach an abstract name has on a device. The name is hashed because it may
     * hold any character, differs by case where Windows file names do not, and
     * an AF_UNIX path is at most 108 bytes with the directory in it.
     */
    static Path abstractPath(String name) throws IOException {
        if (name == null) throw new NullPointerException("name");
        byte[] digest;
        try {
            digest = java.security.MessageDigest.getInstance("SHA-256")
                    .digest(name.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
        StringBuilder file = new StringBuilder("a-");
        for (int i = 0; i < 8; i++) file.append(String.format("%02x", digest[i]));
        return Path.of(com.macapk.loader.SharedStorage.androidRoot(), "dev", "socket", file.toString());
    }

    public void connect(LocalSocketAddress endpoint) throws IOException {
        Path path;
        switch (endpoint.getNamespace()) {
            case FILESYSTEM: path = Path.of(endpoint.getName()); break;
            case ABSTRACT: path = abstractPath(endpoint.getName()); break;
            default:
                throw new IOException("unsupported local socket namespace: " + endpoint.getNamespace());
        }
        UnixDomainSocketAddress addr = UnixDomainSocketAddress.of(path);
        mChannel = SocketChannel.open(StandardProtocolFamily.UNIX);
        mChannel.connect(addr);
        mConnected = true;
        mRemoteAddress = endpoint;
    }

    public void connect(LocalSocketAddress endpoint, int timeout) throws IOException {
        connect(endpoint);
    }

    public void bind(LocalSocketAddress bindpoint) throws IOException {
        // AOSP binds before connect for client-named sockets; JDK client
        // channels are unnamed. Record intent; FILESYSTEM bind is unsupported
        // for a client channel on the JDK, so this is a no-op flag.
        mBound = true;
    }

    public InputStream getInputStream() throws IOException {
        if (mChannel == null) throw new IOException("socket not connected");
        return Channels.newInputStream(mChannel);
    }

    public OutputStream getOutputStream() throws IOException {
        if (mChannel == null) throw new IOException("socket not connected");
        return Channels.newOutputStream(mChannel);
    }

    public boolean isConnected() { return mConnected; }
    public boolean isBound() { return mBound; }
    public boolean isClosed() { return mChannel != null && !mChannel.isOpen(); }

    /** Derived from the JDK channel's own bound address when there is one.
     *  Null before bind/connect, which is the state AOSP reports too. */
    public LocalSocketAddress getLocalSocketAddress() {
        return toLocalSocketAddress(channelAddressQuietly(true));
    }

    /**
     * AOSP's name for the peer address. We remember the endpoint connect() was
     * given, and otherwise ask the channel -- an accept()ed Unix socket is
     * usually unnamed, in which case null is the truthful answer rather than a
     * fabricated path.
     */
    public LocalSocketAddress getRemoteSocketAddress() {
        if (mRemoteAddress != null) {
            return mRemoteAddress;
        }
        return toLocalSocketAddress(channelAddressQuietly(false));
    }

    /** @deprecated Mac.apk-only alias kept for callers that predate the
     *  AOSP-correct name. Use {@link #getRemoteSocketAddress()}. */
    @Deprecated
    public LocalSocketAddress getPeerSocketAddress() { return getRemoteSocketAddress(); }

    private SocketAddress channelAddressQuietly(boolean local) {
        if (mChannel == null) {
            return null;
        }
        try {
            return local ? mChannel.getLocalAddress() : mChannel.getRemoteAddress();
        } catch (IOException e) {
            return null;
        }
    }

    private static LocalSocketAddress toLocalSocketAddress(SocketAddress addr) {
        if (!(addr instanceof UnixDomainSocketAddress)) {
            return null;
        }
        Path path = ((UnixDomainSocketAddress) addr).getPath();
        if (path == null || path.toString().isEmpty()) {
            // An unnamed (autobind) Unix socket: there is no address to report.
            return null;
        }
        return new LocalSocketAddress(path.toString(),
                LocalSocketAddress.Namespace.FILESYSTEM);
    }

    /**
     * AOSP hands back the socket's own file descriptor. Ours is owned by a JDK
     * SocketChannel, whose fd lives in sun.nio.ch.SocketChannelImpl -- a
     * package the runtime does NOT --add-opens (see
     * android.os.ParcelFileDescriptor for the same limit), so there is no way
     * to reach it. Null rather than a fabricated FileDescriptor: a caller that
     * got a bogus fd would go on to read, write or dup it. AOSP also returns
     * null here before the socket is created, so callers already handle it.
     */
    public java.io.FileDescriptor getFileDescriptor() { return null; }

    public void setSoTimeout(int n) throws IOException {}
    public int getSoTimeout() throws IOException { return 0; }
    public void setReceiveBufferSize(int size) throws IOException {}
    public int getReceiveBufferSize() throws IOException { return 0; }
    public void setSendBufferSize(int size) throws IOException {}
    public int getSendBufferSize() throws IOException { return 0; }

    /** Ancillary fd passing is a Linux-specific feature with no JDK API; honest
     *  empty result. */
    public java.io.FileDescriptor[] getAncillaryFileDescriptors() throws IOException {
        return null;
    }
    public void setFileDescriptorsForSend(java.io.FileDescriptor[] fds) {}

    public Credentials getPeerCredentials() throws IOException { return null; }

    public void shutdownInput() throws IOException {
        if (mChannel != null) mChannel.shutdownInput();
        mInputShutdown = true;
    }
    public void shutdownOutput() throws IOException {
        if (mChannel != null) mChannel.shutdownOutput();
        mOutputShutdown = true;
    }

    /** Real local state, set by shutdownInput/shutdownOutput above. */
    public boolean isInputShutdown() { return mInputShutdown; }
    public boolean isOutputShutdown() { return mOutputShutdown; }

    public void close() throws IOException {
        if (mChannel != null) mChannel.close();
        mConnected = false;
    }

    SocketAddress channelLocalAddress() throws IOException {
        return mChannel == null ? null : mChannel.getLocalAddress();
    }
}
