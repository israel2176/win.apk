// android.print.PrintManager
//
// AOSP's printing entry point: apps fetch it with getSystemService(PRINT_SERVICE) and call
// print(jobName, adapter, attributes) from an activity. As on a device, print() returns the job at once
// (STATE_CREATED) and the print UI then drives the app's PrintDocumentAdapter on the main thread:
// onStart, onLayout, onWrite into a file, onFinish. The print UI is Windows' own Print dialog over the
// app's window, and the job's document is printed on the Windows printer chosen there (PrintSpooler,
// com.macapk.loader.HostPrinting). The job's states are a device's: QUEUED once the dialog has the
// document, STARTED while the printer takes it, then COMPLETED, FAILED (with why as its status) or
// CANCELED (the dialog cancelled, or PrintJob.cancel()).
//
// What differs: Windows' printers are no android.printservice.PrintService, so isPrintServiceEnabled
// answers false for every service, as on a device with none installed.
//
// Mirrors frameworks/base/core/java/android/print/PrintManager.java (AOSP 14).
package android.print;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.graphics.drawable.Icon;
import android.text.TextUtils;

import java.util.ArrayList;
import java.util.List;

public final class PrintManager {

    /** @hide AOSP's key for the PrintJobInfo returned by the spooler. */
    public static final String EXTRA_PRINT_JOB = "android.print.intent.extra.EXTRA_PRINT_JOB";
    /** @hide */
    public static final String EXTRA_PRINT_DOCUMENT_ADAPTER =
            "android.print.intent.extra.EXTRA_PRINT_DOCUMENT_ADAPTER";
    /** @hide */
    public static final String EXTRA_PRINT_DIALOG_INTENT =
            "android.print.intent.extra.EXTRA_PRINT_DIALOG_INTENT";

    private final Context mContext;

    /** @hide Kept for callers that construct one directly. */
    public PrintManager() { this(null); }

    /**
     * @hide SystemServiceRegistry makes the manager with the context's outer context (an activity's
     * ContextImpl gives the activity), which print() requires to be an Activity.
     */
    public PrintManager(Context context) { mContext = outerContext(context); }

    public PrintJob print(String printJobName, PrintDocumentAdapter documentAdapter,
            PrintAttributes attributes) {
        if (!(mContext instanceof Activity)) {
            throw new IllegalStateException("Can print only from an activity");
        }
        if (TextUtils.isEmpty(printJobName)) {
            throw new IllegalArgumentException("printJobName cannot be empty");
        }
        if (documentAdapter == null) {
            throw new IllegalArgumentException("documentAdapter cannot be null");
        }
        return new PrintJob(PrintSpooler.create(printJobName, documentAdapter, attributes, mContext), this);
    }

    /** The print jobs this app made while it runs, in their current states. */
    public List<PrintJob> getPrintJobs() {
        List<PrintJob> jobs = new ArrayList<PrintJob>();
        for (PrintJobInfo info : PrintSpooler.infos()) {
            jobs.add(new PrintJob(info, this));
        }
        return jobs;
    }

    /** No android.printservice.PrintService is installed: Windows' printers are the host's. */
    public boolean isPrintServiceEnabled(ComponentName service) {
        if (service == null) {
            throw new NullPointerException("service cannot be null");
        }
        return false;
    }

    /** @hide The icon a print service gave a printer: none, as no print service is installed. */
    public Icon getCustomPrinterIcon(PrinterId printerId) {
        return null;
    }

    // ---- package-private, used by PrintJob --------------------------------

    PrintJobInfo getPrintJobInfo(PrintJobId printJobId) {
        return PrintSpooler.info(printJobId);
    }

    void cancelPrintJob(PrintJobId printJobId) {
        PrintSpooler.cancel(printJobId);
    }

    void restartPrintJob(PrintJobId printJobId) {
        PrintSpooler.restart(printJobId);
    }

    /** ContextImpl.getOuterContext(): the activity (or service, or application) the context is the base
     *  of; the context itself when it is no ContextImpl. */
    private static Context outerContext(Context context) {
        if (context == null) return null;
        for (Class<?> c = context.getClass(); c != null; c = c.getSuperclass()) {
            if (!c.getName().equals("android.app.ContextImpl")) continue;
            try {
                java.lang.reflect.Method m = c.getDeclaredMethod("getOuterContext");
                m.setAccessible(true);
                Object outer = m.invoke(context);
                return outer instanceof Context ? (Context) outer : context;
            } catch (ReflectiveOperationException | RuntimeException e) {
                return context;
            }
        }
        return context;
    }
}
