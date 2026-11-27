// HostPrinting -- Windows' printers (windows/host/native_printing.cpp), for android.print's spooler
// (android.print.PrintSpooler): the default printer's settings, Windows' Print dialog over the app's window,
// and a PDF printed on the printer the dialog chose. The lookup, the dialog and the printing run on the
// host's own threads (opening a printer can take seconds); their answers come back through onDefaults,
// onChosen and onPrintState, on those threads.
package com.macapk.loader;

import java.util.HashMap;
import java.util.Map;

public final class HostPrinting {
    private HostPrinting() {}

    /** The length of the numbers a printer's settings are given in (native_printing.cpp, kNumbers). */
    public static final int NUMBERS = 13;

    /** onChosen's results. */
    public static final int CHOSEN = 0, CANCELLED = 1, FAILED = 2;

    /** onPrintState's states. */
    public static final int STARTED = 1, COMPLETED = 2, PRINT_FAILED = 3, PRINT_CANCELLED = 4;

    public interface DefaultsListener {
        /** found: names {printer, paper} and its NUMBERS settings; else there is no default printer. */
        void onDefaults(boolean found, String[] names, int[] numbers);
    }

    public interface ChooseListener {
        /** result CHOSEN: setup is the choice's handle, names {printer, paper}, numbers its settings
         *  (NUMBERS of them), ranges the pages chosen as 1-based inclusive pairs (empty: all). FAILED:
         *  names[0] says why. CANCELLED: nothing else. */
        void onChosen(int result, long setup, String[] names, int[] numbers, int[] ranges);
    }

    public interface PrintListener {
        void onPrintState(int state, String status);
    }

    private static final Map<Integer, DefaultsListener> sDefaults = new HashMap<>();
    private static final Map<Integer, ChooseListener> sChoosers = new HashMap<>();
    private static final Map<Integer, PrintListener> sPrints = new HashMap<>();
    private static int sNextId = 1;

    // hints: the app's PrintAttributes as Windows' settings, applied where the printer has them: the paper
    // (DMPAPER_*), orientation (1 portrait, 2 landscape), colour (1 monochrome, 2 colour), duplex (1
    // one-sided, 2 long edge, 3 short edge); 0 for what the app did not say.

    /** Looks the default printer up with the hints applied, on a host thread; false when it could not be
     *  asked (no host), in which case the listener is never called. */
    public static boolean defaults(int[] hints, DefaultsListener listener) {
        int id;
        synchronized (sChoosers) {
            id = sNextId++;
        }
        synchronized (sDefaults) {
            sDefaults.put(id, listener);
        }
        boolean started;
        try {
            started = nativeDefaults(hints, id);
        } catch (UnsatisfiedLinkError e) {
            started = false;
        }
        if (!started) {
            synchronized (sDefaults) {
                sDefaults.remove(id);
            }
        }
        return started;
    }

    /** Opens Windows' Print dialog on the default printer with the hints applied, for a document of
     *  pageCount pages (-1: not known); false when it could not be opened (no host), in which case the
     *  listener is never called. */
    public static boolean choose(int pageCount, int[] hints, ChooseListener listener) {
        int id;
        synchronized (sChoosers) {
            id = sNextId++;
            sChoosers.put(id, listener);
        }
        boolean started;
        try {
            started = nativeChoose(pageCount, hints, id);
        } catch (UnsatisfiedLinkError e) {
            started = false;
        }
        if (!started) {
            synchronized (sChoosers) {
                sChoosers.remove(id);
            }
        }
        return started;
    }

    /** Prints the PDF at path on a setup onChosen gave: pages are 0-based indices into the file (empty:
     *  all of them). Returns the print's id for cancel, 0 when it could not start. */
    public static int print(long setup, String path, String jobName, int[] pages, PrintListener listener) {
        int id;
        synchronized (sChoosers) {
            id = sNextId++;
        }
        synchronized (sPrints) {
            sPrints.put(id, listener);
        }
        boolean started;
        try {
            started = nativePrint(setup, path, jobName, pages, id);
        } catch (UnsatisfiedLinkError e) {
            started = false;
        }
        if (!started) {
            synchronized (sPrints) {
                sPrints.remove(id);
            }
            return 0;
        }
        return id;
    }

    public static void cancel(int id) {
        try {
            nativeCancel(id);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    /** Frees a setup onChosen gave. */
    public static void release(long setup) {
        try {
            nativeRelease(setup);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    // ---- from the host's threads ----

    static void onDefaults(int id, boolean found, String[] names, int[] numbers) {
        DefaultsListener l;
        synchronized (sDefaults) {
            l = sDefaults.remove(id);
        }
        if (l != null) l.onDefaults(found, names, numbers);
    }

    static void onChosen(int id, int result, long setup, String[] names, int[] numbers, int[] ranges) {
        ChooseListener l;
        synchronized (sChoosers) {
            l = sChoosers.remove(id);
        }
        if (l != null) l.onChosen(result, setup, names, numbers, ranges);
        else if (setup != 0) release(setup);
    }

    static void onPrintState(int id, int state, String status) {
        PrintListener l;
        synchronized (sPrints) {
            l = state == STARTED ? sPrints.get(id) : sPrints.remove(id);
        }
        if (l != null) l.onPrintState(state, status);
    }

    private static native boolean nativeDefaults(int[] hints, int id);
    private static native boolean nativeChoose(int pageCount, int[] hints, int id);
    private static native boolean nativePrint(long setup, String path, String jobName, int[] pages, int id);
    private static native void nativeCancel(int id);
    private static native void nativeRelease(long setup);
}
