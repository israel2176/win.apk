// CompiledXml -- an AttributeSet that can say which element of which compiled XML file it reads
// (docs/research/aosp-views.md, step R1).
//
// AOSP's resources code reads an element's attributes from the compiled file itself (the
// XmlBlock.Parser standing on it), not through the AttributeSet interface. Win.apk's inflater
// hands views a set over a DOM element instead; with AOSP's resources (-Dwinapk.aosp.res=1) the
// set says where its element came from (BinaryXmlParser records it), and the AOSP side opens a
// parser on the same start tag (windows/java/aosp_res, AttrSets). Off, nothing calls this.
package com.macapk.loader.res;

public interface CompiledXml {
    /** The compiled XML file the element was parsed from, or null (a text file, a made-up set). */
    byte[] compiledXml();

    /** The element's start tag in that file, counted from 0 in document order. */
    int startTagIndex();
}
