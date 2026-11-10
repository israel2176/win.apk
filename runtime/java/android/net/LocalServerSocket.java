// android.net.LocalServerSocket
//
// A listening Unix-domain socket, on a JDK ServerSocketChannel with
// StandardProtocolFamily.UNIX. A FILESYSTEM address binds its path. The String
// ctor is Android's ABSTRACT namespace, which only Linux has: the name becomes
// a socket file in the android root (LocalSocket.abstractPath), so every app
// of this install reaches it under the same name, as on a device. Mirrors
// frameworks/base/core/java/android/net/LocalServerSocket.java.
package android.net;

import java.io.IOException;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.channels.FileChannel;
import java.nio.channels.OverlappingFileLockException;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;

public class LocalServerSocket implements java.io.Closeable {

    private final ServerSocketChannel mChannel;
    private final LocalSocketAddress mLocalAddress;
    /** The socket file of an ABSTRACT name, ours to delete; null for a FILESYSTEM address. */
    private final Path mAbstractPath;
    /** Holds the ABSTRACT name while this socket is open (see the String ctor). */
    private final FileChannel mAbstractLock;

    /** Binds the name in the ABSTRACT namespace. The kernel drops an abstract
     *  name with its last socket, but a file stays behind a process that was
     *  killed. So the name's holder is whoever holds the lock on the file
     *  beside it, which Windows releases with the process: with the lock
     *  ours, a socket file still there is nobody's, and is replaced. */
    public LocalServerSocket(String name) throws IOException {
        mAbstractPath = LocalSocket.abstractPath(name);
        Files.createDirectories(mAbstractPath.getParent());
        FileChannel lock = FileChannel.open(mAbstractPath.resolveSibling(mAbstractPath.getFileName() + ".lock"),
                StandardOpenOption.CREATE, StandardOpenOption.WRITE);
        ServerSocketChannel channel = null;
        try {
            try {
                if (lock.tryLock() == null) throw new IOException("Address already in use: " + name);
            } catch (OverlappingFileLockException inThisProcess) {
                throw new IOException("Address already in use: " + name);
            }
            Files.deleteIfExists(mAbstractPath);
            channel = ServerSocketChannel.open(StandardProtocolFamily.UNIX);
            channel.bind(UnixDomainSocketAddress.of(mAbstractPath));
        } catch (IOException | RuntimeException e) {
            if (channel != null) channel.close();
            lock.close();
            throw e;
        }
        mChannel = channel;
        mAbstractLock = lock;
        mLocalAddress = new LocalSocketAddress(name);
    }

    public LocalServerSocket(java.io.FileDescriptor fd) throws IOException {
        throw new IOException("fd-based local server socket not supported");
    }

    /** Mac.apk extension entry point: bind a FILESYSTEM-namespace path with a
     *  real JDK Unix-domain server channel. */
    public LocalServerSocket(LocalSocketAddress address) throws IOException {
        if (address.getNamespace() != LocalSocketAddress.Namespace.FILESYSTEM) {
            throw new IOException("unsupported local socket namespace: "
                    + address.getNamespace());
        }
        mChannel = ServerSocketChannel.open(StandardProtocolFamily.UNIX);
        mChannel.bind(UnixDomainSocketAddress.of(Path.of(address.getName())));
        mLocalAddress = address;
        mAbstractPath = null;
        mAbstractLock = null;
    }

    public LocalSocket accept() throws IOException {
        SocketChannel ch = mChannel.accept();
        return new LocalSocket(ch);
    }

    public LocalSocketAddress getLocalSocketAddress() { return mLocalAddress; }

    public java.io.FileDescriptor getFileDescriptor() { return null; }

    public void close() throws IOException {
        if (mChannel == null || !mChannel.isOpen()) return;
        mChannel.close();
        if (mAbstractPath != null) {
            // Still under the lock, so the file deleted is ours and not the next holder's.
            try {
                Files.deleteIfExists(mAbstractPath);
            } catch (IOException e) {
                // Left behind: the next bind of the name replaces it.
            }
            mAbstractLock.close();
        }
    }
}
