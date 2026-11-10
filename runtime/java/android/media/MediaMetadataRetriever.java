// android.media.MediaMetadataRetriever -- pulls metadata + frames out of a
// media file. AOSP frameworks/base media/java/android/media/MediaMetadataRetriever.java.
//
// The tracks come from MediaExtractor (the host's demuxer), so the keys a
// container's tracks answer are real: duration, the presence of audio and
// video, the video's size and rotation, the bit rate, the track count. Frames
// are decoded by the host for a source with a name (see "frame / image
// extraction" below). Tags (title, artist, album art) are not read: those
// keys and getEmbeddedPicture() answer null, as AOSP does for a file without them.
package android.media;

public class MediaMetadataRetriever implements AutoCloseable {
    public static final int OPTION_PREVIOUS_SYNC = 0x00;
    public static final int OPTION_NEXT_SYNC = 0x01;
    public static final int OPTION_CLOSEST_SYNC = 0x02;
    public static final int OPTION_CLOSEST = 0x03;

    public static final int METADATA_KEY_CD_TRACK_NUMBER = 0;
    public static final int METADATA_KEY_ALBUM = 1;
    public static final int METADATA_KEY_ARTIST = 2;
    public static final int METADATA_KEY_AUTHOR = 3;
    public static final int METADATA_KEY_COMPOSER = 4;
    public static final int METADATA_KEY_DATE = 5;
    public static final int METADATA_KEY_GENRE = 6;
    public static final int METADATA_KEY_TITLE = 7;
    public static final int METADATA_KEY_YEAR = 8;
    public static final int METADATA_KEY_DURATION = 9;
    public static final int METADATA_KEY_NUM_TRACKS = 10;
    public static final int METADATA_KEY_WRITER = 11;
    public static final int METADATA_KEY_MIMETYPE = 12;
    public static final int METADATA_KEY_ALBUMARTIST = 13;
    public static final int METADATA_KEY_DISC_NUMBER = 14;
    public static final int METADATA_KEY_COMPILATION = 15;
    public static final int METADATA_KEY_HAS_AUDIO = 16;
    public static final int METADATA_KEY_HAS_VIDEO = 17;
    public static final int METADATA_KEY_VIDEO_WIDTH = 18;
    public static final int METADATA_KEY_VIDEO_HEIGHT = 19;
    public static final int METADATA_KEY_BITRATE = 20;
    public static final int METADATA_KEY_VIDEO_ROTATION = 24;
    public static final int METADATA_KEY_CAPTURE_FRAMERATE = 25;
    public static final int METADATA_KEY_HAS_IMAGE = 26;
    public static final int METADATA_KEY_IMAGE_COUNT = 27;
    public static final int METADATA_KEY_IMAGE_WIDTH = 29;
    public static final int METADATA_KEY_IMAGE_HEIGHT = 30;
    // 32, not 38. frameworks/base/media/java/android/media/MediaMetadataRetriever.java
    // (android-11.0.0_r48). extractMetadata() switches on this key, so a wrong value
    // reads back a different (or no) field.
    public static final int METADATA_KEY_VIDEO_FRAME_COUNT = 32;

    private String mPath;   // a path or an http(s) URL the host can open by name; null for a source that has none
    private MediaExtractor mExtractor;   // the source's tracks; null when the host cannot read it

    public MediaMetadataRetriever() {}

    private void open(String pathOrUrl) {
        closeDemux();
        mPath = pathOrUrl;
        MediaExtractor ex = new MediaExtractor();
        try {
            ex.setDataSource(pathOrUrl);
            mExtractor = ex;
        } catch (java.io.IOException | RuntimeException e) {
            // A file the host's demuxer cannot read: every key answers null,
            // AOSP's answer for a key the source does not have.
            ex.release();
        }
    }

    // AOSP validates the source up front and throws IllegalArgumentException
    // for a local path that plainly does not exist; a file that exists but
    // that the host cannot demux is not an error here, its keys are null.
    public void setDataSource(String path) throws IllegalArgumentException {
        closeDemux();
        mPath = null;
        if (path == null) throw new IllegalArgumentException("null path");
        String p = path.startsWith("file://") ? path.substring(7) : path;
        if (p.indexOf("://") < 0 && !new java.io.File(p).exists())
            throw new IllegalArgumentException(p + " does not exist");
        if (p.indexOf("://") >= 0 && !p.startsWith("http://") && !p.startsWith("https://")) {
            mPath = null;   // a scheme only a Context can resolve
            return;
        }
        open(p);
    }
    public void setDataSource(String uri, java.util.Map<String, String> headers) { setDataSource(uri); }
    public void setDataSource(java.io.FileDescriptor fd) { setDataSource(fd, 0, 0x7ffffffffffffffL); }
    /** The descriptor's tracks answer extractMetadata; frames need a source
     *  with a name (a path or a URL) and are null for this one. */
    public void setDataSource(java.io.FileDescriptor fd, long offset, long length) {
        closeDemux();
        mPath = null;
        MediaExtractor ex = new MediaExtractor();
        try {
            ex.setDataSource(fd, offset, length);
            mExtractor = ex;
        } catch (java.io.IOException | RuntimeException e) {
            ex.release();
            throw new IllegalArgumentException("setDataSource failed: " + e.getMessage());
        }
    }
    public void setDataSource(android.content.Context context, android.net.Uri uri) {
        if (uri == null) throw new IllegalArgumentException("null uri");
        String scheme = uri.getScheme();
        if (scheme == null || "file".equals(scheme)) {
            setDataSource(uri.getPath());
        } else if ("http".equals(scheme) || "https".equals(scheme)) {
            setDataSource(uri.toString());
        } else {
            closeDemux();
            mPath = null;
            MediaExtractor ex = new MediaExtractor();
            try {
                ex.setDataSource(context, uri, null);
                mExtractor = ex;
            } catch (java.io.IOException | RuntimeException e) {
                ex.release();
                throw new IllegalArgumentException("setDataSource failed: " + e.getMessage());
            }
        }
    }
    public void setDataSource(android.content.res.AssetFileDescriptor afd) {
        if (afd == null) throw new IllegalArgumentException("null afd");
        if (afd.getDeclaredLength() < 0) setDataSource(afd.getFileDescriptor());
        else setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), afd.getDeclaredLength());
    }
    public void setDataSource(MediaDataSource dataSource) {
        closeDemux();
        mPath = null;
        MediaExtractor ex = new MediaExtractor();
        try {
            ex.setDataSource(dataSource);
            mExtractor = ex;
        } catch (java.io.IOException | RuntimeException e) {
            ex.release();
            throw new IllegalArgumentException("setDataSource failed: " + e.getMessage());
        }
    }

    /** The first track whose mime starts with `kind` ("video/" or "audio/"), or null. */
    private MediaFormat track(String kind) {
        if (mExtractor == null) return null;
        for (int i = 0; i < mExtractor.getTrackCount(); i++) {
            MediaFormat f = mExtractor.getTrackFormat(i);
            if (f.getString(MediaFormat.KEY_MIME, "").startsWith(kind)) return f;
        }
        return null;
    }

    private long durationUs() {
        long us = 0;
        for (int i = 0; mExtractor != null && i < mExtractor.getTrackCount(); i++) {
            us = Math.max(us, mExtractor.getTrackFormat(i).getLong(MediaFormat.KEY_DURATION, 0));
        }
        return us;
    }

    public String extractMetadata(int keyCode) {
        if (mExtractor == null) return keyCode == METADATA_KEY_MIMETYPE ? mimeTypeFromPath() : null;
        MediaFormat video = track("video/");
        switch (keyCode) {
            // AOSP: "If this key exists the media contains audio content" --
            // the value itself is the conventional presence marker "yes".
            case METADATA_KEY_HAS_AUDIO: return track("audio/") != null ? "yes" : null;
            case METADATA_KEY_HAS_VIDEO: return video != null ? "yes" : null;
            case METADATA_KEY_VIDEO_WIDTH:
                return video != null ? String.valueOf(video.getInteger(MediaFormat.KEY_WIDTH, 0)) : null;
            case METADATA_KEY_VIDEO_HEIGHT:
                return video != null ? String.valueOf(video.getInteger(MediaFormat.KEY_HEIGHT, 0)) : null;
            case METADATA_KEY_VIDEO_ROTATION:
                return video != null ? String.valueOf(video.getInteger(MediaFormat.KEY_ROTATION, 0)) : null;
            case METADATA_KEY_DURATION: {
                // AOSP: duration in ms, as a decimal string.
                long us = durationUs();
                return us > 0 ? String.valueOf(us / 1000) : null;
            }
            case METADATA_KEY_BITRATE: {
                // AOSP: average bitrate in bits/sec: the tracks' own when the
                // container states them, else file size over duration.
                long sum = 0;
                for (int i = 0; i < mExtractor.getTrackCount(); i++) {
                    sum += mExtractor.getTrackFormat(i).getInteger(MediaFormat.KEY_BIT_RATE, 0);
                }
                if (sum > 0) return String.valueOf(sum);
                long us = durationUs();
                long bytes = mPath != null && mPath.indexOf("://") < 0 ? new java.io.File(mPath).length() : 0;
                return us > 0 && bytes > 0 ? String.valueOf(Math.round(bytes * 8.0 * 1_000_000.0 / us)) : null;
            }
            case METADATA_KEY_MIMETYPE: {
                // AOSP examples: "video/mp4", "audio/mp4": the container's
                // mime, by the path's extension when it has one.
                String byName = mimeTypeFromPath();
                return byName != null ? byName : video != null ? "video/mp4" : null;
            }
            case METADATA_KEY_NUM_TRACKS:
                return String.valueOf(mExtractor.getTrackCount());
            default:
                // Tags (title, artist, ...) are not read from the container.
                return null;
        }
    }

    // ---- frame / image extraction -----------------------------------------
    // A frame is decoded by the host (com.macapk.loader.HostMedia.nativeFrameAt:
    // the sync sample before the time, decoded on to the time) for a source
    // with a name: a path or an http(s) URL. For a FileDescriptor, a content
    // Uri and a MediaDataSource, and for a file without video, these return
    // null, AOSP's answer when a frame "cannot be retrieved". The option is
    // not told apart: the frame is the one at the time, which is
    // OPTION_CLOSEST's, and no further from it than the sync frames the
    // other options name. Frames by index and HEIF images are not read.

    public android.graphics.Bitmap getFrameAtTime(long timeUs, int option) {
        if (mPath == null || mExtractor == null || track("video/") == null) return null;
        int[] dims = new int[2];
        int[] argb;
        try {
            argb = com.macapk.loader.HostMedia.nativeFrameAt(mPath, timeUs, dims);
        } catch (LinkageError e) {
            return null;
        }
        if (argb == null || dims[0] <= 0 || dims[1] <= 0) return null;
        return android.graphics.Bitmap.createBitmap(argb, dims[0], dims[1], android.graphics.Bitmap.Config.ARGB_8888);
    }
    public android.graphics.Bitmap getFrameAtTime(long timeUs) { return getFrameAtTime(timeUs, OPTION_CLOSEST_SYNC); }
    public android.graphics.Bitmap getFrameAtTime() { return getFrameAtTime(-1, OPTION_CLOSEST_SYNC); }
    public android.graphics.Bitmap getFrameAtTime(long timeUs, int option, BitmapParams params) {
        java.util.Objects.requireNonNull(params, "params must not be null");
        return getFrameAtTime(timeUs, option);
    }
    public android.graphics.Bitmap getScaledFrameAtTime(long timeUs, int option,
                                                        int dstWidth, int dstHeight) {
        if (dstWidth <= 0 || dstHeight <= 0) throw new IllegalArgumentException("Invalid width or height");
        android.graphics.Bitmap frame = getFrameAtTime(timeUs, option);
        if (frame == null) return null;
        // AOSP keeps the aspect ratio inside the box asked for.
        float scale = Math.min(dstWidth / (float) frame.getWidth(), dstHeight / (float) frame.getHeight());
        return android.graphics.Bitmap.createScaledBitmap(frame, Math.max(1, Math.round(frame.getWidth() * scale)),
                Math.max(1, Math.round(frame.getHeight() * scale)), true);
    }
    public android.graphics.Bitmap getScaledFrameAtTime(long timeUs, int option,
            int dstWidth, int dstHeight, BitmapParams params) {
        java.util.Objects.requireNonNull(params, "params must not be null");
        return getScaledFrameAtTime(timeUs, option, dstWidth, dstHeight);
    }
    public android.graphics.Bitmap getFrameAtIndex(int frameIndex) { return null; }
    public android.graphics.Bitmap getFrameAtIndex(int frameIndex, BitmapParams params) {
        java.util.Objects.requireNonNull(params, "params must not be null");
        return null;
    }
    /** AOSP: @NonNull -- "@throws IllegalStateException if the container does
     *  not have video or image tracks". None does here (audio-only demuxer), so
     *  that exception is the honest answer; a null from an @NonNull method would
     *  NPE somewhere else in the app instead. */
    public java.util.List<android.graphics.Bitmap> getFramesAtIndex(int frameIndex,
            int numFrames) {
        throw new IllegalStateException("The source has no video or image track");
    }
    public java.util.List<android.graphics.Bitmap> getFramesAtIndex(int frameIndex,
            int numFrames, BitmapParams params) {
        java.util.Objects.requireNonNull(params, "params must not be null");
        throw new IllegalStateException("The source has no video or image track");
    }
    public android.graphics.Bitmap getImageAtIndex(int imageIndex) { return null; }
    public android.graphics.Bitmap getImageAtIndex(int imageIndex, BitmapParams params) {
        java.util.Objects.requireNonNull(params, "params must not be null");
        return null;
    }
    public android.graphics.Bitmap getPrimaryImage() { return null; }
    public android.graphics.Bitmap getPrimaryImage(BitmapParams params) {
        java.util.Objects.requireNonNull(params, "params must not be null");
        return null;
    }

    /** API 30. Lets the caller ask for a particular Bitmap.Config and learn
     *  which one was actually produced. AOSP's default preferred config is
     *  ARGB_8888, and actualConfig starts equal to it. */
    public static final class BitmapParams {
        private android.graphics.Bitmap.Config mPreferred =
                android.graphics.Bitmap.Config.ARGB_8888;
        private android.graphics.Bitmap.Config mActual =
                android.graphics.Bitmap.Config.ARGB_8888;

        public BitmapParams() { }

        public void setPreferredConfig(android.graphics.Bitmap.Config config) {
            mPreferred = java.util.Objects.requireNonNull(config, "config must not be null");
        }
        public android.graphics.Bitmap.Config getPreferredConfig() { return mPreferred; }
        /** AOSP writes this only when a bitmap was actually produced; nothing
         *  here produces one, so it keeps its initial value. */
        public android.graphics.Bitmap.Config getActualConfig() { return mActual; }
    }
    public byte[] getEmbeddedPicture() { return null; }
    public void release() { closeDemux(); mPath = null; }
    @Override public void close() { release(); }

    private void closeDemux() {
        if (mExtractor != null) { mExtractor.release(); mExtractor = null; }
    }

    private String mimeTypeFromPath() {
        if (mPath == null) return null;
        int slash = Math.max(mPath.lastIndexOf('/'), mPath.lastIndexOf('\\'));
        int dot = mPath.lastIndexOf('.');
        if (dot <= slash || dot == mPath.length() - 1) return null;
        return android.webkit.MimeTypeMap.getSingleton().getMimeTypeFromExtension(mPath.substring(dot + 1));
    }
}
