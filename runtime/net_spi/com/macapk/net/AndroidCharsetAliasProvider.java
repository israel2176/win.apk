// AndroidCharsetAliasProvider -- the charset names Android accepts with '_'
// where the canonical name has '-' ("UTF_8", "UTF_16LE", "US_ASCII").
//
// WHY: Android's Charset.forName falls through to ICU (libcore
// NativeConverter.charsetForName -> ucnv_getCanonicalName), and ICU matches a
// converter alias ignoring case and every '-', '_' and space between the
// letters and digits (ucnv_io.cpp, ucnv_compareNames / ucnv_io_stripASCIIForCompare).
// So "UTF_8" is UTF-8 on a device, and apps pass it: a bank app
// (com.ideomobile.mercantile) calls URLEncoder.encode(s, "UTF_8"), which on
// stock OpenJDK throws UnsupportedEncodingException, since UTF-8's aliases there
// are only "UTF8" and "unicode-1-1-utf-8". Every String / URLEncoder /
// InputStreamReader name lookup ends in Charset.forName, whose last stop is
// the CharsetProvider services on the class path, consulted only after the
// JDK's own charsets miss, so this costs nothing on the common path.
//
// Conservative on purpose: only '_' -> '-', the spelling difference real code
// has. ICU's full rule ("utf8", "U-T-F-8") is not copied: the JDK knows "UTF8"
// already, and the rest is not something apps write.
package com.macapk.net;

import java.nio.charset.Charset;
import java.nio.charset.spi.CharsetProvider;
import java.util.Collections;
import java.util.Iterator;

public final class AndroidCharsetAliasProvider extends CharsetProvider {
    @Override public Charset charsetForName(String charsetName) {
        if (charsetName == null || charsetName.indexOf('_') < 0) return null;
        String dashed = charsetName.replace('_', '-');
        // Charset.forName does not come back here for a name nobody knows:
        // its provider lookup is gated per thread, so a nested miss answers
        // UnsupportedCharsetException, and that is a miss here too.
        try {
            return Charset.isSupported(dashed) ? Charset.forName(dashed) : null;
        } catch (IllegalArgumentException e) {
            return null;
        }
    }

    /** No charsets of its own: these are other names for the JDK's, so
     *  Charset.availableCharsets() must not list them twice. */
    @Override public Iterator<Charset> charsets() {
        return Collections.emptyIterator();
    }
}
