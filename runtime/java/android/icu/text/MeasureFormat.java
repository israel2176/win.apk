// android.icu.text.MeasureFormat -- formats a Measure (number + unit) with the
// locale's CLDR unit patterns. Published API:
//   developer.android.com/reference/android/icu/text/MeasureFormat
//   public class MeasureFormat extends UFormat
//
// Hermes' Intl.NumberFormat {style: "unit"} builds
//   MeasureFormat.getInstance(locale, unitDisplay.getFormatWidth(), numberFormat)
// and formats `new Measure(n, unit)`. ICU4J renders that with a
// LocalizedNumberFormatter carrying the NumberFormat's settings plus the unit
// and width. The JDK has no unit data at all, so this routes to the host's own
// ICU (macOS libicucore, ICU 78 on macOS 26) through com.macapk.icu.HostIcu,
// expressing the same request as an ICU number skeleton:
//   measure-unit/<type>-<subtype>   the unit (ICU's own type names)
//   unit-width-full-name|short|narrow   WIDE / SHORT / NARROW
//   .00## / @@## / precision-integer    the NumberFormat's fraction or
//                                       significant-digit window
//   integer-width/*000, group-off, rounding-mode-*, sign-always
// so the number, the plural form of the unit and the pattern all come from ICU.
// Without the host bridge (a bare JVM) format() throws
// UnsupportedOperationException rather than inventing a string.
package android.icu.text;

import android.icu.util.Measure;
import android.icu.util.MeasureUnit;
import android.icu.util.ULocale;

import java.text.FieldPosition;
import java.text.ParsePosition;
import java.util.Locale;

public class MeasureFormat extends UFormat {
    private static final long serialVersionUID = 1L;

    /** ICU MeasureFormat.FormatWidth. */
    public enum FormatWidth {
        WIDE,
        SHORT,
        NARROW,
        NUMERIC
    }

    private final ULocale locale;
    private final FormatWidth width;
    private final NumberFormat numberFormat;

    private MeasureFormat(ULocale locale, FormatWidth width, NumberFormat numberFormat) {
        this.locale = (locale != null) ? locale : ULocale.getDefault(ULocale.Category.FORMAT);
        this.width = (width != null) ? width : FormatWidth.SHORT;
        this.numberFormat = (numberFormat != null) ? numberFormat : NumberFormat.getInstance(this.locale);
    }

    public static MeasureFormat getInstance(ULocale locale, FormatWidth formatWidth) {
        return new MeasureFormat(locale, formatWidth, null);
    }

    public static MeasureFormat getInstance(Locale locale, FormatWidth formatWidth) {
        return getInstance(ULocale.forLocale(locale), formatWidth);
    }

    public static MeasureFormat getInstance(ULocale locale, FormatWidth formatWidth, NumberFormat format) {
        return new MeasureFormat(locale, formatWidth, format);
    }

    public static MeasureFormat getInstance(Locale locale, FormatWidth formatWidth, NumberFormat format) {
        return getInstance(ULocale.forLocale(locale), formatWidth, format);
    }

    /** ICU: a format for CurrencyAmount objects in the locale ("$12.50"). */
    public static MeasureFormat getCurrencyFormat(ULocale locale) {
        return new CurrencyFormat(locale);
    }

    public static MeasureFormat getCurrencyFormat(Locale locale) {
        return getCurrencyFormat(ULocale.forLocale(locale));
    }

    public static MeasureFormat getCurrencyFormat() {
        return getCurrencyFormat(ULocale.getDefault(ULocale.Category.FORMAT));
    }

    /** ICU's CurrencyFormat: money goes through the locale's currency number
     *  format, with the amount's own currency, not through a unit pattern. */
    private static final class CurrencyFormat extends MeasureFormat {
        private static final long serialVersionUID = 1L;

        CurrencyFormat(ULocale locale) {
            super(locale, FormatWidth.WIDE, null);
        }

        @Override
        public StringBuffer format(Object obj, StringBuffer toAppendTo, FieldPosition fpos) {
            if (!(obj instanceof android.icu.util.CurrencyAmount)) {
                throw new IllegalArgumentException("Invalid type: " + obj.getClass().getName());
            }
            android.icu.util.CurrencyAmount amount = (android.icu.util.CurrencyAmount) obj;
            NumberFormat money = NumberFormat.getCurrencyInstance(getLocale());
            money.setCurrency(amount.getCurrency());
            return toAppendTo.append(money.format(amount.getNumber()));
        }
    }

    public final FormatWidth getWidth() { return width; }
    public final ULocale getLocale() { return locale; }
    public NumberFormat getNumberFormat() { return (NumberFormat) numberFormat.clone(); }

    /** ICU: obj is a Measure, or a Measure[] / Collection of them (formatted as formatMeasures). */
    @Override
    public StringBuffer format(Object obj, StringBuffer toAppendTo, FieldPosition fpos) {
        if (obj instanceof Measure) {
            return toAppendTo.append(formatMeasure((Measure) obj));
        }
        if (obj instanceof Measure[]) {
            return toAppendTo.append(formatMeasures((Measure[]) obj));
        }
        if (obj instanceof java.util.Collection) {
            java.util.Collection<?> c = (java.util.Collection<?>) obj;
            Measure[] ms = new Measure[c.size()];
            int i = 0;
            for (Object o : c) {
                if (!(o instanceof Measure)) throw new IllegalArgumentException(obj.toString());
                ms[i++] = (Measure) o;
            }
            return toAppendTo.append(formatMeasures(ms));
        }
        throw new IllegalArgumentException(String.valueOf(obj));
    }

    /** ICU: several measures joined with the locale's unit list pattern ("5 h, 3 min"). */
    public final String formatMeasures(Measure... measures) {
        if (measures == null || measures.length == 0) return "";
        java.util.List<String> parts = new java.util.ArrayList<>();
        for (Measure m : measures) parts.add(formatMeasure(m));
        ListFormatter lf = ListFormatter.getInstance(ULocale.unwrap(locale));
        return lf.format(parts);
    }

    @Override
    public Object parseObject(String source, ParsePosition pos) {
        throw new UnsupportedOperationException();   // as ICU's MeasureFormat
    }

    private String formatMeasure(Measure m) {
        return com.macapk.icu.HostIcu.formatSkeleton(skeleton(m.getUnit()), locale.getName(), m.getNumber());
    }

    /** The ICU number skeleton for this format's NumberFormat settings + unit + width. */
    String skeleton(MeasureUnit unit) {
        StringBuilder sk = new StringBuilder();
        sk.append("measure-unit/").append(unit.getType()).append('-').append(unit.getSubtype());
        switch (width) {
            case WIDE: sk.append(" unit-width-full-name"); break;
            case NARROW: sk.append(" unit-width-narrow"); break;
            default: sk.append(" unit-width-short"); break;
        }
        NumberFormat nf = numberFormat;
        DecimalFormat df = (nf instanceof DecimalFormat) ? (DecimalFormat) nf : null;
        if (df != null && df.areSignificantDigitsUsed()) {
            int min = Math.max(1, df.getMinimumSignificantDigits());
            int max = Math.max(min, df.getMaximumSignificantDigits());
            sk.append(' ');
            for (int i = 0; i < min; i++) sk.append('@');
            for (int i = min; i < max; i++) sk.append('#');
        } else {
            int minF = Math.max(0, nf.getMinimumFractionDigits());
            int maxF = Math.max(minF, Math.min(nf.getMaximumFractionDigits(), 100));
            if (maxF == 0) {
                sk.append(" precision-integer");
            } else {
                sk.append(" .");
                for (int i = 0; i < minF; i++) sk.append('0');
                for (int i = minF; i < maxF; i++) sk.append('#');
            }
        }
        int minI = nf.getMinimumIntegerDigits();
        if (minI > 1) {
            sk.append(" integer-width/*");
            for (int i = 0; i < Math.min(minI, 100); i++) sk.append('0');
        }
        if (!nf.isGroupingUsed()) sk.append(" group-off");
        switch (nf.getRoundingMode()) {
            case 0: sk.append(" rounding-mode-up"); break;
            case 1: sk.append(" rounding-mode-down"); break;
            case 2: sk.append(" rounding-mode-ceiling"); break;
            case 3: sk.append(" rounding-mode-floor"); break;
            case 4: sk.append(" rounding-mode-half-up"); break;
            case 5: sk.append(" rounding-mode-half-down"); break;
            case 7: sk.append(" rounding-mode-unnecessary"); break;
            default: break;   // 6 HALF_EVEN: ICU's default
        }
        if (df != null && df.isSignAlwaysShown()) sk.append(" sign-always");
        return sk.toString();
    }
}
