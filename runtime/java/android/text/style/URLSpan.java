// android.text.style.URLSpan -- AOSP span that marks a run of text as a
// hyperlink and remembers its target URL. AOSP:
//   public class URLSpan extends ClickableSpan { final String mURL; ...
//   public URLSpan(String url); public String getURL(); }
//
// androidx LinkifyCompat.addLinks(textView, mask) constructs
// `new URLSpan(url)` and setSpan()s it; F-Droid's AppDetails header
// binder then reads back each span via getSpans(URLSpan.class) and
// getURL() to wire its own click handling. The previous bare stub had
// neither the (String) ctor nor getURL(), so the bind threw and aborted
// the detail RecyclerView's layout. Appearance (link colour + underline) comes
// from ClickableSpan, as in AOSP; the no-arg ctor is kept for any caller that
// constructed the old stub.
package android.text.style;

// AOSP: `extends ClickableSpan`. This one is load-bearing at runtime, not just for
// casts: TextView/LinkMovementMethod find tappable spans with
// `getSpans(..., ClickableSpan.class)`, so a URLSpan outside that hierarchy is
// simply never found and links never respond to a tap.
// frameworks/base/core/java/android/text/style/URLSpan.java
// 2026-08-23 conformance sweep: implements android.text.ParcelableSpan.
// Payload (a single String, the URL) and type id 11 verified against
// frameworks/base/core/java/android/text/style/URLSpan.java
// (android16-release), and matches TextUtils.java's own writeSpanPayload
// for URL_SPAN.
public class URLSpan extends ClickableSpan implements android.text.ParcelableSpan {
    private final String mURL;

    public URLSpan() { this.mURL = null; }
    public URLSpan(String url) { this.mURL = url; }

    public String getURL() { return mURL; }

    @Override public int getSpanTypeId() { return getSpanTypeIdInternal(); }
    @Override public int getSpanTypeIdInternal() { return android.text.TextUtils.URL_SPAN; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) {
        writeToParcelInternal(dest, flags);
    }
    @Override public void writeToParcelInternal(android.os.Parcel dest, int flags) {
        dest.writeString(mURL);
    }
    public static final android.os.Parcelable.Creator<URLSpan> CREATOR =
            new android.os.Parcelable.Creator<URLSpan>() {
        @Override public URLSpan createFromParcel(android.os.Parcel in) {
            return new URLSpan(in.readString());
        }
        @Override public URLSpan[] newArray(int size) { return new URLSpan[size]; }
    };

    /** AOSP's: an ACTION_VIEW of the URL, named as coming from this app, started from the
     *  widget's context. It goes through intent resolution like any other start, so the app's
     *  own filters take its own links, and a URL leaves the app only where a device's would
     *  (ActivityHost.hostActionFor: web, mailto, tel, geo, behind the user-input gate). A
     *  link to any other scheme is an ActivityNotFoundException, logged, as on a device; it
     *  must not reach Windows' protocol handlers (search-ms:, ms-msdt:...) from a message's
     *  text. */
    @Override public void onClick(android.view.View widget) {
        android.net.Uri uri = android.net.Uri.parse(getURL());
        android.content.Context context = widget.getContext();
        android.content.Intent intent = new android.content.Intent(android.content.Intent.ACTION_VIEW, uri);
        intent.putExtra(android.provider.Browser.EXTRA_APPLICATION_ID, context.getPackageName());
        try {
            context.startActivity(intent);
        } catch (android.content.ActivityNotFoundException e) {
            android.util.Log.w("URLSpan", "Actvity was not found for intent, " + intent.toString());
        }
    }

    // NO updateDrawState override, on purpose. AOSP URLSpan does not declare
    // one (frameworks/base/core/java/android/text/style/URLSpan.java), so a
    // link inherits ClickableSpan.updateDrawState: `ds.setColor(ds.linkColor);
    // ds.setUnderlineText(true);`. The override that used to sit here (v727,
    // added before this class extended ClickableSpan) set only the underline,
    // so once Canvas started drawing UNDERLINE_TEXT_FLAG every link came out
    // underlined in the BODY text colour instead of the theme's link colour
    // (measured on deskclock's "... and much more!" link, 2026-09-25).
}


