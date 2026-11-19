package android.icu.text;

// What AOSP's android.text.AndroidBidi asks of ICU's Bidi (a paragraph's embedding levels), on
// java.text.Bidi: the framework has no android.icu.text. Only the four members AndroidBidi
// calls; LEVEL_DEFAULT_LTR / LEVEL_DEFAULT_RTL are constants javac put into AndroidBidi itself.
public class Bidi {
    public static final byte LEVEL_DEFAULT_LTR = (byte) 0x7e;
    public static final byte LEVEL_DEFAULT_RTL = (byte) 0x7f;

    private java.text.Bidi bidi;

    public Bidi(int maxLength, int maxRunCount) {}

    public void setPara(char[] chars, byte paraLevel, byte[] embeddingLevels) {
        int flags = paraLevel == LEVEL_DEFAULT_LTR ? java.text.Bidi.DIRECTION_DEFAULT_LEFT_TO_RIGHT
                : paraLevel == LEVEL_DEFAULT_RTL ? java.text.Bidi.DIRECTION_DEFAULT_RIGHT_TO_LEFT
                : (paraLevel & 1) == 0 ? java.text.Bidi.DIRECTION_LEFT_TO_RIGHT
                : java.text.Bidi.DIRECTION_RIGHT_TO_LEFT;
        bidi = new java.text.Bidi(chars, 0, embeddingLevels, 0, chars.length, flags);
    }

    public byte getLevelAt(int charIndex) { return (byte) bidi.getLevelAt(charIndex); }

    public byte getParaLevel() { return (byte) bidi.getBaseLevel(); }
}
