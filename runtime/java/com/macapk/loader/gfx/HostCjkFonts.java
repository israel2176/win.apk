// HostCjkFonts -- Windows' own Chinese, Japanese and Korean fonts behind the CJK families of the
// runtime's fonts.xml, when the install does not carry Noto CJK (AospGfx.materializeSystemFonts).
//
// WHY. Android's fonts.xml ends its fallback chain with four CJK families, all one file:
//
//     <family lang="zh-Hans">         NotoSansCJK-Regular.ttc index 2
//     <family lang="zh-Hant,zh-Bopo"> NotoSansCJK-Regular.ttc index 3
//     <family lang="ja">              NotoSansCJK-Regular.ttc index 0
//     <family lang="ko">              NotoSansCJK-Regular.ttc index 1
//
// That file is 19 MB of a 29 MB font folder, and it was left out of the install to keep its size
// down (Hebrew and Latin stay exactly Android's). build_aosp_gfx.sh no longer
// stages it, so without this every Han, kana and Hangul character would be a box: AOSP's parser
// skips a font whose file is missing, and a family with none is dropped.
//
// WHAT. The same four families, in fonts.xml's own place in the chain and with its lang tags,
// backed by the fonts every Windows 10 and 11 has in %WINDIR%\Fonts: Microsoft YaHei, Microsoft
// JhengHei, Yu Gothic and Malgun Gothic (then the older SimSun, MingLiU, Meiryo, MS Gothic, Gulim).
// Only those <font> elements change; every other family, Roboto, Noto Hebrew, Naskh Arabic and the
// emoji among them, is the staged fonts.xml as it is, so text without CJK in it is untouched. The
// result is written to <data>\cache\aosp-fonts\fonts-<hash>.xml and read by the Java side (with the
// Windows files through SystemFonts' updatableFontMap, by the PostScript name the element now
// carries) and, as /system/etc/fonts.xml, by native code (Flutter's Skia), whose /system/fonts finds
// a name in bin\aosp\fonts first and then in %WINDIR%\Fonts (paths.cpp).
//
// METRICS. Windows' faces are not Noto CJK: YaHei, JhengHei and Yu Gothic have taller ascents and
// descents (a line of Chinese with fallback line spacing grows), their advances for punctuation and
// the CJK symbols differ, and their bold is a real cut where a device fakes it (only 400 is listed,
// as fonts.xml lists one face, so minikin still fakes it). Text measures as on a phone only in the
// scripts that keep Android's fonts.
//
// When Windows has none of them (a stripped or Wine prefix), the families are left out and CJK text
// draws boxes, as Android would with no CJK font. A folder that still has NotoSansCJK-Regular.ttc
// (a stage built before) is used as it is.
package com.macapk.loader.gfx;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class HostCjkFonts {
    private HostCjkFonts() {}

    /** The font fonts.xml's CJK families name, which the install no longer carries. */
    static final String NOTO_CJK = "NotoSansCJK-Regular.ttc";

    /**
     * Per fonts.xml family (its first lang tag), Windows' candidates in preference order: a file of
     * %WINDIR%\Fonts and the PostScript names of the face wanted in it. A file whose faces have none
     * of those names (another Windows build's naming) gives its first face, which in each of these
     * files is the plain one (not the UI or proportional variant).
     */
    private static final String[][][] TABLE = {
        { { "zh-Hans" }, { "msyh.ttc", "MicrosoftYaHei" }, { "Deng.ttf", "DengXian-Regular", "DengXian" },
          { "simsun.ttc", "SimSun" } },
        { { "zh-Hant" }, { "msjh.ttc", "MicrosoftJhengHeiRegular", "MicrosoftJhengHei" },
          { "mingliu.ttc", "MingLiU" }, { "msyh.ttc", "MicrosoftYaHei" } },
        { { "ja" }, { "YuGothR.ttc", "YuGothic-Regular" }, { "meiryo.ttc", "Meiryo" },
          { "msgothic.ttc", "MS-Gothic" } },
        { { "ko" }, { "malgun.ttf", "MalgunGothicRegular", "MalgunGothic" }, { "gulim.ttc", "Gulim" } },
    };

    /** What setUp made: the fonts.xml to read, and the Windows files by the PostScript name its
     *  elements carry (SystemFonts' updatableFontMap). */
    static final class Result {
        final File fontsXml;
        final Map<String, File> files;
        final String summary;
        Result(File fontsXml, Map<String, File> files, String summary) {
            this.fontsXml = fontsXml; this.files = files; this.summary = summary;
        }
    }

    /** The Windows fonts folder, or null off Windows. */
    static File windowsFontsDir() {
        for (String var : new String[] { "SystemRoot", "WINDIR" }) {
            String root = System.getenv(var);
            if (root == null || root.isEmpty()) continue;
            File d = new File(root, "Fonts");
            if (d.isDirectory()) return d;
        }
        return null;
    }

    /**
     * The fonts.xml with its CJK families on Windows' fonts, written under {@code cacheDir}; null when
     * {@code fontsDir} still has Noto CJK or its fonts.xml names none (nothing to do). Never throws
     * for a missing Windows font: that family is left out.
     */
    static Result setUp(File fontsDir, File windowsFonts, File cacheDir) throws IOException {
        if (new File(fontsDir, NOTO_CJK).isFile()) return null;
        String xml = new String(Files.readAllBytes(new File(fontsDir, "fonts.xml").toPath()),
                StandardCharsets.UTF_8);
        if (!xml.contains(NOTO_CJK)) return null;
        Map<String, File> files = new LinkedHashMap<>();
        StringBuilder summary = new StringBuilder();
        String out = rewrite(xml, lang -> windowsFonts == null ? null : pick(lang, windowsFonts), files, summary);
        if (!cacheDir.isDirectory() && !cacheDir.mkdirs() && !cacheDir.isDirectory()) {
            throw new IOException("cannot make " + cacheDir);
        }
        byte[] bytes = out.getBytes(StandardCharsets.UTF_8);
        File target = new File(cacheDir, String.format(Locale.ROOT, "fonts-%08x.xml", java.util.Arrays.hashCode(bytes)));
        if (!target.isFile() || target.length() != bytes.length) {
            // Another app starting at the same moment writes the same bytes: a private file moved into
            // place, so neither reads half of the other's.
            File tmp = File.createTempFile("fonts", ".xml.part", cacheDir);
            try {
                Files.write(tmp.toPath(), bytes);
                Files.move(tmp.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING,
                        StandardCopyOption.ATOMIC_MOVE);
            } finally {
                tmp.delete();
            }
        }
        return new Result(target, files, summary.toString());
    }

    /** A Windows face for a family: {file, index, PostScript name}, or null. */
    interface Picker { Object[] pick(String lang); }

    private static final Pattern FAMILY = Pattern.compile("(<family\\s+lang=\"([^\"]*)\"[^>]*>)(.*?)(</family>)",
            Pattern.DOTALL);
    private static final Pattern NOTO_FONT = Pattern.compile(
            "([ \\t]*)<font\\s([^>]*)>\\s*" + Pattern.quote(NOTO_CJK) + "\\s*</font>[ \\t]*\\r?\\n?");

    /** fonts.xml with each family's Noto CJK element on the picked face (or gone when none). */
    static String rewrite(String xml, Picker picker, Map<String, File> files, StringBuilder summary) {
        Matcher fam = FAMILY.matcher(xml);
        StringBuilder out = new StringBuilder(xml.length());
        while (fam.find()) {
            String body = fam.group(3);
            if (!body.contains(NOTO_CJK)) continue;
            String lang = fam.group(2);
            Object[] face = picker.pick(lang.split(",")[0].trim());
            Matcher font = NOTO_FONT.matcher(body);
            StringBuilder nb = new StringBuilder();
            while (font.find()) {
                String repl = "";
                if (face != null) {
                    File file = (File) face[0];
                    String ps = (String) face[2];
                    // The index is the Windows file's; the PostScript name keys the file in the
                    // updatableFontMap (FontListParser.readFont), so the Java side opens it there.
                    String attrs = font.group(2).replaceAll("\\s*(index|postScriptName)=\"[^\"]*\"", "").trim();
                    repl = font.group(1) + "<font " + attrs + " index=\"" + face[1] + "\" postScriptName=\""
                            + ps + "\">" + file.getName() + "</font>\n";
                    files.put(ps, file);
                }
                font.appendReplacement(nb, Matcher.quoteReplacement(repl));
            }
            font.appendTail(nb);
            if (summary.length() > 0) summary.append(", ");
            summary.append(lang).append(": ")
                    .append(face == null ? "none" : ((File) face[0]).getName() + "#" + face[1] + " " + face[2]);
            fam.appendReplacement(out, Matcher.quoteReplacement(fam.group(1) + nb + fam.group(4)));
        }
        fam.appendTail(out);
        return out.toString();
    }

    /** The first of a family's candidates %WINDIR%\Fonts has with readable outlines: {file, index, name}. */
    static Object[] pick(String lang, File windowsFonts) {
        for (String[][] row : TABLE) {
            if (!row[0][0].equalsIgnoreCase(lang)) continue;
            for (int c = 1; c < row.length; c++) {
                File f = new File(windowsFonts, row[c][0]);
                if (!f.isFile()) continue;
                List<String> names;
                try { names = faceNames(f); } catch (IOException | RuntimeException e) { continue; }
                for (int n = 1; n < row[c].length; n++) {
                    int i = names.indexOf(row[c][n]);
                    if (i >= 0) return new Object[] { f, i, row[c][n] };
                }
                if (!names.isEmpty() && names.get(0) != null) return new Object[] { f, 0, names.get(0) };
            }
        }
        return null;
    }

    /**
     * Each face's PostScript name (nameID 6) in a font file or collection, null for a face without
     * 'glyf', 'CFF ' or 'CFF2' outlines (which the native runtime's FreeType could not draw). OpenType
     * spec: "Font Collections" for the 'ttcf' header, "Table Directory", "name".
     */
    static List<String> faceNames(File file) throws IOException {
        List<String> out = new ArrayList<>();
        try (RandomAccessFile f = new RandomAccessFile(file, "r")) {
            if (f.readInt() == 0x74746366) {          // 'ttcf'
                f.readInt();                           // version
                int n = f.readInt();
                if (n <= 0 || n > 256) return out;
                long[] offs = new long[n];
                for (int i = 0; i < n; i++) offs[i] = f.readInt() & 0xffffffffL;
                for (long off : offs) out.add(postScriptName(f, off));
            } else {
                out.add(postScriptName(f, 0));
            }
        }
        return out;
    }

    private static String postScriptName(RandomAccessFile f, long off) throws IOException {
        f.seek(off + 4);
        int numTables = f.readUnsignedShort();
        if (numTables <= 0 || numTables > 512) return null;
        long nameOff = -1;
        boolean outlines = false;
        for (int t = 0; t < numTables; t++) {
            f.seek(off + 12 + 16L * t);
            int tag = f.readInt();
            f.readInt();                               // checksum
            long o = f.readInt() & 0xffffffffL;
            if (tag == 0x6e616d65) nameOff = o;        // 'name'
            if (tag == 0x676c7966 || tag == 0x43464620 || tag == 0x43464632) outlines = true;
        }
        if (nameOff < 0 || !outlines) return null;
        f.seek(nameOff);
        f.readUnsignedShort();                         // format
        int count = f.readUnsignedShort();
        int strOff = f.readUnsignedShort();
        String mac = null;
        for (int r = 0; r < count; r++) {
            f.seek(nameOff + 6 + 12L * r);
            int platform = f.readUnsignedShort();
            int encoding = f.readUnsignedShort();
            f.readUnsignedShort();                     // language
            int nameId = f.readUnsignedShort();
            int len = f.readUnsignedShort();
            int so = f.readUnsignedShort();
            if (nameId != 6 || len == 0 || len > 512) continue;
            byte[] raw = new byte[len];
            f.seek(nameOff + strOff + so);
            f.readFully(raw);
            if ((platform == 3 && (encoding == 1 || encoding == 10)) || platform == 0) {
                return new String(raw, StandardCharsets.UTF_16BE);
            }
            if (platform == 1 && encoding == 0 && mac == null) mac = new String(raw, StandardCharsets.ISO_8859_1);
        }
        return mac;
    }
}
