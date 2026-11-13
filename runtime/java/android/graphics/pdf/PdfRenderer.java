// android.graphics.pdf.PdfRenderer (API 21): draws the pages of a PDF into bitmaps.
// AOSP: frameworks/base/graphics/java/android/graphics/pdf/PdfRenderer.java, on PDFium.
//
// Win.apk: there is no PDF engine in the runtime yet, so a document cannot be opened, and the
// constructor says so the way AOSP reports a file it cannot read: an IOException, which every
// caller handles (a viewer shows "can't open this file", a scanner skips the preview). The class
// was missing altogether, and 27 of the 89 most installed apps name it: reaching it was a
// NoClassDefFoundError, which nothing catches. The methods below are AOSP's, for a renderer that
// could not exist; they are here so that code naming them links.
package android.graphics.pdf;

import android.graphics.Bitmap;
import android.graphics.Matrix;
import android.graphics.Rect;
import android.os.ParcelFileDescriptor;

import java.io.IOException;

public final class PdfRenderer implements AutoCloseable {
    public PdfRenderer(ParcelFileDescriptor fileDescriptor) throws IOException {
        if (fileDescriptor == null) throw new NullPointerException("input cannot be null");
        throw new IOException("PDF documents cannot be rendered here yet (android.graphics.pdf.PdfRenderer)");
    }

    @Override public void close() { throw new IllegalStateException("Already closed"); }
    public int getPageCount() { throw new IllegalStateException("Already closed"); }
    public boolean shouldScaleForPrinting() { throw new IllegalStateException("Already closed"); }
    public Page openPage(int index) { throw new IllegalStateException("Already closed"); }

    public final class Page implements AutoCloseable {
        public static final int RENDER_MODE_FOR_DISPLAY = 1;
        public static final int RENDER_MODE_FOR_PRINT = 2;

        private Page() {}
        public int getIndex() { throw new IllegalStateException("Already closed"); }
        public int getWidth() { throw new IllegalStateException("Already closed"); }
        public int getHeight() { throw new IllegalStateException("Already closed"); }
        public void render(Bitmap destination, Rect destClip, Matrix transform, int renderMode) {
            throw new IllegalStateException("Already closed");
        }
        @Override public void close() { throw new IllegalStateException("Already closed"); }
    }
}
