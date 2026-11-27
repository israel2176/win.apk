// android.print.PrintSpooler -- what Android's print spooler and print UI do for a print job, in the app's
// process, with Windows' printers as the print service (com.macapk.loader.HostPrinting, windows/host/
// native_printing.cpp).
//
// On a device PrintManager.print() hands the app's PrintDocumentAdapter to the spooler, whose print
// activity opens over the app: it calls onStart, lays the document out (onLayout, EXTRA_PRINT_PREVIEW true)
// for the printer it selects first, lets the user choose a printer, paper, copies and pages, lays it out
// again for that choice (preview false), asks onWrite for the chosen pages into a file, queues the job
// for the print service and closes, calling onFinish. The print service then starts the job and completes
// it (or fails it, saying why). Here every step is the same, and:
//   - the printer selected first is Windows' default printer, with the app's PrintAttributes applied as
//     Android's print UI applies them (the paper, its orientation, colour and two-sided when the printer
//     has them);
//   - the print UI is Windows' own Print dialog, over the app's window;
//   - the print service is the Windows printer chosen: the PDF the adapter wrote is rendered page by page
//     into its device context with the user's settings.
// What Windows' dialog cannot show is the document's preview: the first layout is made for it on a device
// and is made here all the same, so an adapter sees the same calls. Cancelling the dialog cancels the
// job, as backing out of the print UI does. A layout or write the adapter fails fails the job with the
// adapter's message as its status, where a device would show the message in its print UI. When the
// activity that printed is destroyed before the print UI is done, the job is cancelled and the adapter
// dropped, never called again (not onFinish either), as AOSP's PrintDocumentAdapterDelegate drops it; a
// layout or write in progress then has its CancellationSignal cancelled, as the spooler cancels it.
//
// Every call into the adapter and every change to a job's state happen on the app's main thread, as the
// spooler's arrive there; the adapter may answer its callbacks from any thread. Windows' printer calls
// (the default printer's lookup, the dialog, printing) run on the host's threads, never the main one.
// Jobs are kept for the process's life, as PrintManagerService keeps an app's print jobs for as long as
// the app runs.
package android.print;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import com.macapk.loader.HostPrinting;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

final class PrintSpooler {

    private static final String LOG_TAG = "PrintSpooler";

    /** The printers and the print dialog: Windows', or a test's stand-in (PrintManagerTest). */
    interface Host {
        boolean defaults(int[] hints, HostPrinting.DefaultsListener listener);
        boolean choose(int pageCount, int[] hints, HostPrinting.ChooseListener listener);
        int print(long setup, String path, String jobName, int[] pages, HostPrinting.PrintListener listener);
        void cancel(int printId);
        void release(long setup);
    }

    static Host sHost = new Host() {
        @Override public boolean defaults(int[] hints, HostPrinting.DefaultsListener listener) {
            return HostPrinting.defaults(hints, listener);
        }
        @Override public boolean choose(int pageCount, int[] hints, HostPrinting.ChooseListener listener) {
            return HostPrinting.choose(pageCount, hints, listener);
        }
        @Override public int print(long setup, String path, String jobName, int[] pages,
                HostPrinting.PrintListener listener) {
            return HostPrinting.print(setup, path, jobName, pages, listener);
        }
        @Override public void cancel(int printId) { HostPrinting.cancel(printId); }
        @Override public void release(long setup) { HostPrinting.release(setup); }
    };

    /** Where the documents the adapters write are kept until printed (a test's own folder). */
    static File sSpoolDir;

    private static final Map<PrintJobId, Job> sJobs = new LinkedHashMap<>();
    private static Handler sMain;

    private PrintSpooler() {}

    static PrintJobInfo create(String label, PrintDocumentAdapter adapter, PrintAttributes attributes,
            Context context) {
        PrintJobInfo info = new PrintJobInfo();
        info.setId(new PrintJobId());
        info.setLabel(label);
        info.setState(PrintJobInfo.STATE_CREATED);
        info.setCopies(1);
        info.setCreationTime(System.currentTimeMillis());
        info.setPages(PageRange.ALL_PAGES_ARRAY);
        info.setAttributes(attributes != null ? attributes : new PrintAttributes.Builder().build());
        Job job = new Job(info, adapter, attributes, context);
        synchronized (sJobs) {
            sJobs.put(info.getId(), job);
        }
        // The print UI starts after print() returned, as the spooler's activity does.
        main().post(job::start);
        return new PrintJobInfo(info);
    }

    /** A copy of the job's info, as the spooler answers over Binder; null for a job it does not have. */
    static PrintJobInfo info(PrintJobId id) {
        Job job = job(id);
        if (job == null) return null;
        synchronized (job) {
            return new PrintJobInfo(job.info);
        }
    }

    static List<PrintJobInfo> infos() {
        List<PrintJobInfo> out = new ArrayList<>();
        synchronized (sJobs) {
            for (Job job : sJobs.values()) {
                synchronized (job) {
                    out.add(new PrintJobInfo(job.info));
                }
            }
        }
        return out;
    }

    static void cancel(PrintJobId id) {
        Job job = job(id);
        if (job != null) main().post(job::cancel);
    }

    static void restart(PrintJobId id) {
        Job job = job(id);
        if (job != null) main().post(job::restart);
    }

    private static Job job(PrintJobId id) {
        if (id == null) return null;
        synchronized (sJobs) {
            return sJobs.get(id);
        }
    }

    private static synchronized Handler main() {
        if (sMain == null) sMain = new Handler(Looper.getMainLooper());
        return sMain;
    }

    // ---- Windows' settings in Android's terms -------------------------------------------------------------

    // HostPrinting's numbers (native_printing.cpp, describe): paper width, height, left, top, right, bottom
    // margins in mils, as oriented; dpi x, y; DEVMODE's colour (0 not said, 1 monochrome, 2 colour); a colour
    // device; DEVMODE's duplex (0 not said, 1 one-sided, 2 long edge, 3 short edge); copies; collate.
    static final int N_WIDTH = 0, N_HEIGHT = 1, N_LEFT = 2, N_TOP = 3, N_RIGHT = 4, N_BOTTOM = 5, N_DPI_X = 6,
            N_DPI_Y = 7, N_COLOR = 8, N_COLOR_DEVICE = 9, N_DUPLEX = 10, N_COPIES = 11, N_COLLATE = 12;

    /** The attributes a printer's settings mean: Android's own media size for the paper when it is one,
     *  else one named after Windows' paper; landscape when the paper is turned. */
    static PrintAttributes attributesOf(String paperName, int[] n) {
        PrintAttributes.MediaSize media = mediaSizeOf(paperName, n[N_WIDTH], n[N_HEIGHT]);
        int colorMode = n[N_COLOR] == 1 || n[N_COLOR_DEVICE] == 0
                ? PrintAttributes.COLOR_MODE_MONOCHROME : PrintAttributes.COLOR_MODE_COLOR;
        int duplex = n[N_DUPLEX] == 2 ? PrintAttributes.DUPLEX_MODE_LONG_EDGE
                : n[N_DUPLEX] == 3 ? PrintAttributes.DUPLEX_MODE_SHORT_EDGE : PrintAttributes.DUPLEX_MODE_NONE;
        String dpi = n[N_DPI_X] == n[N_DPI_Y] ? n[N_DPI_X] + " dpi" : n[N_DPI_X] + " x " + n[N_DPI_Y] + " dpi";
        return new PrintAttributes.Builder()
                .setMediaSize(media)
                .setResolution(new PrintAttributes.Resolution(n[N_DPI_X] + "x" + n[N_DPI_Y], dpi,
                        Math.max(1, n[N_DPI_X]), Math.max(1, n[N_DPI_Y])))
                .setMinMargins(new PrintAttributes.Margins(n[N_LEFT], n[N_TOP], n[N_RIGHT], n[N_BOTTOM]))
                .setColorMode(colorMode)
                .setDuplexMode(duplex)
                .build();
    }

    /** How far, in mils per side, a paper may be from one of Android's sizes and still be it (a driver's
     *  sizes are rounded to its own units). */
    private static final int MEDIA_TOLERANCE_MILS = 40;

    static PrintAttributes.MediaSize mediaSizeOf(String paperName, int widthMils, int heightMils) {
        int shortSide = Math.min(widthMils, heightMils), longSide = Math.max(widthMils, heightMils);
        PrintAttributes.MediaSize best = null;
        int bestDiff = Integer.MAX_VALUE;
        List<PrintAttributes.MediaSize> sizes = new ArrayList<>(PrintAttributes.MediaSize.getAllPredefinedSizes());
        // Ties (a size Android defines twice, portrait and landscape, as Tabloid and Ledger) go to the
        // portrait one, ISO and North American names first, then by id: the same answer every run.
        sizes.sort(Comparator.comparingInt((PrintAttributes.MediaSize m) -> m.isPortrait() ? 0 : 1)
                .thenComparingInt(m -> m.getId().startsWith("ISO_") ? 0 : m.getId().startsWith("NA_") ? 1 : 2)
                .thenComparing(PrintAttributes.MediaSize::getId));
        for (PrintAttributes.MediaSize m : sizes) {
            int s = Math.min(m.getWidthMils(), m.getHeightMils()), l = Math.max(m.getWidthMils(), m.getHeightMils());
            int ds = Math.abs(s - shortSide), dl = Math.abs(l - longSide);
            if (ds > MEDIA_TOLERANCE_MILS || dl > MEDIA_TOLERANCE_MILS) continue;
            if (ds + dl < bestDiff) {
                best = m;
                bestDiff = ds + dl;
            }
        }
        if (best == null) {
            String label = paperName != null && !paperName.isEmpty() ? paperName
                    : String.format(Locale.ROOT, "%.2f x %.2f in", shortSide / 1000f, longSide / 1000f);
            best = new PrintAttributes.MediaSize("WINDOWS_" + shortSide + "X" + longSide, label,
                    Math.max(1, shortSide), Math.max(1, longSide));
        } else if (!best.isPortrait()) {
            best = best.asPortrait();
        }
        return widthMils > heightMils ? best.asLandscape() : best;
    }

    // The app's attributes as hints for the printer selected first and the dialog (native_printing.cpp,
    // apply_hints): Windows' paper code (DMPAPER_*) for the media size, the orientation (1 portrait, 2
    // landscape), colour (1 monochrome, 2 colour) and duplex (1 one-sided, 2 long edge, 3 short edge); 0 for
    // what the app did not say.
    private static final Map<String, Integer> PAPER_CODES = new LinkedHashMap<>();
    static {
        PAPER_CODES.put("NA_LETTER", 1);
        PAPER_CODES.put("NA_TABLOID", 3);
        PAPER_CODES.put("NA_LEDGER", 4);
        PAPER_CODES.put("NA_LEGAL", 5);
        PAPER_CODES.put("ISO_A3", 8);
        PAPER_CODES.put("ISO_A4", 9);
        PAPER_CODES.put("ISO_A5", 11);
        PAPER_CODES.put("JIS_B4", 12);
        PAPER_CODES.put("JIS_B5", 13);
        PAPER_CODES.put("NA_QUARTO", 15);
        PAPER_CODES.put("ISO_A6", 70);
    }

    static int[] hintsOf(PrintAttributes a) {
        int[] hints = new int[4];
        if (a == null) return hints;
        PrintAttributes.MediaSize m = a.getMediaSize();
        if (m != null) {
            String id = m.getId();
            boolean unknown = "UNKNOWN_PORTRAIT".equals(id) || "UNKNOWN_LANDSCAPE".equals(id);
            if (!unknown) {
                // A turned size keeps its id (MediaSize.asLandscape).
                Integer code = PAPER_CODES.get(id);
                hints[0] = code != null ? code : 0;
            }
            hints[1] = m.isPortrait() ? 1 : 2;
        }
        hints[2] = a.getColorMode() == PrintAttributes.COLOR_MODE_MONOCHROME ? 1
                : a.getColorMode() == PrintAttributes.COLOR_MODE_COLOR ? 2 : 0;
        hints[3] = a.getDuplexMode() == PrintAttributes.DUPLEX_MODE_NONE ? 1
                : a.getDuplexMode() == PrintAttributes.DUPLEX_MODE_LONG_EDGE ? 2
                : a.getDuplexMode() == PrintAttributes.DUPLEX_MODE_SHORT_EDGE ? 3 : 0;
        return hints;
    }

    /** The dialog's page ranges (1-based, inclusive pairs; none: every page) as Android's, 0-based. */
    static PageRange[] pagesOf(int[] ranges, int pageCount) {
        if (ranges == null || ranges.length < 2) return PageRange.ALL_PAGES_ARRAY;
        List<PageRange> out = new ArrayList<>();
        for (int i = 0; i + 1 < ranges.length; i += 2) {
            int from = Math.max(1, Math.min(ranges[i], ranges[i + 1])) - 1;
            int to = Math.max(ranges[i], ranges[i + 1]) - 1;
            if (pageCount > 0) to = Math.min(to, pageCount - 1);
            if (to >= from) out.add(new PageRange(from, to));
        }
        PageRange[] pages = normalize(out.toArray(new PageRange[0]));
        if (pages.length == 0) return PageRange.ALL_PAGES_ARRAY;
        if (pageCount > 0 && pages.length == 1 && pages[0].getStart() == 0
                && pages[0].getEnd() == pageCount - 1) {
            return PageRange.ALL_PAGES_ARRAY;
        }
        return pages;
    }

    /** Sorted, overlapping and adjacent ranges merged (AOSP's PageRangeUtils.normalize). */
    static PageRange[] normalize(PageRange[] pages) {
        if (pages == null) return null;
        PageRange[] sorted = pages.clone();
        Arrays.sort(sorted, Comparator.comparingInt(PageRange::getStart));
        List<PageRange> out = new ArrayList<>();
        for (PageRange r : sorted) {
            if (r == null) continue;
            if (!out.isEmpty()) {
                PageRange last = out.get(out.size() - 1);
                if ((long) r.getStart() <= (long) last.getEnd() + 1) {
                    out.set(out.size() - 1, new PageRange(last.getStart(), Math.max(last.getEnd(), r.getEnd())));
                    continue;
                }
            }
            out.add(r);
        }
        return out.toArray(new PageRange[0]);
    }

    static boolean isAll(PageRange[] pages) {
        return pages != null && pages.length == 1 && pages[0].equals(PageRange.ALL_PAGES);
    }

    /**
     * Which pages of the file the adapter wrote are the ones the user asked for, as indices into the file:
     * empty for every page of it, null when the file does not have them all (the spooler then fails the
     * job; AOSP's PageRangeUtils.computeWhichPagesInFileToPrint). The adapter may write exactly the pages
     * asked for, the whole document, or any pages that include them.
     */
    static int[] filePages(PageRange[] requested, PageRange[] written, int pageCount) {
        PageRange[] want = normalize(requested);
        PageRange[] have = normalize(written);
        if (have == null || have.length == 0) return null;
        if (want == null || want.length == 0 || isAll(want)) {
            if (isAll(have)) return new int[0];
            // Every page was asked for: the file must hold the whole document.
            if (pageCount > 0 && have.length == 1 && have[0].getStart() == 0
                    && have[0].getEnd() >= pageCount - 1) {
                return new int[0];
            }
            return null;
        }
        if (Arrays.equals(want, have)) return new int[0];
        if (isAll(have)) return expand(want, pageCount, 0);
        // The file holds the written pages in order: a requested page's index is its place among them.
        List<Integer> out = new ArrayList<>();
        for (PageRange r : want) {
            for (int page = r.getStart(); page <= r.getEnd(); page++) {
                int index = indexIn(have, page);
                if (index < 0) return null;
                out.add(index);
                if (out.size() > 100000) return null;
            }
        }
        return toArray(out);
    }

    private static int[] expand(PageRange[] pages, int pageCount, int offset) {
        List<Integer> out = new ArrayList<>();
        for (PageRange r : pages) {
            int end = pageCount > 0 ? Math.min(r.getEnd(), pageCount - 1) : r.getEnd();
            for (int page = r.getStart(); page <= end && out.size() <= 100000; page++) out.add(page + offset);
        }
        return toArray(out);
    }

    private static int indexIn(PageRange[] have, int page) {
        int index = 0;
        for (PageRange r : have) {
            if (page >= r.getStart() && page <= r.getEnd()) return index + page - r.getStart();
            index += r.getSize();
        }
        return -1;
    }

    private static int[] toArray(List<Integer> list) {
        int[] a = new int[list.size()];
        for (int i = 0; i < a.length; i++) a[i] = list.get(i);
        return a;
    }

    /** The attributes when no printer is known (no default printer): the paper of the device's region. */
    static PrintAttributes regionAttributes() {
        String country = Locale.getDefault().getCountry();
        boolean letter = "US".equals(country) || "CA".equals(country) || "MX".equals(country)
                || "PH".equals(country) || "CL".equals(country);
        return new PrintAttributes.Builder()
                .setMediaSize(letter ? PrintAttributes.MediaSize.NA_LETTER : PrintAttributes.MediaSize.ISO_A4)
                .setResolution(new PrintAttributes.Resolution("300x300", "300 dpi", 300, 300))
                .setMinMargins(PrintAttributes.Margins.NO_MARGINS)
                .setColorMode(PrintAttributes.COLOR_MODE_COLOR)
                .setDuplexMode(PrintAttributes.DUPLEX_MODE_NONE)
                .build();
    }

    // ---- one job ------------------------------------------------------------------------------------------

    /** One print job through the spooler's steps. Its methods run on the main thread; `info` is read from
     *  any thread under the job's lock. */
    private static final class Job {
        final PrintJobInfo info;
        private final PrintDocumentAdapter mAdapter;
        private final PrintAttributes mHint;
        private final Context mContext;

        private int mSession;              // a start's number: a late callback of an earlier one is ignored
        private boolean mFinishCalled;
        private CancellationSignal mSignal;
        private boolean mAdapterBusy;      // an onLayout or onWrite not answered yet
        private boolean mDropped;          // the printing activity was destroyed: the adapter is never called
        private Application mApp;          // where mWatch is registered
        private Application.ActivityLifecycleCallbacks mWatch;
        private PrintAttributes mLaidOut;  // the attributes of the last layout
        private PrintDocumentInfo mDocInfo;
        private PageRange[] mRequested = PageRange.ALL_PAGES_ARRAY;
        private long mSetup;               // the dialog's choice (HostPrinting), 0 none
        private File mFile;                // the document the adapter wrote
        private int[] mFilePages;
        private int mPrintId;              // the print running, 0 none

        Job(PrintJobInfo info, PrintDocumentAdapter adapter, PrintAttributes hint, Context context) {
            this.info = info;
            mAdapter = adapter;
            mHint = hint;
            mContext = context;
            // Watched from print() on, as AOSP's delegate is made there: the activity may go before the
            // print UI starts.
            watchActivity();
        }

        private void setState(int state, CharSequence status) {
            synchronized (this) {
                info.setState(state);
                info.setStatus(status);
                if (state != PrintJobInfo.STATE_QUEUED && state != PrintJobInfo.STATE_STARTED) {
                    info.setCancelling(false);
                }
            }
        }

        // -- the print UI's part --

        void start() {
            if (mDropped) return;
            final int session = ++mSession;
            mFinishCalled = false;
            mSignal = new CancellationSignal();
            mAdapterBusy = false;
            mLaidOut = null;
            mDocInfo = null;
            watchActivity();
            try {
                mAdapter.onStart();
            } catch (Throwable t) {
                fail("onStart threw: " + t, t);
                return;
            }
            // The printer selected first: Windows' default, with the app's paper, orientation, colour and
            // two-sided applied where it has them. Opening a printer can take seconds (a network one
            // offline, longer), so it is looked up on a host thread and the first layout waits for it.
            boolean asked = sHost.defaults(hintsOf(mHint), (found, names, numbers) -> onMain(session, () -> {
                PrintAttributes first;
                if (found && names != null && names.length >= 2 && numbers != null
                        && numbers.length >= HostPrinting.NUMBERS) {
                    first = attributesOf(names[1], numbers);
                    synchronized (this) {
                        info.setPrinterName(names[0]);
                    }
                } else {
                    first = regionAttributes();
                }
                layout(session, first, true, () -> choose(session));
            }));
            if (!asked) layout(session, regionAttributes(), true, () -> choose(session));
        }

        // -- the printing activity's life (AOSP's PrintDocumentAdapterDelegate) --

        private void watchActivity() {
            if (mWatch != null || !(mContext instanceof Activity)) return;
            final Activity activity = (Activity) mContext;
            Application app = activity.getApplication();
            if (app == null) return;
            mWatch = new Application.ActivityLifecycleCallbacks() {
                @Override public void onActivityCreated(Activity a, Bundle state) {}
                @Override public void onActivityStarted(Activity a) {}
                @Override public void onActivityResumed(Activity a) {}
                @Override public void onActivityPaused(Activity a) {}
                @Override public void onActivityStopped(Activity a) {}
                @Override public void onActivitySaveInstanceState(Activity a, Bundle state) {}
                @Override public void onActivityDestroyed(Activity a) {
                    if (a == activity) activityDestroyed();
                }
            };
            mApp = app;
            app.registerActivityLifecycleCallbacks(mWatch);
        }

        private void unwatchActivity() {
            if (mWatch == null) return;
            mApp.unregisterActivityLifecycleCallbacks(mWatch);
            mWatch = null;
            mApp = null;
        }

        /** The printing activity is gone before the print UI was done: the adapter is dropped and the job
         *  cancelled. A job already queued (the adapter's session over) prints on. */
        private void activityDestroyed() {
            unwatchActivity();
            mDropped = true;
            if (mFinishCalled) return;
            // Late answers of the adapter, the default printer and the dialog belong to the old session.
            ++mSession;
            mFinishCalled = true;
            if (mAdapterBusy) {
                // The print UI's end cancels the layout or write in progress, as the spooler's
                // RemotePrintDocument cancels its command; the adapter's answer is not waited for.
                mAdapterBusy = false;
                try {
                    mSignal.cancel();
                } catch (Throwable t) {
                    logw("the adapter's cancellation threw", t);
                }
            }
            discardFile();
            releaseSetup();
            setState(PrintJobInfo.STATE_CANCELED, null);
        }

        private void layout(int session, PrintAttributes attributes, boolean preview, Runnable then) {
            final PrintAttributes old = mLaidOut;
            Bundle extras = new Bundle();
            extras.putBoolean(PrintDocumentAdapter.EXTRA_PRINT_PREVIEW, preview);
            final boolean[] settled = {false};
            PrintDocumentAdapter.LayoutResultCallback cb = new PrintDocumentAdapter.LayoutResultCallback() {
                @Override public void onLayoutFinished(PrintDocumentInfo docInfo, boolean changed) {
                    if (docInfo == null) {
                        answer(() -> fail("onLayout finished without document info", null));
                        return;
                    }
                    answer(() -> {
                        mLaidOut = attributes;
                        mDocInfo = docInfo;
                        synchronized (Job.this) {
                            info.setAttributes(attributes);
                            info.setDocumentInfo(docInfo);
                        }
                        then.run();
                    });
                }
                @Override public void onLayoutFailed(CharSequence error) {
                    answer(() -> fail("onLayout failed: " + error, null));
                }
                @Override public void onLayoutCancelled() {
                    answer(Job.this::cancelled);
                }
                private void answer(Runnable r) {
                    synchronized (settled) {
                        if (settled[0]) return;
                        settled[0] = true;
                    }
                    onMain(session, () -> adapterAnswered(r));
                }
            };
            mAdapterBusy = true;
            try {
                mAdapter.onLayout(old, attributes, mSignal, cb, extras);
            } catch (Throwable t) {
                synchronized (settled) {
                    if (settled[0]) return;
                    settled[0] = true;
                }
                mAdapterBusy = false;
                fail("onLayout threw: " + t, t);
            }
        }

        /** The adapter answered its layout or write (on the main thread, in its session). */
        private void adapterAnswered(Runnable r) {
            mAdapterBusy = false;
            r.run();
        }

        private void choose(int session) {
            int pageCount = mDocInfo != null ? mDocInfo.getPageCount() : PrintDocumentInfo.PAGE_COUNT_UNKNOWN;
            boolean opened = sHost.choose(pageCount, hintsOf(mHint),
                    (result, setup, names, numbers, ranges) -> onMain(session, () -> {
                        if (result == HostPrinting.CHOSEN) {
                            chosen(session, setup, names, numbers, ranges);
                        } else if (result == HostPrinting.FAILED) {
                            fail(names != null && names.length > 0 ? names[0] : "the print dialog failed", null);
                        } else {
                            cancelled();
                        }
                    }, () -> {
                        if (setup != 0) sHost.release(setup);
                    }));
            if (!opened) fail("no print dialog: Windows' printing is not available", null);
        }

        private void chosen(int session, long setup, String[] names, int[] numbers, int[] ranges) {
            releaseSetup();
            mSetup = setup;
            int pageCount = mDocInfo != null ? mDocInfo.getPageCount() : PrintDocumentInfo.PAGE_COUNT_UNKNOWN;
            mRequested = pagesOf(ranges, pageCount);
            PrintAttributes chosen = attributesOf(names[1], numbers);
            synchronized (this) {
                info.setPrinterName(names[0]);
                info.setCopies(Math.max(1, numbers[N_COPIES]));
                info.setPages(mRequested);
            }
            // The layout the document is printed from is never a preview's, so it is made again.
            layout(session, chosen, false, () -> write(session));
        }

        private void write(int session) {
            final ParcelFileDescriptor fd;
            try {
                discardFile();
                File dir = sSpoolDir != null ? sSpoolDir : new File(System.getProperty("java.io.tmpdir"));
                if (!dir.isDirectory()) dir.mkdirs();
                mFile = File.createTempFile("print-", ".pdf", dir);
                mFile.deleteOnExit();
                fd = ParcelFileDescriptor.open(mFile, ParcelFileDescriptor.MODE_READ_WRITE
                        | ParcelFileDescriptor.MODE_TRUNCATE);
            } catch (Throwable t) {
                fail("the print document could not be made: " + t, t);
                return;
            }
            final boolean[] settled = {false};
            PrintDocumentAdapter.WriteResultCallback cb = new PrintDocumentAdapter.WriteResultCallback() {
                @Override public void onWriteFinished(PageRange[] pages) {
                    answer(() -> written(session, pages));
                }
                @Override public void onWriteFailed(CharSequence error) {
                    answer(() -> fail("onWrite failed: " + error, null));
                }
                @Override public void onWriteCancelled() {
                    answer(Job.this::cancelled);
                }
                private void answer(Runnable r) {
                    synchronized (settled) {
                        if (settled[0]) return;
                        settled[0] = true;
                    }
                    closeQuietly(fd);
                    onMain(session, () -> adapterAnswered(r));
                }
            };
            mAdapterBusy = true;
            try {
                mAdapter.onWrite(mRequested, fd, mSignal, cb);
            } catch (Throwable t) {
                synchronized (settled) {
                    if (settled[0]) return;
                    settled[0] = true;
                }
                mAdapterBusy = false;
                closeQuietly(fd);
                fail("onWrite threw: " + t, t);
            }
        }

        private void written(int session, PageRange[] pages) {
            int pageCount = mDocInfo != null ? mDocInfo.getPageCount() : PrintDocumentInfo.PAGE_COUNT_UNKNOWN;
            long size = mFile != null ? mFile.length() : 0;
            int[] filePages = filePages(mRequested, pages, pageCount);
            if (filePages == null) {
                fail("the adapter did not write the pages asked for: " + Arrays.toString(pages), null);
                return;
            }
            if (size == 0) {
                fail("the adapter wrote an empty document", null);
                return;
            }
            if (mDocInfo != null) mDocInfo.setDataSize(size);
            mFilePages = filePages;
            // The print UI is done: the job is queued for the printer and the adapter's session ends.
            setState(PrintJobInfo.STATE_QUEUED, null);
            finish();
            send();
        }

        // -- the print service's part --

        private void send() {
            final int session = mSession;
            final String label;
            synchronized (this) {
                label = info.getLabel();
            }
            mPrintId = sHost.print(mSetup, mFile.getAbsolutePath(), label, mFilePages,
                    (state, status) -> onMain(session, () -> printState(state, status), null));
            if (mPrintId == 0) {
                setState(PrintJobInfo.STATE_FAILED, "the printer could not be reached");
                logw("print job \"" + label + "\" failed: the printer could not be reached", null);
            }
        }

        private void printState(int state, String status) {
            switch (state) {
                case HostPrinting.STARTED:
                    setState(PrintJobInfo.STATE_STARTED, null);
                    break;
                case HostPrinting.COMPLETED:
                    mPrintId = 0;
                    discardFile();
                    releaseSetup();
                    setState(PrintJobInfo.STATE_COMPLETED, null);
                    break;
                case HostPrinting.PRINT_CANCELLED:
                    mPrintId = 0;
                    discardFile();
                    releaseSetup();
                    setState(PrintJobInfo.STATE_CANCELED, null);
                    break;
                default:
                    // Kept: restart() prints the same document on the same printer again.
                    mPrintId = 0;
                    setState(PrintJobInfo.STATE_FAILED, status != null ? status : "the printer failed");
                    break;
            }
        }

        // -- the app's PrintJob --

        void cancel() {
            int state;
            synchronized (this) {
                state = info.getState();
            }
            if (mPrintId != 0) {
                synchronized (this) {
                    info.setCancelling(true);
                }
                sHost.cancel(mPrintId);
            } else if (state == PrintJobInfo.STATE_QUEUED || state == PrintJobInfo.STATE_STARTED
                    || state == PrintJobInfo.STATE_BLOCKED || state == PrintJobInfo.STATE_FAILED) {
                discardFile();
                releaseSetup();
                setState(PrintJobInfo.STATE_CANCELED, null);
            }
        }

        void restart() {
            int state;
            synchronized (this) {
                state = info.getState();
            }
            if (state != PrintJobInfo.STATE_FAILED || mPrintId != 0) return;
            if (mFile != null && mFile.isFile() && mSetup != 0) {
                // The document is spooled: the printer is asked again, as the spooler queues it again.
                setState(PrintJobInfo.STATE_QUEUED, null);
                send();
            } else if (!mDropped) {
                // Failed before it was spooled: the print UI starts over.
                setState(PrintJobInfo.STATE_CREATED, null);
                start();
            }
        }

        // -- endings --

        void fail(String why, Throwable t) {
            logw("print job \"" + info.getLabel() + "\" failed: " + why, t);
            discardFile();
            releaseSetup();
            finish();
            setState(PrintJobInfo.STATE_FAILED, why);
        }

        private void cancelled() {
            discardFile();
            releaseSetup();
            finish();
            setState(PrintJobInfo.STATE_CANCELED, null);
        }

        private void finish() {
            if (mFinishCalled) return;
            mFinishCalled = true;
            // The adapter's session is over: the activity's end no longer concerns the job.
            unwatchActivity();
            try {
                mAdapter.onFinish();
            } catch (Throwable t) {
                logw("onFinish threw", t);
            }
        }

        private void discardFile() {
            if (mFile != null) {
                mFile.delete();
                mFile = null;
            }
        }

        private void releaseSetup() {
            if (mSetup != 0) {
                sHost.release(mSetup);
                mSetup = 0;
            }
        }

        /** Runs `r` on the main thread if the job is still in `session`; `otherwise` when it is not. */
        private void onMain(int session, Runnable r) {
            onMain(session, r, null);
        }

        private void onMain(int session, Runnable r, Runnable otherwise) {
            main().post(() -> {
                if (session != mSession) {
                    if (otherwise != null) otherwise.run();
                    return;
                }
                try {
                    r.run();
                } catch (Throwable t) {
                    fail("print job aborted: " + t, t);
                }
            });
        }
    }

    private static void closeQuietly(ParcelFileDescriptor fd) {
        try {
            fd.close();
        } catch (Throwable ignored) {
        }
    }

    private static void logw(String msg, Throwable t) {
        try {
            if (t != null) Log.w(LOG_TAG, msg, t); else Log.w(LOG_TAG, msg);
        } catch (Throwable ignored) {
        }
    }
}
