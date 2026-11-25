# aosp_app_take.sh -- sourced, not run: which of android-all 14's classes are AOSP's app layer in
# Win.apk (docs/research/aosp-app.md). build_aosp_app.sh puts them (and what they need) into
# lib/aosp-app.jar; build_java.sh compiles the framework against them, since the framework has no
# Activity, Application, Instrumentation, LoadedApk, ActivityThread or framework Fragment of its own,
# so that both agree on one list.
#
# The lifecycle and what drives it in the app's process. A nested class goes with its outer one, but
# for ActivityManager's TaskDescription (AOSP's Activity builds one from its theme; Win.apk's
# ActivityManager lacks its setters), taken alone: Win.apk's ActivityManager stays. ToolbarActionBar
# is Activity's setActionBar(Toolbar): without it the verifier cannot check Activity, and the
# framework's class-data sharing archive leaves Activity out.
#
#   aosp_app_take <android-all's entry list>    prints the class files to take, one per line

aosp_app_take() {
    grep -E '^(com/android/internal/app/ToolbarActionBar([$].*)?|android/content/BroadcastReceiver([$].*)?|android/content/(ContentProviderNative|ContentProviderProxy|IContentProvider)([$].*)?|android/os/(ICancellationSignal|RemoteCallback|IRemoteCallback)([$].*)?|android/app/servertransaction/.*|android/app/ActivityManager[$]TaskDescription([$].*)?|android/app/(ActionBar|ActivityThread|ActivityThreadInternal|ClientTransactionHandler|ConfigurationController|Activity|ActivityClient|ActivityTaskManager|Instrumentation|ContextImpl|ReceiverRestrictedContext|LoadedApk|Application|ResourcesManager|IActivityManager|IActivityTaskManager|IActivityClientController|IApplicationThread|IAppTask|IServiceConnection|IIntentReceiver|IInstrumentationWatcher|IUiAutomationConnection|ProfilerInfo|ResultInfo|ActivityTransitionState|ActivityTransitionCoordinator|EnterTransitionCoordinator|ExitTransitionCoordinator|SharedElementCallback|Fragment|FragmentController|FragmentHostCallback|FragmentContainer|FragmentManager|FragmentManagerImpl|FragmentManagerState|FragmentManagerNonConfig|FragmentState|FragmentTransaction|FragmentTransition|FragmentBreadCrumbs|BackStackRecord|BackStackState|LoaderManager|LoaderManagerImpl|DialogFragment|ListFragment|ListActivity|ExpandableListActivity|AliasActivity|LauncherActivity|ActivityGroup|LocalActivityManager|TabActivity|ActivityOptions|ComponentOptions|Service|IntentService|ServiceStartArgs|TaskStackBuilder|VoiceInteractor|DirectAction|PictureInPictureParams|PictureInPictureUiState)([$].*)?)\.class$' \
        "$1"
}
