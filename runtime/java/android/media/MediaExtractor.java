// android.media.MediaExtractor -- demuxes a media container into compressed
// access units. AOSP frameworks/base media/java/android/media/MediaExtractor.java.
//
// Backed by the host's demuxer (com.macapk.loader.HostMedia; on Windows Media
// Foundation's source reader, windows/host/video_decode.h's Demuxer): every
// audio and video track of what the host can read (MP4/3GP/M4A/MOV, MP3,
// ADTS, and what its installed codec packs add), with the samples in the form
// MediaCodec takes -- H.264 and HEVC as Annex-B with the parameter sets in
// csd-0/csd-1, AAC raw with the AudioSpecificConfig in csd-0. A path and an
// http(s) URL are opened as they are; a FileDescriptor range, a content://
// Uri and a MediaDataSource are copied to a temporary file first, because the
// host's reader opens names, not descriptors.
package android.media;
import java.io.FileDescriptor;
import java.nio.ByteBuffer;
import java.util.Map;

public final class MediaExtractor {
    private long    mHandle  = 0;
    private boolean[] mSelected = new boolean[0];
    private MediaFormat[] mFormats = new MediaFormat[0];
    // The current sample (AOSP's model: readSampleData reads it, advance()
    // moves on). Read lazily, so that selectTrack calls made after
    // setDataSource decide what the first one is.
    private byte[]  mCurrent = null;
    private final long[] mMeta = new long[3];   // {track, presentationTimeUs, sync}
    private boolean mPrimed = false;
    // Temp file backing a spooled data source; deleted by release().
    private java.io.File mTmpFile = null;

    // What a spooled source may weigh: beyond this the copy is refused rather
    // than filling the disk.
    private static final long MAX_SRC_BYTES = 4L << 30;

    public MediaExtractor() {}

    private void open(String pathOrUrl, java.io.File tmp) throws java.io.IOException {
        long h = 0;
        try {
            h = com.macapk.loader.HostMedia.nativeDemuxOpen(tmp != null ? tmp.getAbsolutePath() : pathOrUrl);
        } catch (LinkageError e) {
            // A runtime without the demuxer: the same answer as an unreadable file.
        }
        int n = h != 0 ? com.macapk.loader.HostMedia.nativeDemuxTrackCount(h) : 0;
        if (h == 0 || n <= 0) {
            if (h != 0) com.macapk.loader.HostMedia.nativeDemuxClose(h);
            if (tmp != null) tmp.delete();
            throw new java.io.IOException("MediaExtractor: cannot open " + (tmp != null ? "the data source" : pathOrUrl));
        }
        mHandle = h;
        mTmpFile = tmp;
        mSelected = new boolean[n];
        mFormats = new MediaFormat[n];
    }

    public void setDataSource(String path) throws java.io.IOException {
        if (path == null) throw new IllegalArgumentException("path == null");
        release();
        // Strip a file:// scheme; the native opener wants a filesystem path.
        String p = path;
        if (p.startsWith("file://")) p = p.substring(7);
        open(p, null);
    }
    public void setDataSource(String path, Map<String, String> headers) throws java.io.IOException { setDataSource(path); }
    public void setDataSource(FileDescriptor fd) throws java.io.IOException {
        // AOSP passes a sentinel length that the native extractor clamps down
        // to the real file size; setDataSource below does the same clamp.
        setDataSource(fd, 0, 0x7ffffffffffffffL);
    }

    /**
     * AOSP opens the [offset, offset+length) region of an already-open fd. The
     * host's reader opens a name, so the region is copied to a temporary file
     * first. The reads are positional, so the file offset the caller owns is
     * never disturbed.
     */
    public void setDataSource(FileDescriptor fd, long offset, long length) throws java.io.IOException {
        if (fd == null || !fd.valid())
            throw new java.io.IOException("MediaExtractor: invalid FileDescriptor");
        release();
        // Deliberately NOT closed: the descriptor belongs to the caller (an
        // AssetFileDescriptor it may keep using), and a FileInputStream
        // adopted from an existing FileDescriptor registers no GC-time
        // cleanup of its own, so leaving it unclosed cannot close their fd.
        java.io.FileInputStream in = new java.io.FileInputStream(fd);
        final java.nio.channels.FileChannel ch = in.getChannel();
        long size = ch.size();
        if (offset < 0) offset = 0;
        if (length <= 0 || (size > 0 && offset + length > size))
            length = (size > offset) ? size - offset : 0;
        if (length <= 0 || length > MAX_SRC_BYTES)
            throw new java.io.IOException("MediaExtractor: unusable fd range off="
                    + offset + " len=" + length);
        final long start = offset;
        open(null, spool(new java.io.InputStream() {
            private long pos = start;
            @Override public int read() throws java.io.IOException {
                byte[] one = new byte[1];
                return read(one, 0, 1) == 1 ? (one[0] & 0xff) : -1;
            }
            @Override public int read(byte[] b, int off, int len) throws java.io.IOException {
                int n = ch.read(ByteBuffer.wrap(b, off, len), pos);
                if (n > 0) pos += n;
                return n;
            }
        }, length));
    }

    /** API 23. The source's bytes, read through readAt. */
    public void setDataSource(final MediaDataSource source) throws java.io.IOException {
        if (source == null) throw new IllegalArgumentException("source == null");
        release();
        long size = source.getSize();
        open(null, spool(new java.io.InputStream() {
            private long pos = 0;
            @Override public int read() throws java.io.IOException {
                byte[] one = new byte[1];
                return read(one, 0, 1) == 1 ? (one[0] & 0xff) : -1;
            }
            @Override public int read(byte[] b, int off, int len) throws java.io.IOException {
                int n = source.readAt(pos, b, off, len);
                if (n > 0) pos += n;
                return n;
            }
        }, size > 0 ? Math.min(size, MAX_SRC_BYTES) : MAX_SRC_BYTES));
    }

    public void setDataSource(android.content.Context context, android.net.Uri uri,
                              Map<String, String> headers) throws java.io.IOException {
        if (uri == null) throw new IllegalArgumentException("uri == null");
        String scheme = uri.getScheme();
        if (scheme == null || "file".equals(scheme)) {
            setDataSource(uri.getPath());
            return;
        }
        if ("http".equals(scheme) || "https".equals(scheme)) {
            setDataSource(uri.toString());
            return;
        }
        // content://, android.resource://: whatever the resolver opens.
        if (context == null) throw new java.io.IOException("MediaExtractor: no Context for " + uri);
        release();
        java.io.InputStream in = context.getContentResolver().openInputStream(uri);
        if (in == null) throw new java.io.IOException("MediaExtractor: cannot open " + uri);
        try {
            open(null, spool(in, MAX_SRC_BYTES));
        } finally {
            try { in.close(); } catch (java.io.IOException ignore) { }
        }
    }
    // AOSP: a declared length below 0 means "the whole file", so it drops the
    // slice bounds entirely rather than passing a negative length down.
    public void setDataSource(android.content.res.AssetFileDescriptor afd) throws java.io.IOException {
        if (afd == null) throw new IllegalArgumentException("afd == null");
        if (afd.getDeclaredLength() < 0) {
            setDataSource(afd.getFileDescriptor());
        } else {
            setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), afd.getDeclaredLength());
        }
    }

    private static java.io.File spool(java.io.InputStream in, long limit) throws java.io.IOException {
        return com.macapk.loader.HostMedia.spool(in, limit);
    }

    /** Reads the next sample of the selected tracks into mCurrent (null at the end). */
    private void fetch() {
        mCurrent = (mHandle != 0) ? com.macapk.loader.HostMedia.nativeDemuxRead(mHandle, mMeta) : null;
        mPrimed = true;
    }
    private void ensureCurrent() {
        if (!mPrimed) fetch();
    }

    public int getTrackCount() { return mSelected.length; }

    public MediaFormat getTrackFormat(int index) {
        if (mHandle == 0 || index < 0 || index >= mFormats.length)
            throw new IllegalArgumentException("no track " + index);
        if (mFormats[index] == null) mFormats[index] = readFormat(index);
        return new MediaFormat(mFormats[index]);
    }

    private MediaFormat readFormat(int index) {
        java.util.HashMap<String, String> info = new java.util.HashMap<>();
        String text = com.macapk.loader.HostMedia.nativeDemuxTrackInfo(mHandle, index);
        if (text != null) {
            for (String line : text.split("\n")) {
                int eq = line.indexOf('=');
                if (eq > 0) info.put(line.substring(0, eq), line.substring(eq + 1));
            }
        }
        MediaFormat f = new MediaFormat();
        f.setString(MediaFormat.KEY_MIME, info.getOrDefault("mime", "application/octet-stream"));
        if ("1".equals(info.get("video"))) {
            int w = intOf(info, "width"), h = intOf(info, "height");
            f.setInteger(MediaFormat.KEY_WIDTH, w);
            f.setInteger(MediaFormat.KEY_HEIGHT, h);
            int rotation = intOf(info, "rotation");
            if (rotation != 0) f.setInteger(MediaFormat.KEY_ROTATION, rotation);
            float fps = 0;
            try { fps = Float.parseFloat(info.getOrDefault("frame-rate", "0")); } catch (NumberFormatException ignore) { }
            if (fps > 0) f.setInteger(MediaFormat.KEY_FRAME_RATE, Math.max(1, Math.round(fps)));
            // What a decoder's input buffer must hold: AOSP's extractors state
            // it; the estimate is media3's (see MediaCodec.resolveInputCapacity).
            f.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, Math.max(1 << 16, w * h * 3 / 4));
        } else {
            f.setInteger(MediaFormat.KEY_SAMPLE_RATE, intOf(info, "sample-rate"));
            f.setInteger(MediaFormat.KEY_CHANNEL_COUNT, intOf(info, "channel-count"));
        }
        int bitrate = intOf(info, "bitrate");
        if (bitrate > 0) f.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
        String language = info.get("language");
        f.setString(MediaFormat.KEY_LANGUAGE, language == null || language.isEmpty() ? "und" : language);
        long dur = com.macapk.loader.HostMedia.nativeDemuxDurationUs(mHandle);
        if (dur > 0) f.setLong(MediaFormat.KEY_DURATION, dur);
        for (int i = 0; i < 2; i++) {
            byte[] csd = com.macapk.loader.HostMedia.nativeDemuxTrackCsd(mHandle, index, i);
            if (csd != null && csd.length > 0) f.setByteBuffer("csd-" + i, ByteBuffer.wrap(csd));
        }
        return f;
    }
    private static int intOf(Map<String, String> info, String key) {
        try { return Integer.parseInt(info.getOrDefault(key, "0")); } catch (NumberFormatException e) { return 0; }
    }

    public void selectTrack(int index) {
        if (mHandle == 0 || index < 0 || index >= mSelected.length) return;
        mSelected[index] = true;
        com.macapk.loader.HostMedia.nativeDemuxSelect(mHandle, index, true);
        // Nothing was current because nothing was selected: look again.
        if (mPrimed && mCurrent == null) mPrimed = false;
    }
    public void unselectTrack(int index) {
        if (mHandle == 0 || index < 0 || index >= mSelected.length) return;
        mSelected[index] = false;
        com.macapk.loader.HostMedia.nativeDemuxSelect(mHandle, index, false);
        if (mCurrent != null && mMeta[0] == index) { mCurrent = null; mPrimed = false; }
    }

    public boolean advance() {
        if (mHandle == 0) return false;
        ensureCurrent();
        if (mCurrent == null) return false;
        fetch();
        return mCurrent != null;
    }

    public int readSampleData(ByteBuffer byteBuf, int offset) {
        ensureCurrent();
        if (mCurrent == null) return -1;
        int n = mCurrent.length;
        if (offset < 0 || byteBuf.capacity() - offset < n)
            throw new IllegalArgumentException("readSampleData: the buffer holds " + (byteBuf.capacity() - offset)
                    + " bytes, the sample is " + n);
        // AOSP leaves the buffer's position at offset and its limit after the sample.
        byteBuf.limit(byteBuf.capacity());
        byteBuf.position(offset);
        byteBuf.put(mCurrent, 0, n);
        byteBuf.limit(offset + n);
        byteBuf.position(offset);
        return n;
    }

    public int  getSampleTrackIndex() { ensureCurrent(); return mCurrent != null ? (int) mMeta[0] : -1; }
    public long getSampleTime()       { ensureCurrent(); return mCurrent != null ? mMeta[1] : -1; }
    public int  getSampleFlags() {
        ensureCurrent();
        return mCurrent == null ? -1 : mMeta[2] != 0 ? SAMPLE_FLAG_SYNC : 0;
    }
    // AOSP declares `public native long getSampleSize()`; an int return is a
    // different descriptor and would not link from SDK-compiled bytecode.
    // frameworks/base/media/java/android/media/MediaExtractor.java
    public long getSampleSize()       { ensureCurrent(); return mCurrent != null ? mCurrent.length : -1L; }

    public void seekTo(long timeUs, int mode) {
        if (mHandle == 0) return;
        com.macapk.loader.HostMedia.nativeDemuxSeek(mHandle, timeUs, mode);
        mCurrent = null;
        mPrimed = false;
    }

    // AOSP: -1 when "that information is unavailable or not applicable (no
    // cache)", and false unless data is being cached: the host reads files and
    // streams without a cache this class could measure.
    public long getCachedDuration() { return -1; }
    public boolean hasCacheReachedEndOfStream() { return false; }
    public boolean getSampleCryptoInfo(MediaCodec.CryptoInfo info) { return false; }

    public void release() {
        if (mHandle != 0) { com.macapk.loader.HostMedia.nativeDemuxClose(mHandle); mHandle = 0; }
        // The spooled source stays alive only as long as the demuxer that reads it.
        if (mTmpFile != null) { mTmpFile.delete(); mTmpFile = null; }
        mCurrent = null; mPrimed = false;
        mSelected = new boolean[0];
        mFormats = new MediaFormat[0];
    }

    // =====================================================================
    // API-gap close-out. Signatures from /tmp/aosp_api/android16.txt; behaviour
    // from frameworks/base/media/java/android/media/MediaExtractor.java
    // (android16-release).
    // =====================================================================

    /** The metrics-bundle keys AOSP publishes for getMetrics(). */
    public static final class MetricsConstants {
        private MetricsConstants() { }
        public static final String FORMAT = "android.media.mediaextractor.fmt";
        public static final String MIME_TYPE = "android.media.mediaextractor.mime";
        public static final String TRACKS = "android.media.mediaextractor.ntrk";
    }

    /**
     * AOSP: the DRM initialisation data (the PSSH boxes) the container declares,
     * "or null if none". This extractor never parses a PSSH box -- Mac.apk
     * implements no DRM at all (see MediaDrm) -- so null is the truthful answer,
     * and it is the one an unprotected file gives on Android too. An app that
     * gets null takes its clear-content path, which is the only path we can
     * actually serve.
     */
    public DrmInitData getDrmInitData() { return null; }

    /** @Nullable in AOSP; the same answer, from the same absence, as
     *  getDrmInitData above. */
    public java.util.Map<java.util.UUID, byte[]> getPsshInfo() { return null; }

    /**
     * API 28. The audio presentations a next-gen-audio (AC-4 / MPEG-H) track
     * declares. @NonNull in AOSP, empty when the track carries none -- which is
     * every track here: the extractor handles ordinary AAC/MP3/container audio
     * and parses no presentation descriptors. Empty is AOSP's own answer for a
     * plain track, not a placeholder.
     */
    public java.util.List<AudioPresentation> getAudioPresentations(int trackIndex) {
        return java.util.Collections.emptyList();
    }

    /**
     * API 26. The conditional-access descrambling info for a track. @Nullable in
     * AOSP: null when the track is not scrambled. Nothing here is scrambled --
     * there is no CAS plugin (see MediaCas) and the extractor cannot open one --
     * so null for every track.
     */
    public CasInfo getCasInfo(int index) { return null; }

    /**
     * @deprecated in AOSP. Attaches a MediaCas so scrambled tracks can be
     * descrambled. A MediaCas cannot be CONSTRUCTED on this host (its
     * constructor throws UnsupportedCasException, see that class), so this is
     * unreachable with a real argument; a null argument is rejected the way
     * AOSP's @NonNull contract requires rather than silently accepted.
     */
    @Deprecated
    public void setMediaCas(MediaCas mediaCas) {
        java.util.Objects.requireNonNull(mediaCas, "mediaCas must not be null");
    }

    /** Describes how one track is scrambled. Never produced here (see
     *  getCasInfo); the type exists so that signature resolves. */
    public static final class CasInfo {
        private final int mSystemId;
        private final MediaCas.Session mSession;
        private final byte[] mPrivateData;

        CasInfo(int systemId, MediaCas.Session session, byte[] privateData) {
            mSystemId = systemId;
            mSession = session;
            mPrivateData = privateData;
        }

        public int getSystemId() { return mSystemId; }
        public MediaCas.Session getSession() { return mSession; }
        /** @Nullable in AOSP. */
        public byte[] getPrivateData() { return mPrivateData; }
    }

    // ---- metrics / log session -------------------------------------------

    private android.media.metrics.LogSessionId mLogSessionId =
            android.media.metrics.LogSessionId.LOG_SESSION_ID_NONE;

    /** @NonNull in AOSP; LOG_SESSION_ID_NONE until the app sets one. */
    public android.media.metrics.LogSessionId getLogSessionId() { return mLogSessionId; }

    public void setLogSessionId(android.media.metrics.LogSessionId logSessionId) {
        mLogSessionId = java.util.Objects.requireNonNull(logSessionId,
                "logSessionId must not be null");
    }

    /**
     * AOSP's getMetrics() reports what the native extractor did. The three keys
     * AOSP publishes as public constants are filled in from this extractor's
     * REAL state; the private native counters are omitted rather than
     * zero-filled, because a zero counter reads as a measurement. AOSP returns
     * null when there are no metrics at all, so a caller already handles a thin
     * bundle.
     */
    public android.os.PersistableBundle getMetrics() {
        android.os.PersistableBundle bundle = new android.os.PersistableBundle();
        bundle.putInt(MetricsConstants.TRACKS, getTrackCount());
        if (getTrackCount() > 0) {
            try {
                MediaFormat f = getTrackFormat(0);
                if (f != null && f.containsKey(MediaFormat.KEY_MIME)) {
                    bundle.putString(MetricsConstants.MIME_TYPE,
                            f.getString(MediaFormat.KEY_MIME));
                }
            } catch (Throwable ignore) {
                // A track whose format cannot be read simply contributes no key,
                // which is better than contributing a wrong one.
            }
        }
        return bundle;
    }

    /** AOSP declares finalize() so a dropped extractor releases its native
     *  demuxer and temp file. release() is idempotent. */
    @Override protected void finalize() throws Throwable {
        try { release(); } finally { super.finalize(); }
    }

    public static final int SEEK_TO_PREVIOUS_SYNC = 0;
    public static final int SEEK_TO_NEXT_SYNC = 1;
    public static final int SEEK_TO_CLOSEST_SYNC = 2;
    public static final int SAMPLE_FLAG_SYNC = 1;
    public static final int SAMPLE_FLAG_ENCRYPTED = 2;
}
