// PrintManagerTest -- a print job through PrintSpooler as Android's print spooler drives it, over a stand-in
// for Windows' printers and Print dialog (PrintSpooler.Host), on a plain JVM with a main looper:
//   - the adapter's calls in a device's order, on the main thread: onStart; onLayout for the printer
//     selected first (no old attributes, a preview); onLayout again for the dialog's choice (the old
//     attributes, not a preview); onWrite for the pages chosen; onFinish once the job is queued;
//   - the job's states: CREATED from print(), QUEUED, STARTED, then COMPLETED, FAILED with the printer's
//     words, or CANCELED (the dialog cancelled, PrintJob.cancel());
//   - the document handed to the printer is what the adapter wrote, with the pages chosen picked out of a
//     whole document; a failed job restarts on the same printer;
//   - getPrintJobs lists the app's jobs; print() outside an activity throws, as AOSP's does;
//   - Windows' settings in Android's terms: media sizes, orientation, colour, duplex, page ranges, and
//     the app's attributes as hints for the dialog.
// Prints PASS/FAIL per check and "ALL PASSED" at the end; exits 1 on a failure.
package android.print;

import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.Looper;
import android.os.ParcelFileDescriptor;

import com.macapk.loader.HostPrinting;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public class PrintManagerTest {
    static int failures;

    static void check(String name, boolean ok) {
        System.out.println((ok ? "PASS " : "FAIL ") + name);
        if (!ok) failures++;
    }

    static volatile Thread sMainThread;

    // ---- the stand-in for Windows ----

    /** A4 portrait at 600 dpi, 1/6 inch margins, a colour device that can print two-sided. */
    static int[] a4() { return new int[] {8268, 11693, 167, 167, 167, 167, 600, 600, 2, 1, 1, 1, 1}; }

    /** Letter turned to landscape, monochrome, long-edge duplex, two copies. */
    static int[] letterLandscape() {
        return new int[] {11000, 8500, 250, 250, 250, 250, 300, 300, 1, 1, 2, 2, 1};
    }

    static final class FakeHost implements PrintSpooler.Host {
        final List<String> events = Collections.synchronizedList(new ArrayList<>());
        volatile int[] lastHints;
        volatile int lastPageCount;
        volatile String chooseAnswer = "chosen";     // chosen | cancelled | failed
        volatile int[] chosenRanges = {2, 3};
        volatile String printAnswer = "completed";   // completed | failed | hold
        volatile String printedPath;
        volatile byte[] printedBytes;
        volatile int[] printedPages;
        volatile long printedSetup;
        volatile HostPrinting.PrintListener held;
        final List<Long> released = Collections.synchronizedList(new ArrayList<>());
        int nextPrint = 1;

        @Override public boolean defaults(int[] hints, String[] names, int[] numbers) {
            lastHints = hints.clone();
            names[0] = "Office";
            names[1] = "A4";
            System.arraycopy(a4(), 0, numbers, 0, numbers.length);
            return true;
        }

        @Override public boolean choose(int pageCount, int[] hints, HostPrinting.ChooseListener l) {
            lastPageCount = pageCount;
            events.add("dialog");
            new Thread(() -> {
                if (chooseAnswer.equals("chosen")) {
                    l.onChosen(HostPrinting.CHOSEN, 7, new String[] {"Laser", "Letter"}, letterLandscape(),
                            chosenRanges);
                } else if (chooseAnswer.equals("failed")) {
                    l.onChosen(HostPrinting.FAILED, 0, new String[] {"No printer is installed"}, null, null);
                } else {
                    l.onChosen(HostPrinting.CANCELLED, 0, null, null, null);
                }
            }).start();
            return true;
        }

        @Override public int print(long setup, String path, String jobName, int[] pages,
                HostPrinting.PrintListener l) {
            printedSetup = setup;
            printedPath = path;
            printedPages = pages.clone();
            try {
                printedBytes = Files.readAllBytes(new File(path).toPath());
            } catch (Exception e) {
                printedBytes = null;
            }
            events.add("print " + jobName);
            final String answer = printAnswer;
            new Thread(() -> {
                l.onPrintState(HostPrinting.STARTED, null);
                if (answer.equals("completed")) l.onPrintState(HostPrinting.COMPLETED, null);
                else if (answer.equals("failed")) l.onPrintState(HostPrinting.PRINT_FAILED, "Paper jam");
                else held = l;
            }).start();
            return nextPrint++;
        }

        @Override public void cancel(int printId) {
            events.add("cancel " + printId);
            HostPrinting.PrintListener l = held;
            if (l != null) new Thread(() -> l.onPrintState(HostPrinting.PRINT_CANCELLED, null)).start();
        }

        @Override public void release(long setup) { released.add(setup); }
    }

    // ---- the app's adapter ----

    static final class Adapter extends PrintDocumentAdapter {
        final List<String> calls = Collections.synchronizedList(new ArrayList<>());
        volatile boolean offMain;
        volatile PrintAttributes lastNew, lastOld;
        volatile String failLayout;
        volatile boolean writeAll;
        final CountDownLatch finished = new CountDownLatch(1);

        void note(String s) {
            if (Thread.currentThread() != sMainThread) offMain = true;
            calls.add(s);
        }

        @Override public void onStart() { note("start"); }

        @Override public void onLayout(PrintAttributes oldAttributes, PrintAttributes newAttributes,
                CancellationSignal cancellationSignal, LayoutResultCallback callback, Bundle extras) {
            note("layout " + (oldAttributes == null ? "first" : "again") + " preview="
                    + extras.getBoolean(PrintDocumentAdapter.EXTRA_PRINT_PREVIEW));
            lastOld = oldAttributes;
            lastNew = newAttributes;
            if (failLayout != null) {
                callback.onLayoutFailed(failLayout);
                return;
            }
            // Answered from another thread, as an adapter that lays out in the background does.
            new Thread(() -> callback.onLayoutFinished(new PrintDocumentInfo.Builder("doc.pdf")
                    .setContentType(PrintDocumentInfo.CONTENT_TYPE_DOCUMENT).setPageCount(4).build(),
                    oldAttributes == null || !oldAttributes.equals(newAttributes))).start();
        }

        @Override public void onWrite(PageRange[] pages, ParcelFileDescriptor destination,
                CancellationSignal cancellationSignal, WriteResultCallback callback) {
            note("write " + Arrays.toString(pages));
            try (FileOutputStream out = new FileOutputStream(destination.getFileDescriptor())) {
                out.write("%PDF-1.4 test document".getBytes("US-ASCII"));
            } catch (Exception e) {
                callback.onWriteFailed(e.toString());
                return;
            }
            callback.onWriteFinished(writeAll ? PageRange.ALL_PAGES_ARRAY : pages);
        }

        @Override public void onFinish() {
            note("finish");
            finished.countDown();
        }
    }

    // ---- helpers ----

    static PrintJobInfo waitFor(PrintJobId id, int state) throws InterruptedException {
        long end = System.currentTimeMillis() + 5000;
        PrintJobInfo info = PrintSpooler.info(id);
        while (info.getState() != state && System.currentTimeMillis() < end) {
            Thread.sleep(5);
            info = PrintSpooler.info(id);
        }
        return info;
    }

    static PrintJobInfo start(Adapter adapter, PrintAttributes hint) {
        return PrintSpooler.create("Report", adapter, hint, null);
    }

    public static void main(String[] args) throws Exception {
        CountDownLatch ready = new CountDownLatch(1);
        Thread main = new Thread(() -> {
            Looper.prepareMainLooper();
            ready.countDown();
            Looper.loop();
        }, "main");
        main.setDaemon(true);
        main.start();
        ready.await();
        sMainThread = main;

        File spool = Files.createTempDirectory("print-test").toFile();
        PrintSpooler.sSpoolDir = spool;
        FakeHost host = new FakeHost();
        PrintSpooler.sHost = host;
        PrintManager pm = new PrintManager();

        // 1. A whole job.
        Adapter a = new Adapter();
        PrintAttributes hint = new PrintAttributes.Builder()
                .setMediaSize(PrintAttributes.MediaSize.ISO_A4.asLandscape())
                .setColorMode(PrintAttributes.COLOR_MODE_MONOCHROME).build();
        PrintJobInfo created = start(a, hint);
        check("print() returns the job created", created.getState() == PrintJobInfo.STATE_CREATED);
        PrintJobInfo done = waitFor(created.getId(), PrintJobInfo.STATE_COMPLETED);
        check("the job completes", done.getState() == PrintJobInfo.STATE_COMPLETED);
        check("the adapter's calls in a device's order: " + a.calls, a.calls.equals(Arrays.asList(
                "start", "layout first preview=true", "layout again preview=false", "write [PageRange[1 - 2]]",
                "finish")));
        check("every adapter call on the main thread", !a.offMain);
        check("the dialog came between the two layouts and before printing",
                host.events.equals(Arrays.asList("dialog", "print Report")));
        check("the dialog knew the page count", host.lastPageCount == 4);
        check("the app's attributes were the dialog's hints: A4, landscape, monochrome",
                Arrays.equals(host.lastHints, new int[] {9, 2, 1, 0}));
        check("the second layout was given the first's attributes as old",
                a.lastOld != null && a.lastOld.getMediaSize().getId().equals("ISO_A4"));
        check("and the dialog's choice as new: Letter landscape, monochrome, long edge",
                a.lastNew.getMediaSize().getId().equals("NA_LETTER") && !a.lastNew.getMediaSize().isPortrait()
                        && a.lastNew.getColorMode() == PrintAttributes.COLOR_MODE_MONOCHROME
                        && a.lastNew.getDuplexMode() == PrintAttributes.DUPLEX_MODE_LONG_EDGE
                        && a.lastNew.getResolution().getHorizontalDpi() == 300);
        check("the printer got what the adapter wrote",
                host.printedBytes != null && new String(host.printedBytes, "US-ASCII").startsWith("%PDF-1.4"));
        check("on the setup the dialog chose", host.printedSetup == 7);
        check("every page of the file (it holds exactly the pages asked)", host.printedPages.length == 0);
        check("the job names its printer, copies and pages", "Laser".equals(done.getPrinterName())
                && done.getCopies() == 2 && Arrays.equals(done.getPages(), new PageRange[] {new PageRange(1, 2)}));
        check("the document's size is recorded", done.getDocumentInfo().getDataSize() > 0);
        check("the spooled document is removed once printed", !new File(host.printedPath).exists());
        check("the dialog's setup is released", host.released.contains(7L));
        PrintJob handle = new PrintJob(created, pm);
        check("PrintJob reads the completed state", handle.isCompleted());

        // 2. The adapter writes the whole document: the pages chosen are picked from it.
        host.events.clear();
        Adapter all = new Adapter();
        all.writeAll = true;
        PrintJobInfo j2 = start(all, null);
        waitFor(j2.getId(), PrintJobInfo.STATE_COMPLETED);
        check("a whole document: the printer is told pages 2 and 3 of it",
                Arrays.equals(host.printedPages, new int[] {1, 2}));
        check("no app attributes: no hints", Arrays.equals(host.lastHints, new int[] {0, 0, 0, 0}));

        // 3. The dialog cancelled.
        host.chooseAnswer = "cancelled";
        Adapter c = new Adapter();
        PrintJobInfo j3 = start(c, null);
        PrintJobInfo cancelled = waitFor(j3.getId(), PrintJobInfo.STATE_CANCELED);
        c.finished.await(5, TimeUnit.SECONDS);
        check("cancelling the dialog cancels the job", cancelled.getState() == PrintJobInfo.STATE_CANCELED);
        check("and ends the adapter's session without a write: " + c.calls,
                c.calls.equals(Arrays.asList("start", "layout first preview=true", "finish")));

        // 4. No printer at all.
        host.chooseAnswer = "failed";
        Adapter np = new Adapter();
        PrintJobInfo j4 = start(np, null);
        PrintJobInfo noPrinter = waitFor(j4.getId(), PrintJobInfo.STATE_FAILED);
        check("the dialog's failure fails the job with its words",
                noPrinter.getState() == PrintJobInfo.STATE_FAILED
                        && String.valueOf(noPrinter.getStatus(null)).contains("No printer"));
        host.chooseAnswer = "chosen";

        // 5. The adapter fails its layout.
        Adapter bad = new Adapter();
        bad.failLayout = "nothing to print";
        PrintJobInfo j5 = start(bad, null);
        PrintJobInfo failed = waitFor(j5.getId(), PrintJobInfo.STATE_FAILED);
        bad.finished.await(5, TimeUnit.SECONDS);
        check("a failed layout fails the job with the adapter's message",
                failed.getState() == PrintJobInfo.STATE_FAILED
                        && String.valueOf(failed.getStatus(null)).contains("nothing to print"));
        check("and calls onFinish", bad.calls.contains("finish"));

        // 6. The printer fails; the job restarts on the same printer.
        host.printAnswer = "failed";
        Adapter jam = new Adapter();
        PrintJobInfo j6 = start(jam, null);
        PrintJobInfo jammed = waitFor(j6.getId(), PrintJobInfo.STATE_FAILED);
        check("the printer's failure fails the job with its words",
                jammed.getState() == PrintJobInfo.STATE_FAILED
                        && "Paper jam".equals(String.valueOf(jammed.getStatus(null))));
        check("the adapter's session ended when the job was queued", jam.calls.contains("finish"));
        host.printAnswer = "completed";
        host.printedBytes = null;
        new PrintJob(jammed, pm).restart();
        PrintJobInfo restarted = waitFor(j6.getId(), PrintJobInfo.STATE_COMPLETED);
        check("restart prints the spooled document again", restarted.getState() == PrintJobInfo.STATE_COMPLETED
                && host.printedBytes != null && host.printedBytes.length > 0);
        check("without asking the adapter again", jam.calls.size() == 5);

        // 7. PrintJob.cancel() while the printer has it.
        host.printAnswer = "hold";
        Adapter slow = new Adapter();
        PrintJobInfo j7 = start(slow, null);
        PrintJobInfo started = waitFor(j7.getId(), PrintJobInfo.STATE_STARTED);
        check("the job is started while the printer takes it", started.getState() == PrintJobInfo.STATE_STARTED);
        PrintJob running = new PrintJob(started, pm);
        running.cancel();
        PrintJobInfo stopped = waitFor(j7.getId(), PrintJobInfo.STATE_CANCELED);
        check("PrintJob.cancel() stops the printer and cancels the job",
                stopped.getState() == PrintJobInfo.STATE_CANCELED && host.events.contains("cancel 5"));
        check("a cancelled job's document is removed", !new File(host.printedPath).exists());
        host.printAnswer = "completed";

        // 8. The app's jobs.
        List<PrintJob> jobs = pm.getPrintJobs();
        check("getPrintJobs lists every job this app made", jobs.size() == 7);
        check("each in its current state", jobs.get(0).isCompleted() && jobs.get(2).isCancelled());

        // 9. print() outside an activity.
        boolean threw = false;
        try {
            pm.print("Report", new Adapter(), null);
        } catch (IllegalStateException e) {
            threw = true;
        }
        check("print() outside an activity throws IllegalStateException", threw);

        // 10. Windows' settings in Android's terms.
        check("A4 in mils is ISO_A4", PrintSpooler.mediaSizeOf("A4", 8268, 11693).getId().equals("ISO_A4"));
        PrintAttributes.MediaSize turned = PrintSpooler.mediaSizeOf("Letter", 11000, 8500);
        check("Letter turned is NA_LETTER landscape", turned.getId().equals("NA_LETTER") && !turned.isPortrait());
        check("11 x 17 in is Tabloid, the portrait one",
                PrintSpooler.mediaSizeOf("", 11000, 17000).getId().equals("NA_TABLOID"));
        check("a driver's rounding still matches",
                PrintSpooler.mediaSizeOf("", 8270, 11690).getId().equals("ISO_A4"));
        PrintAttributes.MediaSize photo = PrintSpooler.mediaSizeOf("Photo 13x18", 5118, 7087);
        check("an unknown paper keeps Windows' name", photo.getLabel(null).equals("Photo 13x18")
                && photo.getWidthMils() == 5118 && photo.getHeightMils() == 7087);
        int[] mono = a4();
        mono[8] = 0;
        mono[9] = 0;
        check("a monochrome device prints monochrome", PrintSpooler.attributesOf("A4", mono).getColorMode()
                == PrintAttributes.COLOR_MODE_MONOCHROME);
        int[] shortEdge = a4();
        shortEdge[10] = 3;
        check("short-edge duplex", PrintSpooler.attributesOf("A4", shortEdge).getDuplexMode()
                == PrintAttributes.DUPLEX_MODE_SHORT_EDGE);
        check("margins are the unprintable edges",
                PrintSpooler.attributesOf("A4", a4()).getMinMargins().getLeftMils() == 167);
        check("no ranges: every page", PrintSpooler.isAll(PrintSpooler.pagesOf(new int[0], 4)));
        check("1-4 of 4 pages: every page", PrintSpooler.isAll(PrintSpooler.pagesOf(new int[] {1, 4}, 4)));
        check("ranges are 0-based, merged and cut to the document",
                Arrays.equals(PrintSpooler.pagesOf(new int[] {5, 9, 1, 2, 3, 3}, 6),
                        new PageRange[] {new PageRange(0, 2), new PageRange(4, 5)}));
        PageRange[] want = {new PageRange(2, 3)};
        check("written exactly as asked: the whole file",
                PrintSpooler.filePages(want, want, 6).length == 0);
        check("written as the whole document: the pages asked, by index",
                Arrays.equals(PrintSpooler.filePages(want, PageRange.ALL_PAGES_ARRAY, 6), new int[] {2, 3}));
        check("written more than asked: the pages' places in the file",
                Arrays.equals(PrintSpooler.filePages(want, new PageRange[] {new PageRange(1, 4)}, 6),
                        new int[] {1, 2}));
        check("written less than asked: refused",
                PrintSpooler.filePages(want, new PageRange[] {new PageRange(2, 2)}, 6) == null);
        check("nothing written: refused", PrintSpooler.filePages(want, new PageRange[0], 6) == null);
        check("everything asked, everything written",
                PrintSpooler.filePages(PageRange.ALL_PAGES_ARRAY, new PageRange[] {new PageRange(0, 5)}, 6)
                        .length == 0);
        check("hints: unknown portrait says only the orientation", Arrays.equals(PrintSpooler.hintsOf(
                new PrintAttributes.Builder().setMediaSize(PrintAttributes.MediaSize.UNKNOWN_PORTRAIT).build()),
                new int[] {0, 1, 0, 0}));
        check("hints: Letter, colour, short edge", Arrays.equals(PrintSpooler.hintsOf(
                new PrintAttributes.Builder().setMediaSize(PrintAttributes.MediaSize.NA_LETTER)
                        .setColorMode(PrintAttributes.COLOR_MODE_COLOR)
                        .setDuplexMode(PrintAttributes.DUPLEX_MODE_SHORT_EDGE).build()),
                new int[] {1, 1, 2, 3}));

        System.out.println(failures == 0 ? "ALL PASSED" : "FAILED: " + failures);
        System.exit(failures == 0 ? 0 : 1);
    }
}
