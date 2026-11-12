package android.icu.text;

import java.util.Collections;
import java.util.EnumSet;
import java.util.Set;

/**
 * Internationalized domain names: Unicode to ASCII ("xn--") and back, with the
 * per-call error report ICU gives (android.icu.text.IDNA, API 24).
 *
 * The conversion is the JDK's java.net.IDN, which is IDNA2003 where ICU's
 * getUTS46Instance is UTS #46. They agree on the names in real use; UTS #46
 * also lower-cases, which is done here, and its extra checks that java.net.IDN
 * does not make (bidi, joiners, the hyphen positions) are made here only as far
 * as the label rules below go. As in ICU a failed conversion throws nothing:
 * the errors are in the Info, and the destination gets the name as far as it
 * could be converted.
 *
 * Source: https://developer.android.com/reference/android/icu/text/IDNA
 */
public abstract class IDNA {

    /** Default options value: none of the other options are set. */
    public static final int DEFAULT = 0;
    /** Restrict labels to letters, digits and hyphens (STD3 ASCII rules). */
    public static final int USE_STD3_RULES = 2;
    /** Check the labels for the IDNA2008 bidi rules. */
    public static final int CHECK_BIDI = 4;
    /** Check the labels for the IDNA2008 joiner rules. */
    public static final int CHECK_CONTEXTJ = 8;
    /** Nontransitional processing in the to-ASCII direction (sharp s and final sigma stay). */
    public static final int NONTRANSITIONAL_TO_ASCII = 0x10;
    /** Nontransitional processing in the to-Unicode direction. */
    public static final int NONTRANSITIONAL_TO_UNICODE = 0x20;
    /** Check the labels for the IDNA2008 CONTEXTO rules. */
    public static final int CHECK_CONTEXTO = 0x40;

    /** What was wrong with a label or a name. */
    public enum Error {
        EMPTY_LABEL,
        LABEL_TOO_LONG,
        DOMAIN_NAME_TOO_LONG,
        LEADING_HYPHEN,
        TRAILING_HYPHEN,
        HYPHEN_3_4,
        LEADING_COMBINING_MARK,
        DISALLOWED,
        PUNYCODE,
        LABEL_HAS_DOT,
        INVALID_ACE_LABEL,
        BIDI,
        CONTEXTJ,
        CONTEXTO_PUNCTUATION,
        CONTEXTO_DIGITS
    }

    /** The report of one conversion. Reusable: each call resets it. */
    public static final class Info {
        private final EnumSet<Error> errors = EnumSet.noneOf(Error.class);

        public Info() {}

        /** Were there IDNA processing errors? */
        public boolean hasErrors() { return !errors.isEmpty(); }

        /** The errors of the last conversion; unmodifiable. */
        public Set<Error> getErrors() { return Collections.unmodifiableSet(errors); }

        /** Whether transitional and nontransitional processing give different
         *  results for the input. Not worked out here: java.net.IDN has one mode. */
        public boolean isTransitionalDifferent() { return false; }
    }

    /** ICU hides this constructor from apps; subclasses are ICU's own. */
    protected IDNA() {}

    /** An instance for UTS #46 processing with the given option bits. */
    public static IDNA getUTS46Instance(int options) {
        return new HostIdna(options);
    }

    /** Converts one label to its ASCII form into dest, which is emptied first. */
    public abstract StringBuilder labelToASCII(CharSequence label, StringBuilder dest, Info info);

    /** Converts one label to its Unicode form into dest, which is emptied first. */
    public abstract StringBuilder labelToUnicode(CharSequence label, StringBuilder dest, Info info);

    /** Converts a whole domain name to its ASCII form into dest, which is emptied first. */
    public abstract StringBuilder nameToASCII(CharSequence name, StringBuilder dest, Info info);

    /** Converts a whole domain name to its Unicode form into dest, which is emptied first. */
    public abstract StringBuilder nameToUnicode(CharSequence name, StringBuilder dest, Info info);

    private static final class HostIdna extends IDNA {
        private final int flags;

        HostIdna(int options) {
            flags = ((options & USE_STD3_RULES) != 0) ? java.net.IDN.USE_STD3_ASCII_RULES : 0;
        }

        @Override public StringBuilder labelToASCII(CharSequence label, StringBuilder dest, Info info) {
            return label(label.toString(), dest, info, true);
        }

        @Override public StringBuilder labelToUnicode(CharSequence label, StringBuilder dest, Info info) {
            return label(label.toString(), dest, info, false);
        }

        /** convert() first: it is what resets the Info. */
        private StringBuilder label(String label, StringBuilder dest, Info info, boolean toAscii) {
            convert(label, dest, info, toAscii);
            if (label.indexOf('.') >= 0) add(info, Error.LABEL_HAS_DOT);
            return dest;
        }

        @Override public StringBuilder nameToASCII(CharSequence name, StringBuilder dest, Info info) {
            convert(name.toString(), dest, info, true);
            // 253 characters, or 254 with the root's trailing dot.
            int length = dest.length();
            if (length > 253 && !(length == 254 && dest.charAt(253) == '.')) add(info, Error.DOMAIN_NAME_TOO_LONG);
            return dest;
        }

        @Override public StringBuilder nameToUnicode(CharSequence name, StringBuilder dest, Info info) {
            return convert(name.toString(), dest, info, false);
        }

        /** Label by label, so that one bad label is reported and the others still convert. */
        private StringBuilder convert(String name, StringBuilder dest, Info info, boolean toAscii) {
            info.errors.clear();
            dest.setLength(0);
            String[] labels = name.split("[.。．｡]", -1);
            for (int i = 0; i < labels.length; i++) {
                if (i > 0) dest.append('.');
                String label = labels[i];
                if (label.isEmpty()) {
                    // The root's empty label, after the last dot, is not an error.
                    if (i < labels.length - 1 || labels.length == 1) add(info, Error.EMPTY_LABEL);
                    continue;
                }
                dest.append(convertLabel(label, info, toAscii));
            }
            return dest;
        }

        private String convertLabel(String label, Info info, boolean toAscii) {
            String ascii;
            try {
                ascii = java.net.IDN.toASCII(label, flags | java.net.IDN.ALLOW_UNASSIGNED);
            } catch (IllegalArgumentException e) {
                add(info, label.regionMatches(true, 0, "xn--", 0, 4) ? Error.INVALID_ACE_LABEL : Error.DISALLOWED);
                if (label.length() > 63) add(info, Error.LABEL_TOO_LONG);
                return label;
            }
            ascii = ascii.toLowerCase(java.util.Locale.ROOT);
            if (ascii.length() > 63) add(info, Error.LABEL_TOO_LONG);
            String unicode = java.net.IDN.toUnicode(ascii, flags | java.net.IDN.ALLOW_UNASSIGNED);
            if (ascii.startsWith("xn--") && unicode.equals(ascii)) {
                // java.net.IDN.toUnicode gives back its input when the Punycode does not decode.
                add(info, Error.PUNYCODE);
            }
            if (unicode.startsWith("-")) add(info, Error.LEADING_HYPHEN);
            if (unicode.endsWith("-")) add(info, Error.TRAILING_HYPHEN);
            if (unicode.length() >= 4 && unicode.charAt(2) == '-' && unicode.charAt(3) == '-'
                    && !ascii.startsWith("xn--")) {
                add(info, Error.HYPHEN_3_4);
            }
            int type = Character.getType(unicode.codePointAt(0));
            if (type == Character.NON_SPACING_MARK || type == Character.ENCLOSING_MARK
                    || type == Character.COMBINING_SPACING_MARK) {
                add(info, Error.LEADING_COMBINING_MARK);
            }
            return toAscii ? ascii : unicode;
        }

        private static void add(Info info, Error error) { info.errors.add(error); }
    }
}
