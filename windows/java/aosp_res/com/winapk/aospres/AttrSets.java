// AttrSets -- the XmlBlock.Parser behind an AttributeSet, for AOSP's resources code.
//
// AOSP reads an element's attributes during obtainStyledAttributes / obtainAttributes from the
// compiled XML itself: it casts the AttributeSet to XmlBlock.Parser and hands libandroidfw the
// parser's state, which stands on the element's start tag (AttributeResolution's ApplyStyle and
// RetrieveAttributes). AospResRewrite.java turns each of those casts into a call to parser() here.
//
// A set that is a parser comes back as it is. Win.apk's inflater hands its views a set over a DOM
// element (LayoutInflater.DomBackedAttributeSet); the element remembers which compiled file and
// which start tag it came from (com.macapk.loader.res.CompiledXml), so a parser over the same bytes
// is moved to that start tag and given back: the attribute values AOSP resolves are the compiled
// ones, as on a device. Any other set (one made up in code) has no compiled form: null, which
// AOSP's code takes as "no XML attributes" (the style and theme still apply), and one log line per
// kind of set.
package com.winapk.aospres;

import android.content.res.XmlBlock;
import android.util.AttributeSet;

import com.macapk.loader.res.CompiledXml;

import java.util.Map;
import java.util.Set;
import java.util.WeakHashMap;
import java.util.concurrent.ConcurrentHashMap;

import org.xmlpull.v1.XmlPullParser;

public final class AttrSets {
    private AttrSets() {}

    /** One compiled file's block and a parser over it, moved forward as the inflater goes. */
    private static final class Doc {
        final XmlBlock block;
        XmlBlock.Parser parser;
        int at = -1;  // the start tag the parser stands on

        Doc(byte[] bytes) { block = new XmlBlock(bytes); }
    }

    // By the bytes' identity (a byte[] has identity equality): the document's own array, kept by
    // its DOM, so an entry goes with the layout.
    private static final Map<byte[], Doc> DOCS = new WeakHashMap<>();
    private static final Set<String> WARNED = ConcurrentHashMap.newKeySet();

    public static XmlBlock.Parser parser(Object set) {
        if (set == null || set instanceof XmlBlock.Parser) return (XmlBlock.Parser) set;
        if (set instanceof CompiledXml c) {
            byte[] bytes = c.compiledXml();
            int index = c.startTagIndex();
            if (bytes != null && index >= 0) {
                XmlBlock.Parser p = at(bytes, index);
                if (p != null) return p;
            }
        }
        if (set instanceof AttributeSet && WARNED.add(set.getClass().getName())) {
            AospRes.log("an AttributeSet with no compiled form (" + set.getClass().getName()
                    + "): its attributes are not read by AOSP's resources");
        }
        return null;
    }

    private static XmlBlock.Parser at(byte[] bytes, int index) {
        Doc d;
        synchronized (DOCS) {
            d = DOCS.computeIfAbsent(bytes, Doc::new);
        }
        synchronized (d) {
            try {
                if (d.parser == null || index < d.at) {
                    // A parser closed here leaves the block open: a TypedArray that took strings from
                    // it reads them from the block's string pool, not from the parser.
                    if (d.parser != null) d.parser.close();
                    d.parser = (XmlBlock.Parser) d.block.newParser();
                    d.at = -1;
                }
                while (d.at < index) {
                    int e = d.parser.next();
                    if (e == XmlPullParser.END_DOCUMENT) return null;
                    if (e == XmlPullParser.START_TAG) d.at++;
                }
                return d.parser;
            } catch (Exception e) {
                AospRes.log("the compiled XML of a layout could not be read again: " + e);
                return null;
            }
        }
    }
}
