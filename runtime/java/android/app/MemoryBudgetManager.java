package android.app;
/** The system service that tells an app how much memory it may use before the
 *  system counts it as over budget. Apps name the type when they ask
 *  Context.getSystemService for it and go without when the answer is null,
 *  which it is here: Mac.apk sets no memory budgets. The class exists so that
 *  the code naming it links; nothing constructs one. */
public final class MemoryBudgetManager {
    MemoryBudgetManager() {}
}
