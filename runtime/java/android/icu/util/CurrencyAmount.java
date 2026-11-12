package android.icu.util;

/**
 * An amount of money: a number with a Currency as its unit. What
 * MeasureFormat.getCurrencyFormat() formats.
 *
 * Source: https://developer.android.com/reference/android/icu/util/CurrencyAmount
 */
public class CurrencyAmount extends Measure {

    public CurrencyAmount(Number number, Currency currency) {
        super(number, currency);
    }

    public CurrencyAmount(double number, Currency currency) {
        super(Double.valueOf(number), currency);
    }

    public CurrencyAmount(Number number, java.util.Currency currency) {
        this(number, Currency.wrap(currency));
    }

    public CurrencyAmount(double number, java.util.Currency currency) {
        this(number, Currency.wrap(currency));
    }

    public Currency getCurrency() {
        return (Currency) getUnit();
    }
}
