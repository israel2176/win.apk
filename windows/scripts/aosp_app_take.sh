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
# Then platform classes apps extend or implement that Win.apk has no class of (PLATFORM_TAKE below):
# R8 puts a stand-in for each such class into an app built for an older minSdk, a class whose static
# initializer throws NoClassDefFoundError; on a device the boot class path's class wins over it, so
# here the platform's must exist too, or the app's stand-in is loaded and its first subclass dies.
# Google Play services' slice providers (android.app.slice.SliceProvider) were the first: it crashed
# at every start installing them. A nested class of one of Win.apk's own (NsdManager's
# ServiceInfoCallback, WifiManager's LocalOnlyHotspotCallback) is declared in Win.apk's class instead.
#
#   aosp_app_take <android-all's entry list>    prints the class files to take, one per line

# android.app.slice: the slice provider and its manager (an in-process ISliceManager answers it,
# windows/java/aosp_app SliceService). android.app.appsearch, android.net.wifi.aware and .rtt: whole
# packages, as on a device; their managers are not handed out (getSystemService answers null).
# MediaRoute2ProviderService: a media route provider's service, with the AIDL it is bound through.
PLATFORM_TAKE='android/app/slice/.*|android/app/appsearch/.*|android/net/wifi/(aware|rtt)/.*'
PLATFORM_TAKE+='|android/media/(MediaRoute2ProviderService|MediaRoute2ProviderInfo|IMediaRoute2ProviderService'
PLATFORM_TAKE+='|IMediaRoute2ProviderServiceCallback)([$].*)?'

aosp_app_take() {
    grep -E '^(com/android/internal/app/ToolbarActionBar([$].*)?|android/content/BroadcastReceiver([$].*)?|android/content/(ContentProviderNative|ContentProviderProxy|IContentProvider)([$].*)?|android/os/(ICancellationSignal|RemoteCallback|IRemoteCallback)([$].*)?|android/app/servertransaction/.*|android/app/ActivityManager[$]TaskDescription([$].*)?|android/app/(ActionBar|ActivityThread|ActivityThreadInternal|ClientTransactionHandler|ConfigurationController|Activity|ActivityClient|ActivityTaskManager|Instrumentation|ContextImpl|ReceiverRestrictedContext|LoadedApk|Application|ResourcesManager|IActivityManager|IActivityTaskManager|IActivityClientController|IApplicationThread|IAppTask|IServiceConnection|IIntentReceiver|IInstrumentationWatcher|IUiAutomationConnection|ProfilerInfo|ResultInfo|ActivityTransitionState|ActivityTransitionCoordinator|EnterTransitionCoordinator|ExitTransitionCoordinator|SharedElementCallback|Fragment|FragmentController|FragmentHostCallback|FragmentContainer|FragmentManager|FragmentManagerImpl|FragmentManagerState|FragmentManagerNonConfig|FragmentState|FragmentTransaction|FragmentTransition|FragmentBreadCrumbs|BackStackRecord|BackStackState|LoaderManager|LoaderManagerImpl|DialogFragment|ListFragment|ListActivity|ExpandableListActivity|AliasActivity|LauncherActivity|ActivityGroup|LocalActivityManager|TabActivity|ActivityOptions|ComponentOptions|Service|IntentService|ServiceStartArgs|TaskStackBuilder|VoiceInteractor|DirectAction|PictureInPictureParams|PictureInPictureUiState)([$].*)?)\.class$' \
        "$1"
    grep -E "^($PLATFORM_TAKE)\\.class\$" "$1"
}
