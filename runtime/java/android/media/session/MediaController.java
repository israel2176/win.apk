// android.media.session.MediaController -- the controller side of a platform
// media session (poll state/metadata, send transport commands). AOSP
// frameworks/base media/java/android/media/session/MediaController.java.
//
// AOSP's MediaController talks to a system-side session record over Binder,
// so it works identically regardless of which process published the session.
// Mac.apk has no such cross-process transport (see CLAUDE.md: no emulation/
// virtualization/IPC to a separate system process), but every app that reads
// its OWN session back is already running in the same process as that
// session -- MediaSession.Token carries a direct in-process reference (see
// MediaSession.Token) precisely so this class can answer from the live
// session instead of a permanent null. When the token has no owner (rebuilt
// from a Parcel, or genuinely no session -- see Token), the getters fall back
// to AOSP's own "no session" answers (null / RATING_NONE / 0 / empty), so a
// disconnected controller is still exactly as safe to call as before.
//
// Transport commands (play/pause/seekTo/...) from a controller reach the
// in-process session's Callback on its Handler, as AOSP's session record routes
// them (an app's own UI drives its player through its controller). Callback
// delivery the other way -- OUT of the session, e.g. onMetadataChanged when the
// session app updates its now-playing info -- is wired below too.
package android.media.session;

import android.content.Context;

public final class MediaController {

    private final MediaSession.Token mToken;
    // The in-process session this controller was built from, or null (no
    // token, or a token with no local owner -- see MediaSession.Token).
    private final MediaSession mSession;
    private final TransportControls mTransportControls = new TransportControls();

    public MediaController() { mToken = null; mSession = null; }
    public MediaController(Context context, MediaSession.Token token) {
        mToken = token;
        mSession = token != null ? token.owner : null;
    }

    public String getPackageName() { return null; }
    public MediaSession.Token getSessionToken() { return mToken; }
    public PlaybackState getPlaybackState() { return mSession != null ? mSession.getPlaybackState() : null; }
    public android.media.MediaMetadata getMetadata() { return mSession != null ? mSession.getMetadata() : null; }
    public java.util.List<MediaSession.QueueItem> getQueue() { return mSession != null ? mSession.getQueue() : null; }
    public CharSequence getQueueTitle() { return mSession != null ? mSession.getQueueTitle() : null; }
    public android.os.Bundle getExtras() { return mSession != null ? mSession.getExtras() : null; }
    public int getRatingType() {
        return mSession != null ? mSession.getRatingType() : android.media.Rating.RATING_NONE;
    }
    public long getFlags() { return mSession != null ? mSession.getFlags() : 0; }
    public PlaybackInfo getPlaybackInfo() { return null; }
    public android.app.PendingIntent getSessionActivity() {
        return mSession != null ? mSession.getSessionActivity() : null;
    }

    public TransportControls getTransportControls() { return mTransportControls; }

    // Delegates to the owning session's callback registry (package-private
    // add/removeControllerCallback) when this controller was built from an
    // in-process Token, so onPlaybackStateChanged/onMetadataChanged/etc.
    // actually fire when the session updates -- see MediaSession.setXxx().
    // The Handler parameter is accepted but not used to hop threads (matches
    // this class's own existing setCallback(Callback,Handler) precedent, and
    // MediaPlayer/SoundPool's established direct-dispatch idiom elsewhere in
    // this package).
    public void registerCallback(Callback callback) { registerCallback(callback, null); }
    public void registerCallback(Callback callback, android.os.Handler handler) {
        if (mSession != null) mSession.addControllerCallback(callback);
    }
    public void unregisterCallback(Callback callback) {
        if (mSession != null) mSession.removeControllerCallback(callback);
    }

    /** AOSP: a media key for the session, which its Callback.onMediaButtonEvent gets as an
     *  ACTION_MEDIA_BUTTON intent (MediaSession.Callback's default then turns it into
     *  onPlay/onPause...). True when there is a session to take it. */
    public boolean dispatchMediaButtonEvent(android.view.KeyEvent keyEvent) {
        if (keyEvent == null) throw new IllegalArgumentException("KeyEvent may not be null");
        if (!android.view.KeyEvent.isMediaSessionKey(keyEvent.getKeyCode())) return false;
        android.content.Intent intent = new android.content.Intent(android.content.Intent.ACTION_MEDIA_BUTTON)
                .putExtra(android.content.Intent.EXTRA_KEY_EVENT, keyEvent);
        return toSession(cb -> cb.onMediaButtonEvent(intent));
    }
    public void sendCommand(String command, android.os.Bundle args, android.os.ResultReceiver cb) {
        if (android.text.TextUtils.isEmpty(command)) throw new IllegalArgumentException("command cannot be null or empty");
        toSession(c -> c.onCommand(command, args, cb));
    }
    public void setVolumeTo(int value, int flags) {}
    public void adjustVolume(int direction, int flags) {}

    /** What AOSP's session record does with a controller's command: the owning session's Callback
     *  gets it on that callback's Handler (MediaSession.setCallback). Only a session of this process
     *  has an owner here (see MediaSession.Token); without one, or without a callback, the command
     *  goes nowhere, as to a session that has none. */
    private boolean toSession(java.util.function.Consumer<MediaSession.Callback> command) {
        MediaSession s = mSession;
        MediaSession.Callback cb = s != null ? s.getCallback() : null;
        if (cb == null) return false;
        android.os.Handler h = s.getCallbackHandler();
        if (h == null || h.getLooper().isCurrentThread()) command.accept(cb);
        else h.post(() -> command.accept(cb));
        return true;
    }

    // AOSP MediaController.TransportControls -- issues transport commands to the
    // session: each reaches the session's Callback (toSession), as on a device.
    public final class TransportControls {
        TransportControls() {}
        public void prepare() { toSession(MediaSession.Callback::onPrepare); }
        public void prepareFromMediaId(String mediaId, android.os.Bundle extras) {
            toSession(c -> c.onPrepareFromMediaId(mediaId, extras));
        }
        public void prepareFromSearch(String query, android.os.Bundle extras) {
            toSession(c -> c.onPrepareFromSearch(query, extras));
        }
        public void prepareFromUri(android.net.Uri uri, android.os.Bundle extras) {
            toSession(c -> c.onPrepareFromUri(uri, extras));
        }
        public void play() { toSession(MediaSession.Callback::onPlay); }
        public void playFromMediaId(String mediaId, android.os.Bundle extras) {
            toSession(c -> c.onPlayFromMediaId(mediaId, extras));
        }
        public void playFromSearch(String query, android.os.Bundle extras) {
            toSession(c -> c.onPlayFromSearch(query, extras));
        }
        public void playFromUri(android.net.Uri uri, android.os.Bundle extras) {
            toSession(c -> c.onPlayFromUri(uri, extras));
        }
        public void skipToQueueItem(long id) { toSession(c -> c.onSkipToQueueItem(id)); }
        public void pause() { toSession(MediaSession.Callback::onPause); }
        public void stop() { toSession(MediaSession.Callback::onStop); }
        public void seekTo(long pos) { toSession(c -> c.onSeekTo(pos)); }
        public void fastForward() { toSession(MediaSession.Callback::onFastForward); }
        public void rewind() { toSession(MediaSession.Callback::onRewind); }
        public void skipToNext() { toSession(MediaSession.Callback::onSkipToNext); }
        public void skipToPrevious() { toSession(MediaSession.Callback::onSkipToPrevious); }
        public void setRating(android.media.Rating rating) { toSession(c -> c.onSetRating(rating)); }
        public void setPlaybackSpeed(float speed) { toSession(c -> c.onSetPlaybackSpeed(speed)); }
        public void sendCustomAction(PlaybackState.CustomAction customAction, android.os.Bundle args) {
            if (customAction == null) throw new IllegalArgumentException("CustomAction cannot be null.");
            sendCustomAction(customAction.getAction(), args);
        }
        public void sendCustomAction(String action, android.os.Bundle args) {
            if (android.text.TextUtils.isEmpty(action)) throw new IllegalArgumentException("CustomAction cannot be null.");
            toSession(c -> c.onCustomAction(action, args));
        }
    }

    // AOSP MediaController.Callback -- delivers session changes to the controller.
    // Fires when this controller was built from an in-process session's Token
    // (see registerCallback above and MediaSession.setXxx); otherwise (no
    // token, or a token whose owning session already exited this process)
    // there is nothing to route it from and it never fires, same as AOSP's
    // own "session gone" case.
    public abstract static class Callback {
        public Callback() {}
        public void onSessionDestroyed() {}
        public void onSessionEvent(String event, android.os.Bundle extras) {}
        public void onPlaybackStateChanged(PlaybackState state) {}
        public void onMetadataChanged(android.media.MediaMetadata metadata) {}
        public void onQueueChanged(java.util.List<MediaSession.QueueItem> queue) {}
        public void onQueueTitleChanged(CharSequence title) {}
        public void onExtrasChanged(android.os.Bundle extras) {}
        public void onAudioInfoChanged(PlaybackInfo info) {}
    }

    // AOSP MediaController.PlaybackInfo -- describes how the session's audio is
    // routed (local vs remote). We report a local playback info holder.
    public static final class PlaybackInfo implements android.os.Parcelable {
        public static final int PLAYBACK_TYPE_LOCAL = 1;
        public static final int PLAYBACK_TYPE_REMOTE = 2;
        private final int mVolumeType;
        private final int mVolumeControl;
        private final int mMaxVolume;
        private final int mCurrentVolume;
        private final android.media.AudioAttributes mAudioAttrs;
        private final String mVolumeControlId;
        PlaybackInfo(int type, int control, int max, int current,
                     android.media.AudioAttributes attrs) {
            this(type, control, max, current, attrs, null);
        }
        PlaybackInfo(int type, int control, int max, int current,
                     android.media.AudioAttributes attrs, String volumeControlId) {
            mVolumeType = type;
            mVolumeControl = control;
            mMaxVolume = max;
            mCurrentVolume = current;
            mAudioAttrs = attrs;
            mVolumeControlId = volumeControlId;
        }
        public int getPlaybackType() { return mVolumeType; }
        public int getVolumeControl() { return mVolumeControl; }
        public int getMaxVolume() { return mMaxVolume; }
        public int getCurrentVolume() { return mCurrentVolume; }
        public android.media.AudioAttributes getAudioAttributes() { return mAudioAttrs; }

        /** API 30. @Nullable in AOSP: the id the session's VolumeProvider was
         *  built with, null for a local-playback session. */
        public String getVolumeControlId() { return mVolumeControlId; }

        @Override public int describeContents() { return 0; }

        @Override public void writeToParcel(android.os.Parcel dest, int flags) {
            dest.writeInt(mVolumeType);
            dest.writeInt(mVolumeControl);
            dest.writeInt(mMaxVolume);
            dest.writeInt(mCurrentVolume);
            dest.writeTypedObject(mAudioAttrs, flags);
            dest.writeString(mVolumeControlId);
        }

        public static final android.os.Parcelable.Creator<PlaybackInfo> CREATOR =
                new android.os.Parcelable.Creator<PlaybackInfo>() {
            @Override public PlaybackInfo createFromParcel(android.os.Parcel in) {
                int type = in.readInt();
                int control = in.readInt();
                int max = in.readInt();
                int current = in.readInt();
                android.media.AudioAttributes attrs =
                        in.readTypedObject(android.media.AudioAttributes.CREATOR);
                return new PlaybackInfo(type, control, max, current, attrs, in.readString());
            }
            @Override public PlaybackInfo[] newArray(int size) { return new PlaybackInfo[size]; }
        };
    }

    public android.os.Bundle getSessionInfo() { return new android.os.Bundle(); }

    /** API 33. AOSP: "Gets the session's tag" -- the string the app passed to
     *  the MediaSession constructor -- and it is @NonNull. Read from the
     *  session this controller was built from, so a controller and its session
     *  agree; the empty string when the controller has no session (AOSP's own
     *  answer for a controller whose session went away). */
    public String getTag() {
        MediaSession session = mSession;
        if (session != null) {
            String tag = session.getTag();
            if (tag != null) return tag;
        }
        return "";
    }
}

