# API gaps: the full lists

The generated tables behind [`api-gaps.md`](api-gaps.md), from main's CI build `f4cb4f4` (run
45853914292) against the SDK of levels 34, 35 and 36 and the eleven apps of the smoke and UI-gate sets.
`windows/scripts/api_gaps/run_scan.sh` and `report.py` make the sections up to "In-process services",
`services_map.py` "System services" and `ndk_gaps.py` "Native code"; the severity columns of the last two
were written by hand. The class, member and stub lists start at level 34; the tool's TSV files have every
level. "Level" is the API
level the item was added at (api-versions.xml); "hidden" is a member outside the SDK an app names.

## 1. Counts per level

| Level | Missing classes | Missing methods and fields | Missing constants (inlined) | Stubs |
|---:|---:|---:|---:|---:|
| 1 | 15 | 54 | 96 | 202 |
| 3 | 0 | 11 | 63 | 52 |
| 4 | 2 | 2 | 10 | 36 |
| 5 | 0 | 1 | 95 | 45 |
| 7 | 0 | 0 | 0 | 12 |
| 8 | 1 | 2 | 14 | 82 |
| 9 | 11 | 0 | 29 | 24 |
| 10 | 0 | 0 | 0 | 19 |
| 11 | 1 | 0 | 15 | 57 |
| 12 | 9 | 0 | 0 | 17 |
| 13 | 0 | 0 | 1 | 0 |
| 14 | 0 | 3 | 63 | 63 |
| 15 | 0 | 0 | 8 | 3 |
| 16 | 4 | 0 | 57 | 34 |
| 17 | 0 | 1 | 35 | 19 |
| 18 | 0 | 2 | 6 | 40 |
| 19 | 0 | 2 | 6 | 44 |
| 20 | 3 | 1 | 0 | 1 |
| 21 | 9 | 412 | 67 | 166 |
| 22 | 0 | 1 | 1 | 19 |
| 23 | 14 | 20 | 117 | 158 |
| 24 | 100 | 671 | 291 | 114 |
| 25 | 0 | 0 | 5 | 29 |
| 26 | 31 | 29 | 61 | 110 |
| 27 | 0 | 5 | 5 | 7 |
| 28 | 32 | 42 | 46 | 132 |
| 29 | 46 | 140 | 102 | 99 |
| 30 | 36 | 57 | 289 | 94 |
| 31 | 138 | 101 | 155 | 106 |
| 32 | 0 | 0 | 31 | 9 |
| 33 | 80 | 97 | 167 | 76 |
| 34 | 205 | 186 | 205 | 59 |
| 35 | 150 | 226 | 182 | 86 |
| 36 | 184 | 120 | 251 | 74 |
| all | 1071 | 2186 | 2473 | 2088 |

## Missing classes, level 34 and later

| Package | Level: classes |
|---|---|
| `android.adservices.adselection` | **34**: AdSelectionOutcome$Builder, AddAdSelectionOverrideRequest, RemoveAdSelectionOverrideRequest, TestAdSelectionManager; **35**: AdWithBid, AddAdSelectionFromOutcomesOverrideRequest, DecisionLogic, PerBuyerDecisionLogic, RemoveAdSelectionFromOutcomesOverrideRequest, SetAppInstallAdvertisersRequest, SetAppInstallAdvertisersRequest$Builder, SignedContextualAds, SignedContextualAds$Builder; **36**: PerBuyerConfiguration, PerBuyerConfiguration$Builder, SellerConfiguration, SellerConfiguration$Builder |
| `android.adservices.common` | **34**: AdServicesPermissions; **35**: AdServicesOutcomeReceiver, AppInstallFilters, AppInstallFilters$Builder; **36**: ComponentAdData |
| `android.adservices.customaudience` | **34**: AddCustomAudienceOverrideRequest, AddCustomAudienceOverrideRequest$Builder, RemoveCustomAudienceOverrideRequest, RemoveCustomAudienceOverrideRequest$Builder, TestCustomAudienceManager; **36**: PartialCustomAudience, PartialCustomAudience$Builder, ScheduleCustomAudienceUpdateRequest, ScheduleCustomAudienceUpdateRequest$Builder |
| `android.adservices.exceptions` | **34**: AdServicesException |
| `android.adservices.measurement` | **35**: SourceRegistrationRequest, SourceRegistrationRequest$Builder |
| `android.adservices.ondevicepersonalization` | **35**: AppInfo, DownloadCompletedInput, DownloadCompletedOutput, DownloadCompletedOutput$Builder, EventInput, EventLogRecord, EventLogRecord$Builder, EventOutput, EventOutput$Builder, EventUrlProvider, ExecuteInput, ExecuteOutput, ExecuteOutput$Builder, FederatedComputeInput, FederatedComputeInput$Builder, FederatedComputeScheduler, FederatedComputeScheduler$Params, InferenceInput, InferenceInput$Builder, InferenceInput$Params, InferenceInput$Params$Builder, InferenceOutput, InferenceOutput$Builder, IsolatedService, IsolatedServiceException, IsolatedWorker, KeyValueStore, LogReader, ModelManager, MutableKeyValueStore, OnDevicePersonalizationException, OnDevicePersonalizationManager, OnDevicePersonalizationManager$ExecuteResult, RenderInput, RenderOutput, RenderOutput$Builder, RenderingConfig, RenderingConfig$Builder, RequestLogRecord, RequestLogRecord$Builder, RequestToken, SurfacePackageToken, TrainingExampleRecord, TrainingExampleRecord$Builder, TrainingExamplesInput, TrainingExamplesOutput, TrainingExamplesOutput$Builder, TrainingInterval, TrainingInterval$Builder, UserData, WebTriggerInput, WebTriggerOutput, WebTriggerOutput$Builder; **36**: ExecuteInIsolatedServiceRequest, ExecuteInIsolatedServiceRequest$Builder, ExecuteInIsolatedServiceRequest$OutputSpec, ExecuteInIsolatedServiceResponse |
| `android.app` | **35**: Notification$TvExtender |
| `android.app.admin` | **34**: DevicePolicyIdentifiers, PolicyUpdateReceiver, PolicyUpdateResult, TargetUser |
| `android.app.appfunctions` | **36**: AppFunctionException, AppFunctionManager, AppFunctionService, ExecuteAppFunctionRequest, ExecuteAppFunctionRequest$Builder, ExecuteAppFunctionResponse |
| `android.app.appsearch` | **34**: JoinSpec, JoinSpec$Builder, PropertyPath, PropertyPath$PathSegment, SearchSuggestionResult, SearchSuggestionResult$Builder, SearchSuggestionSpec, SearchSuggestionSpec$Builder; **35**: EnterpriseGlobalSearchSession, SchemaVisibilityConfig, SchemaVisibilityConfig$Builder; **36**: AppSearchBlobHandle, AppSearchSchema$BlobHandlePropertyConfig, AppSearchSchema$BlobHandlePropertyConfig$Builder, AppSearchSchema$EmbeddingPropertyConfig, AppSearchSchema$EmbeddingPropertyConfig$Builder, CommitBlobResponse, EmbeddingVector, OpenBlobForReadResponse, OpenBlobForWriteResponse, RemoveBlobResponse, SetBlobVisibilityRequest, SetBlobVisibilityRequest$Builder |
| `android.app.appsearch.util` | **34**: DocumentIdUtil |
| `android.app.jank` | **36**: AppJankStats, RelativeFrameTimeHistogram |
| `android.app.job` | **34**: JobWorkItem$Builder |
| `android.app.sdksandbox` | **34**: LoadSdkException, RequestSurfacePackageException, SandboxedSdkProvider, SdkSandboxManager$SdkSandboxProcessDeathCallback; **35**: AppOwnedSdkSandboxInterface |
| `android.app.sdksandbox.sdkprovider` | **34**: SdkSandboxActivityHandler; **36**: SdkSandboxClientImportanceListener |
| `android.app.wallpaper` | **36**: WallpaperDescription, WallpaperDescription$Builder, WallpaperInstance |
| `android.bluetooth` | **34**: BluetoothSocketException; **35**: BluetoothCodecType; **36**: BluetoothDevice$BluetoothAddress, BluetoothSocketSettings, BluetoothSocketSettings$Builder |
| `android.companion.virtual` | **35**: VirtualDeviceManager$VirtualDeviceListener |
| `android.content.om` | **34**: OverlayManager |
| `android.crypto.hpke` | **35**: HpkeSpi, XdhKeySpec |
| `android.devicelock` | **34**: DeviceId, DeviceLockManager |
| `android.graphics` | **35**: Matrix44; **36**: RuntimeColorFilter |
| `android.graphics.pdf` | **35**: LoadParams, LoadParams$Builder, PdfRendererPreV, PdfRendererPreV$Page, RenderParams, RenderParams$Builder |
| `android.graphics.pdf.content` | **35**: PdfPageGotoLinkContent, PdfPageGotoLinkContent$Destination, PdfPageImageContent, PdfPageLinkContent, PdfPageTextContent |
| `android.graphics.pdf.models` | **35**: FormEditRecord, FormEditRecord$Builder, FormWidgetInfo, FormWidgetInfo$Builder, ListItem, PageMatchBounds |
| `android.graphics.pdf.models.selection` | **35**: PageSelection, SelectionBoundary |
| `android.health.connect` | **34**: AggregateRecordsGroupedByDurationResponse, AggregateRecordsGroupedByPeriodResponse, AggregateRecordsRequest, AggregateRecordsRequest$Builder, AggregateRecordsResponse, HealthConnectException, HealthConnectManager, HealthPermissions, InsertRecordsResponse, LocalTimeRangeFilter, LocalTimeRangeFilter$Builder, ReadRecordsRequest, ReadRecordsRequestUsingFilters, ReadRecordsRequestUsingFilters$Builder, ReadRecordsRequestUsingIds, ReadRecordsRequestUsingIds$Builder, ReadRecordsResponse, RecordIdFilter, TimeInstantRangeFilter, TimeInstantRangeFilter$Builder, TimeRangeFilter; **36**: CreateMedicalDataSourceRequest, CreateMedicalDataSourceRequest$Builder, DeleteMedicalResourcesRequest, DeleteMedicalResourcesRequest$Builder, GetMedicalDataSourcesRequest, GetMedicalDataSourcesRequest$Builder, MedicalResourceId, ReadMedicalResourcesInitialRequest, ReadMedicalResourcesInitialRequest$Builder, ReadMedicalResourcesPageRequest, ReadMedicalResourcesPageRequest$Builder, ReadMedicalResourcesRequest, ReadMedicalResourcesResponse, UpsertMedicalResourceRequest, UpsertMedicalResourceRequest$Builder |
| `android.health.connect.changelog` | **34**: ChangeLogTokenRequest, ChangeLogTokenRequest$Builder, ChangeLogTokenResponse, ChangeLogsRequest, ChangeLogsRequest$Builder, ChangeLogsResponse, ChangeLogsResponse$DeletedLog |
| `android.health.connect.datatypes` | **34**: ActiveCaloriesBurnedRecord, ActiveCaloriesBurnedRecord$Builder, AggregationType, AppInfo, BasalBodyTemperatureRecord, BasalBodyTemperatureRecord$Builder, BasalMetabolicRateRecord, BasalMetabolicRateRecord$Builder, BloodGlucoseRecord, BloodGlucoseRecord$Builder, BloodGlucoseRecord$RelationToMealType, BloodGlucoseRecord$SpecimenSource, BloodPressureRecord, BloodPressureRecord$BloodPressureMeasurementLocation, BloodPressureRecord$BodyPosition, BloodPressureRecord$Builder, BodyFatRecord, BodyFatRecord$Builder, BodyTemperatureMeasurementLocation, BodyTemperatureRecord, BodyTemperatureRecord$Builder, BodyWaterMassRecord, BodyWaterMassRecord$Builder, BoneMassRecord, BoneMassRecord$Builder, CervicalMucusRecord, CervicalMucusRecord$Builder, CervicalMucusRecord$CervicalMucusAppearance, CervicalMucusRecord$CervicalMucusSensation, CyclingPedalingCadenceRecord, CyclingPedalingCadenceRecord$Builder, CyclingPedalingCadenceRecord$CyclingPedalingCadenceRecordSample, DataOrigin, DataOrigin$Builder, Device, Device$Builder, DistanceRecord, DistanceRecord$Builder, ElevationGainedRecord, ElevationGainedRecord$Builder, ExerciseLap, ExerciseLap$Builder, ExerciseRoute, ExerciseRoute$Location, ExerciseRoute$Location$Builder, ExerciseSegment, ExerciseSegment$Builder, ExerciseSegmentType, ExerciseSessionRecord, ExerciseSessionRecord$Builder, ExerciseSessionType, FloorsClimbedRecord, FloorsClimbedRecord$Builder, HeartRateRecord, HeartRateRecord$Builder, HeartRateRecord$HeartRateSample, HeartRateVariabilityRmssdRecord, HeartRateVariabilityRmssdRecord$Builder, HeightRecord, HeightRecord$Builder, HydrationRecord, HydrationRecord$Builder, InstantRecord, IntermenstrualBleedingRecord, IntermenstrualBleedingRecord$Builder, IntervalRecord, LeanBodyMassRecord, LeanBodyMassRecord$Builder, MealType, MenstruationFlowRecord, MenstruationFlowRecord$Builder, MenstruationFlowRecord$MenstruationFlowType, MenstruationPeriodRecord, MenstruationPeriodRecord$Builder, Metadata, Metadata$Builder, NutritionRecord, NutritionRecord$Builder, OvulationTestRecord, OvulationTestRecord$Builder, OvulationTestRecord$OvulationTestResult, OxygenSaturationRecord, OxygenSaturationRecord$Builder, PowerRecord, PowerRecord$Builder, PowerRecord$PowerRecordSample, Record, RespiratoryRateRecord, RespiratoryRateRecord$Builder, RestingHeartRateRecord, RestingHeartRateRecord$Builder, SexualActivityRecord, SexualActivityRecord$Builder, SexualActivityRecord$SexualActivityProtectionUsed, SleepSessionRecord, SleepSessionRecord$Builder, SleepSessionRecord$Stage, SleepSessionRecord$StageType, SpeedRecord, SpeedRecord$Builder, SpeedRecord$SpeedRecordSample, StepsCadenceRecord, StepsCadenceRecord$Builder, StepsCadenceRecord$StepsCadenceRecordSample, StepsRecord, StepsRecord$Builder, TotalCaloriesBurnedRecord, TotalCaloriesBurnedRecord$Builder, Vo2MaxRecord, Vo2MaxRecord$Builder, Vo2MaxRecord$Vo2MaxMeasurementMethod, WeightRecord, WeightRecord$Builder, WheelchairPushesRecord, WheelchairPushesRecord$Builder; **35**: ExerciseCompletionGoal, ExerciseCompletionGoal$ActiveCaloriesBurnedGoal, ExerciseCompletionGoal$DistanceGoal, ExerciseCompletionGoal$DistanceWithVariableRestGoal, ExerciseCompletionGoal$DurationGoal, ExerciseCompletionGoal$RepetitionsGoal, ExerciseCompletionGoal$StepsGoal, ExerciseCompletionGoal$TotalCaloriesBurnedGoal, ExerciseCompletionGoal$UnknownGoal, ExerciseCompletionGoal$UnspecifiedGoal, ExercisePerformanceGoal, ExercisePerformanceGoal$AmrapGoal, ExercisePerformanceGoal$CadenceGoal, ExercisePerformanceGoal$HeartRateGoal, ExercisePerformanceGoal$PowerGoal, ExercisePerformanceGoal$RateOfPerceivedExertionGoal, ExercisePerformanceGoal$SpeedGoal, ExercisePerformanceGoal$UnknownGoal, ExercisePerformanceGoal$WeightGoal, PlannedExerciseBlock, PlannedExerciseBlock$Builder, PlannedExerciseSessionRecord, PlannedExerciseSessionRecord$Builder, PlannedExerciseStep, PlannedExerciseStep$Builder, SkinTemperatureRecord, SkinTemperatureRecord$Builder, SkinTemperatureRecord$Delta; **36**: ActivityIntensityRecord, ActivityIntensityRecord$Builder, FhirResource, FhirResource$Builder, FhirVersion, MedicalDataSource, MedicalDataSource$Builder, MedicalResource, MedicalResource$Builder, MindfulnessSessionRecord, MindfulnessSessionRecord$Builder |
| `android.health.connect.datatypes.units` | **34**: BloodGlucose, Energy, Length, Mass, Percentage, Power, Pressure, Temperature, Velocity, Volume; **35**: TemperatureDelta |
| `android.icu.number` | **34**: NumberFormatter$RoundingPriority, NumberFormatter$TrailingZeroDisplay |
| `android.icu.text` | **35**: DisplayOptions, DisplayOptions$Builder, DisplayOptions$Capitalization, DisplayOptions$DisplayLength, DisplayOptions$GrammaticalCase, DisplayOptions$NameStyle, DisplayOptions$NounClass, DisplayOptions$PluralCategory, DisplayOptions$SubstituteHandling |
| `android.icu.util` | **34**: MeasureUnit$MeasurePrefix |
| `android.media.quality` | **36**: ActiveProcessingPicture, AmbientBacklightEvent, AmbientBacklightMetadata, AmbientBacklightSettings, MediaQualityContract, MediaQualityContract$PictureQuality, MediaQualityContract$SoundQuality, MediaQualityManager, MediaQualityManager$AmbientBacklightCallback, MediaQualityManager$PictureProfileCallback, MediaQualityManager$ProfileQueryParams, MediaQualityManager$ProfileQueryParams$Builder, MediaQualityManager$SoundProfileCallback, ParameterCapability, PictureProfile, PictureProfile$Builder, SoundProfile, SoundProfile$Builder |
| `android.media.tv` | **34**: AdBuffer, TableResponse$Builder, TvRecordingInfo; **36**: SignalingDataInfo, SignalingDataRequest, SignalingDataResponse |
| `android.media.tv.ad` | **36**: TvAdManager, TvAdManager$TvAdServiceCallback, TvAdService, TvAdService$Session, TvAdServiceInfo, TvAdView, TvAdView$OnUnhandledInputEventListener, TvAdView$TvAdCallback |
| `android.net` | **35**: IpSecTransformState, IpSecTransformState$Builder; **36**: L2capNetworkSpecifier, L2capNetworkSpecifier$Builder, TetheringInterface, TetheringManager, TetheringManager$StartTetheringCallback, TetheringManager$StopTetheringCallback, TetheringManager$TetheringEventCallback, TetheringManager$TetheringRequest, TetheringManager$TetheringRequest$Builder |
| `android.net.http` | **34**: UrlRequest$Status |
| `android.net.nsd` | **34**: NsdManager$ServiceInfoCallback; **35**: DiscoveryRequest, DiscoveryRequest$Builder; **36**: AdvertisingRequest, AdvertisingRequest$Builder |
| `android.net.wifi` | **34**: WifiAvailableChannel, WifiManager$LocalOnlyConnectionFailureListener; **36**: BlockingOption, BlockingOption$Builder, SoftApConfiguration$Builder, WifiManager$WifiStateChangedListener |
| `android.net.wifi.aware` | **34**: AwarePairingConfig, AwarePairingConfig$Builder |
| `android.net.wifi.p2p` | **35**: WifiP2pDiscoveryConfig, WifiP2pDiscoveryConfig$Builder, WifiP2pManager$WifiP2pListener; **36**: WifiP2pDirInfo, WifiP2pPairingBootstrappingConfig, WifiP2pUsdBasedLocalServiceAdvertisementConfig, WifiP2pUsdBasedLocalServiceAdvertisementConfig$Builder, WifiP2pUsdBasedServiceDiscoveryConfig, WifiP2pUsdBasedServiceDiscoveryConfig$Builder |
| `android.net.wifi.p2p.nsd` | **36**: WifiP2pUsdBasedServiceConfig, WifiP2pUsdBasedServiceConfig$Builder, WifiP2pUsdBasedServiceResponse |
| `android.net.wifi.rtt` | **35**: RangingResult$Builder; **36**: PasnConfig, PasnConfig$Builder, SecureRangingConfig, SecureRangingConfig$Builder |
| `android.nfc` | **34**: AvailableNfcAntenna, NfcAntennaInfo |
| `android.nfc.cardemulation` | **35**: PollingFrame; **36**: CardEmulation$NfcEventCallback |
| `android.provider` | **35**: E2eeContactKeysManager, E2eeContactKeysManager$E2eeContactKey, E2eeContactKeysManager$E2eeSelfKey; **36**: CloudMediaProviderContract$Capabilities, CloudMediaProviderContract$Capabilities$Builder, CloudMediaProviderContract$MediaCategoryColumns, CloudMediaProviderContract$MediaSetColumns, CloudMediaProviderContract$SearchSuggestionColumns |
| `android.ranging` | **36**: DataNotificationConfig, DataNotificationConfig$Builder, RangingCapabilities, RangingConfig, RangingData, RangingDevice, RangingDevice$Builder, RangingManager, RangingManager$RangingCapabilitiesCallback, RangingMeasurement, RangingPreference, RangingPreference$Builder, RangingSession, RangingSession$Callback, SensorFusionParams, SensorFusionParams$Builder, SessionConfig, SessionConfig$Builder |
| `android.ranging.ble.cs` | **36**: BleCsRangingCapabilities, BleCsRangingParams, BleCsRangingParams$Builder |
| `android.ranging.ble.rssi` | **36**: BleRssiRangingParams, BleRssiRangingParams$Builder |
| `android.ranging.oob` | **36**: DeviceHandle, DeviceHandle$Builder, OobInitiatorRangingConfig, OobInitiatorRangingConfig$Builder, OobResponderRangingConfig, OobResponderRangingConfig$Builder, TransportHandle, TransportHandle$ReceiveCallback |
| `android.ranging.raw` | **36**: RawInitiatorRangingConfig, RawInitiatorRangingConfig$Builder, RawRangingDevice, RawRangingDevice$Builder, RawResponderRangingConfig, RawResponderRangingConfig$Builder |
| `android.ranging.uwb` | **36**: UwbAddress, UwbComplexChannel, UwbComplexChannel$Builder, UwbRangingCapabilities, UwbRangingParams, UwbRangingParams$Builder |
| `android.ranging.wifi.rtt` | **36**: RttRangingCapabilities, RttRangingParams, RttRangingParams$Builder |
| `android.telephony.ims` | **34**: SipDetails |
| `android.telephony.satellite` | **36**: SatelliteManager, SatelliteStateChangeListener |
| `android.text` | **35**: Layout$Builder |
| `android.view` | **35**: SurfaceControl$TransactionStats |
| `android.widget` | **35**: RemoteViews$DrawInstructions, RemoteViews$DrawInstructions$Builder |
| `android.widget.photopicker` | **36**: EmbeddedPhotoPickerClient, EmbeddedPhotoPickerFeatureInfo, EmbeddedPhotoPickerFeatureInfo$Builder, EmbeddedPhotoPickerProvider, EmbeddedPhotoPickerProviderFactory, EmbeddedPhotoPickerSession |
| `dalvik.annotation.optimization` | **34**: CriticalNative, FastNative |
| `javax.annotation.processing` | **34**: Generated |

## Missing methods and fields, level 34 and later (classes that exist)

| Class | Level: members |
|---|---|
| `android.adservices.adid.AdIdManager` | **35**: getAdId() |
| `android.adservices.adselection.AdSelectionConfig` | **34**: CREATOR, describeContents(), getAdSelectionSignals(), getCustomAudienceBuyers(), getDecisionLogicUri(), getPerBuyerSignals(), getSeller(), getSellerSignals(), getTrustedScoringSignalsUri(), writeToParcel(); **35**: getPerBuyerSignedContextualAds() |
| `android.adservices.adselection.AdSelectionConfig$Builder` | **35**: setPerBuyerSignedContextualAds() |
| `android.adservices.adselection.AdSelectionFromOutcomesConfig` | **35**: CREATOR, describeContents(), getAdSelectionIds(), getSelectionLogicUri(), getSelectionSignals(), getSeller(), writeToParcel() |
| `android.adservices.adselection.AdSelectionManager` | **34**: getTestAdSelectionManager(); **35**: setAppInstallAdvertisers() |
| `android.adservices.adselection.AdSelectionOutcome` | **35**: NO_OUTCOME, hasOutcome(); **36**: getComponentAdUris(), getWinningSeller() |
| `android.adservices.adselection.GetAdSelectionDataOutcome` | **35**: getAdSelectionDataId() |
| `android.adservices.adselection.GetAdSelectionDataRequest` | **35**: getCoordinatorOriginUri(), getSeller(); **36**: getSellerConfiguration() |
| `android.adservices.adselection.GetAdSelectionDataRequest$Builder` | **36**: setSellerConfiguration() |
| `android.adservices.adselection.PersistAdSelectionResultRequest` | **35**: getAdSelectionDataId(), getAdSelectionId(), getAdSelectionResult(), getSeller() |
| `android.adservices.adselection.PersistAdSelectionResultRequest$Builder` | **35**: setAdSelectionDataId() |
| `android.adservices.adselection.ReportEventRequest` | **35**: getAdSelectionId(), getData(), getInputEvent(), getKey(), getReportingDestinations() |
| `android.adservices.adselection.ReportEventRequest$Builder` | **35**: setAdSelectionId(), setData(), setKey(), setReportingDestinations() |
| `android.adservices.adselection.ReportImpressionRequest` | **34**: getAdSelectionConfig(), getAdSelectionId() |
| `android.adservices.adselection.UpdateAdCounterHistogramRequest` | **35**: getAdEventType(), getAdSelectionId(), getCallerAdTech() |
| `android.adservices.adselection.UpdateAdCounterHistogramRequest$Builder` | **35**: setAdEventType(), setAdSelectionId(), setCallerAdTech() |
| `android.adservices.common.AdData` | **34**: CREATOR, describeContents(), getMetadata(), getRenderUri(), writeToParcel(); **35**: getAdCounterKeys(), getAdFilters(), getAdRenderId() |
| `android.adservices.common.AdFilters` | **35**: CREATOR, describeContents(), getAppInstallFilters(), getFrequencyCapFilters(), writeToParcel() |
| `android.adservices.common.AdFilters$Builder` | **35**: setAppInstallFilters() |
| `android.adservices.common.AdSelectionSignals` | **34**: CREATOR, EMPTY, describeContents(), writeToParcel() |
| `android.adservices.common.AdTechIdentifier` | **34**: CREATOR, describeContents(), writeToParcel() |
| `android.adservices.common.FrequencyCapFilters` | **35**: CREATOR, describeContents(), getKeyedFrequencyCapsForClickEvents(), getKeyedFrequencyCapsForImpressionEvents(), getKeyedFrequencyCapsForViewEvents(), getKeyedFrequencyCapsForWinEvents(), writeToParcel() |
| `android.adservices.common.KeyedFrequencyCap` | **35**: CREATOR, describeContents(), getAdCounterKey(), getInterval(), getMaxCount(), writeToParcel() |
| `android.adservices.common.KeyedFrequencyCap$Builder` | **35**: setAdCounterKey(), setInterval(), setMaxCount() |
| `android.adservices.customaudience.CustomAudience` | **34**: CREATOR, describeContents(), getActivationTime(), getAds(), getBiddingLogicUri(), getBuyer(), getDailyUpdateUri(), getExpirationTime(), getName(), getTrustedBiddingData(), getUserBiddingSignals(), writeToParcel(); **35**: getAuctionServerRequestFlags(); **36**: getComponentAds(), getPriority() |
| `android.adservices.customaudience.CustomAudience$Builder` | **35**: setAuctionServerRequestFlags(); **36**: setComponentAds(), setPriority() |
| `android.adservices.customaudience.CustomAudienceManager` | **34**: getTestCustomAudienceManager(); **36**: scheduleCustomAudienceUpdate() |
| `android.adservices.customaudience.FetchAndJoinCustomAudienceRequest` | **35**: getActivationTime(), getExpirationTime(), getFetchUri(), getName(), getUserBiddingSignals() |
| `android.adservices.customaudience.FetchAndJoinCustomAudienceRequest$Builder` | **35**: setFetchUri() |
| `android.adservices.customaudience.JoinCustomAudienceRequest` | **34**: getCustomAudience() |
| `android.adservices.customaudience.LeaveCustomAudienceRequest` | **34**: getBuyer(), getName() |
| `android.adservices.customaudience.TrustedBiddingData` | **34**: CREATOR, describeContents(), getTrustedBiddingKeys(), getTrustedBiddingUri(), writeToParcel() |
| `android.adservices.measurement.DeletionRequest` | **34**: getDeletionMode(), getDomainUris(), getEnd(), getMatchBehavior(), getOriginUris(), getStart() |
| `android.adservices.measurement.MeasurementManager` | **35**: deleteRegistrations(), getMeasurementApiStatus(), registerSource(), registerTrigger(), registerWebSource(), registerWebTrigger() |
| `android.adservices.measurement.WebSourceParams` | **34**: CREATOR, describeContents(), getRegistrationUri(), isDebugKeyAllowed(), writeToParcel() |
| `android.adservices.measurement.WebSourceRegistrationRequest` | **34**: CREATOR, describeContents(), getAppDestination(), getInputEvent(), getSourceParams(), getTopOriginUri(), getVerifiedDestination(), getWebDestination(), writeToParcel() |
| `android.adservices.measurement.WebTriggerParams` | **34**: CREATOR, describeContents(), getRegistrationUri(), isDebugKeyAllowed(), writeToParcel() |
| `android.adservices.measurement.WebTriggerRegistrationRequest` | **34**: CREATOR, describeContents(), getDestination(), getTriggerParams(), writeToParcel() |
| `android.adservices.signals.UpdateSignalsRequest` | **35**: getUpdateUri() |
| `android.adservices.signals.UpdateSignalsRequest$Builder` | **35**: setUpdateUri() |
| `android.adservices.topics.GetTopicsRequest` | **34**: getAdsSdkName(), shouldRecordObservation() |
| `android.adservices.topics.GetTopicsResponse$Builder` | **34**: constructor (Ljava/util/List;); **35**: constructor (Ljava/util/List;Ljava/util/List;) |
| `android.app.Activity` | **35**: getCaller(), getCurrentCaller(), getInitialCaller(), onActivityResult(), onNewIntent(), onRequestPermissionsResult(), requestPermissions(), setIntent(), shouldShowRequestPermissionRationale(); **36**: requestOpenInBrowserEducation() |
| `android.app.ActivityOptions` | **36**: isAllowPassThroughOnTouchOutside(), setAllowPassThroughOnTouchOutside() |
| `android.app.AlarmManager` | **34**: setWindow() |
| `android.app.AutomaticZenRule` | **35**: getDeviceEffects(), setDeviceEffects() |
| `android.app.AutomaticZenRule$Builder` | **35**: setDeviceEffects(), setZenPolicy() |
| `android.app.Instrumentation` | **35**: callActivityOnNewIntent() |
| `android.app.PictureInPictureUiState` | **35**: isTransitioningToPip() |
| `android.app.Service` | **35**: onTimeout() |
| `android.app.admin.DevicePolicyManager` | **34**: bindDeviceAdminServiceAsUser() |
| `android.app.job.JobWorkItem` | **34**: getExtras() |
| `android.app.sdksandbox.SandboxedSdk` | **34**: CREATOR, describeContents(), getSharedLibraryInfo(), writeToParcel() |
| `android.app.sdksandbox.SdkSandboxManager` | **34**: addSdkSandboxProcessDeathCallback(), addSyncedSharedPreferencesKeys(), getSandboxedSdks(), getSyncedSharedPreferencesKeys(), loadSdk(), removeSdkSandboxProcessDeathCallback(), removeSyncedSharedPreferencesKeys(), requestSurfacePackage(), startSdkSandboxActivity(), unloadSdk(); **35**: getAppOwnedSdkSandboxInterfaces(), registerAppOwnedSdkSandboxInterface(), unregisterAppOwnedSdkSandboxInterface() |
| `android.app.sdksandbox.sdkprovider.SdkSandboxController` | **34**: getClientSharedPreferences(), registerSdkSandboxActivityHandler(), unregisterSdkSandboxActivityHandler(); **35**: getAppOwnedSdkSandboxInterfaces(), loadSdk(); **36**: registerSdkSandboxClientImportanceListener(), unregisterSdkSandboxClientImportanceListener() |
| `android.bluetooth.BluetoothA2dp` | **35**: getSupportedCodecTypes() |
| `android.bluetooth.BluetoothAdapter` | **36**: listenUsingSocketSettings() |
| `android.bluetooth.BluetoothCodecConfig` | **35**: getExtendedCodecType() |
| `android.bluetooth.BluetoothCodecConfig$Builder` | **35**: setExtendedCodecType() |
| `android.bluetooth.BluetoothDevice` | **36**: createUsingSocketSettings(), getIdentityAddressWithType() |
| `android.bluetooth.le.AdvertiseSettings` | **34**: isDiscoverable() |
| `android.bluetooth.le.AdvertiseSettings$Builder` | **34**: setDiscoverable() |
| `android.bluetooth.le.AdvertisingSetParameters` | **34**: isDiscoverable() |
| `android.bluetooth.le.AdvertisingSetParameters$Builder` | **34**: setDiscoverable() |
| `android.companion.virtual.VirtualDevice` | **34**: CREATOR, describeContents(), writeToParcel() |
| `android.companion.virtual.VirtualDeviceManager` | **35**: registerVirtualDeviceListener(), unregisterVirtualDeviceListener() |
| `android.content.Context` | **34**: bindServiceAsUser() |
| `android.content.om.FabricatedOverlay` | **35**: setNinePatchResourceValue(), setResourceValue() |
| `android.content.res.FontScaleConverter` | **35**: forScale(), isNonLinearFontScalingActive() |
| `android.content.res.Resources` | **35**: registerResourcePaths() |
| `android.graphics.BitmapShader` | **35**: setOverrideGainmap() |
| `android.graphics.Canvas` | **35**: clipOutShader(), clipShader(), concat(); **36**: drawRegion() |
| `android.graphics.ColorSpace$Named` | **36**: OK_LAB |
| `android.graphics.Gainmap` | **36**: getAlternativeImagePrimaries(), getGainmapDirection(), setAlternativeImagePrimaries(), setGainmapDirection() |
| `android.graphics.RecordingCanvas` | **35**: drawRegion() |
| `android.graphics.RuntimeShader` | **36**: setInputColorFilter(), setInputXfermode() |
| `android.graphics.fonts.FontFamily$Builder` | **35**: buildVariableFamily() |
| `android.graphics.pdf.PdfRenderer` | **35**: constructor (Landroid/os/ParcelFileDescriptor;Landroid/graphics/pdf/LoadParams;), getDocumentLinearizationType(), getPdfFormType(), write() |
| `android.graphics.pdf.PdfRenderer$Page` | **35**: applyEdit(), getFormWidgetInfoAtIndex(), getFormWidgetInfoAtPosition(), getFormWidgetInfos(), getGotoLinks(), getImageContents(), getLinkContents(), getTextContents(), render(), searchText(), selectContent() |
| `android.graphics.text.LineBreakConfig` | **35**: CREATOR, getHyphenation(), merge() |
| `android.graphics.text.LineBreakConfig$Builder` | **35**: merge(), setHyphenation() |
| `android.graphics.text.PositionedGlyphs` | **35**: getFakeBold(), getFakeItalic(), getItalicOverride(), getWeightOverride() |
| `android.hardware.input.InputManager` | **34**: getHostUsiVersion(), isStylusPointerIconEnabled(); **35**: getInputDeviceViewBehavior() |
| `android.icu.lang.UCharacter` | **34**: hasBinaryProperty() |
| `android.icu.number.FormattedNumber` | **35**: getNounClass() |
| `android.icu.number.FractionPrecision` | **34**: withSignificantDigits() |
| `android.icu.number.LocalizedNumberFormatter` | **36**: withoutLocale() |
| `android.icu.number.NumberFormatter$SignDisplay` | **34**: ACCOUNTING_NEGATIVE, NEGATIVE |
| `android.icu.number.NumberFormatterSettings` | **35**: displayOptions() |
| `android.icu.number.Precision` | **34**: trailingZeroDisplay() |
| `android.icu.text.DateTimePatternGenerator` | **35**: getDateTimeFormat(), setDateTimeFormat() |
| `android.icu.text.Normalizer2` | **36**: getNFKCSimpleCasefoldInstance() |
| `android.icu.text.NumberFormat$Field` | **35**: APPROXIMATELY_SIGN |
| `android.icu.text.UnicodeSet` | **34**: hasStrings() |
| `android.icu.util.Calendar` | **36**: getTemporalMonthCode(), inTemporalLeapYear(), setTemporalMonthCode() |
| `android.icu.util.MeasureUnit` | **34**: ITEM, KILOWATT_HOUR_PER_100_KILOMETER, MILLIGRAM_OFGLUCOSE_PER_DECILITER, getPrefix(), withPrefix(); **35**: QUARTER, TONNE; **36**: BEAUFORT, GASOLINE_ENERGY_DENSITY |
| `android.icu.util.VersionInfo` | **34**: UNICODE_15_0; **35**: UNICODE_15_1; **36**: UNICODE_16_0 |
| `android.net.ConnectivityManager` | **36**: reserveNetwork() |
| `android.net.ConnectivityManager$NetworkCallback` | **36**: onReserved() |
| `android.net.NetworkCapabilities` | **35**: getSubscriptionIds() |
| `android.net.NetworkRequest` | **35**: getSubscriptionIds() |
| `android.net.NetworkRequest$Builder` | **35**: setSubscriptionIds() |
| `android.net.http.BidirectionalStream` | **34**: getHeaders(), getHttpMethod(), getPriority(), getTrafficStatsTag(), getTrafficStatsUid(), hasTrafficStatsTag(), hasTrafficStatsUid(), isDelayRequestHeadersUntilFirstFlushEnabled() |
| `android.net.http.ConnectionMigrationOptions` | **34**: getAllowNonDefaultNetworkUsage(), getDefaultNetworkMigration(), getPathDegradationMigration() |
| `android.net.http.DnsOptions` | **34**: getPersistHostCache(), getPersistHostCachePeriod(), getPreestablishConnectionsToStaleDnsResults(), getStaleDns(), getStaleDnsOptions(), getUseHttpStackDnsResolver() |
| `android.net.http.DnsOptions$StaleDnsOptions` | **34**: getAllowCrossNetworkUsage(), getFreshLookupTimeout(), getMaxExpiredDelay(), getUseStaleOnNameNotResolved() |
| `android.net.http.QuicOptions` | **34**: getAllowedQuicHosts(), getHandshakeUserAgent(), getIdleConnectionTimeout(), getInMemoryServerConfigsCacheSize(), hasInMemoryServerConfigsCacheSize() |
| `android.net.http.UrlRequest` | **34**: getHeaders(), getHttpMethod(), getPriority(), getTrafficStatsTag(), getTrafficStatsUid(), hasTrafficStatsTag(), hasTrafficStatsUid(), isCacheDisabled(), isDirectExecutorAllowed() |
| `android.net.ipsec.ike.IkeSessionParams` | **35**: getIkeOptions() |
| `android.net.nsd.NsdManager` | **34**: registerServiceInfoCallback(), stopServiceResolution(), unregisterServiceInfoCallback(); **35**: discoverServices() |
| `android.net.nsd.NsdManager$ResolveListener` | **34**: onResolutionStopped(), onStopResolutionFailed() |
| `android.net.nsd.NsdServiceInfo` | **34**: getHostAddresses(), setHostAddresses(); **35**: getSubtypes(), setSubtypes(); **36**: getHostname() |
| `android.net.wifi.ScanResult` | **36**: isRangingFrameProtectionRequired(), isSecureHeLtfSupported() |
| `android.net.wifi.WifiEnterpriseConfig` | **34**: getMinimumTlsVersion(), setMinimumTlsVersion() |
| `android.net.wifi.WifiInfo` | **34**: getAssociatedMloLinks() |
| `android.net.wifi.WifiManager` | **34**: addLocalOnlyConnectionFailureListener(), getAllowedChannels(), getChannelData(), getMaxSupportedConcurrentTdlsSessions(), getNumberOfEnabledTdlsSessions(), getUsableChannels(), isTdlsOperationCurrentlyAvailable(), removeLocalOnlyConnectionFailureListener(), setTdlsEnabled(), setTdlsEnabledWithMacAddress(), validateSoftApConfiguration(); **35**: getPerSsidRoamingModes(), querySendDhcpHostnameRestriction(), removePerSsidRoamingMode(), setPerSsidRoamingMode(), setSendDhcpHostnameRestriction(); **36**: addWifiStateChangedListener(), disallowCurrentSuggestedNetwork(), removeWifiStateChangedListener(), startLocalOnlyHotspotWithConfiguration() |
| `android.net.wifi.WifiNetworkSpecifier` | **34**: getPreferredChannelFrequenciesMhz() |
| `android.net.wifi.p2p.WifiP2pConfig` | **34**: getGroupClientIpProvisioningMode(); **36**: getGroupOwnerVersion(), getPairingBootstrappingConfig(), getPccModeConnectionType(), isAuthorizeConnectionFromPeerEnabled(), setGroupOwnerVersion() |
| `android.net.wifi.p2p.WifiP2pConfig$Builder` | **34**: setGroupClientIpProvisioningMode(); **36**: setAuthorizeConnectionFromPeerEnabled(), setPairingBootstrappingConfig(), setPccModeConnectionType() |
| `android.net.wifi.p2p.WifiP2pDevice` | **35**: getIpAddress(); **36**: isOpportunisticBootstrappingMethodSupported(), isPassphraseDisplayBootstrappingMethodSupported(), isPassphraseKeypadBootstrappingMethodSupported(), isPinCodeDisplayBootstrappingMethodSupported(), isPinCodeKeypadBootstrappingMethodSupported() |
| `android.net.wifi.p2p.WifiP2pGroup` | **36**: getGroupOwnerBssid(), getSecurityType() |
| `android.net.wifi.p2p.WifiP2pManager` | **34**: getListenState(), isGroupOwnerIPv6LinkLocalAddressProvided(), setWfdInfo(); **35**: registerWifiP2pListener(), startPeerDiscovery(), unregisterWifiP2pListener(); **36**: discoverUsdBasedServices(), isPccModeSupported(), isWiFiDirectR2Supported(), requestDirInfo(), startUsdBasedLocalServiceAdvertisement(), validateDirInfo() |
| `android.net.wifi.p2p.WifiP2pManager$ServiceResponseListener` | **36**: onUsdBasedServiceAvailable() |
| `android.net.wifi.p2p.nsd.WifiP2pServiceInfo` | **36**: constructor (Landroid/net/wifi/p2p/nsd/WifiP2pUsdBasedServiceConfig;), getWifiP2pUsdBasedServiceConfig() |
| `android.net.wifi.p2p.nsd.WifiP2pServiceRequest` | **36**: constructor (Landroid/net/wifi/p2p/nsd/WifiP2pUsdBasedServiceConfig;), getWifiP2pUsdBasedServiceConfig() |
| `android.nfc.NfcAdapter` | **34**: getNfcAntennaInfo(); **35**: isObserveModeEnabled(), isObserveModeSupported(), isReaderOptionEnabled(), isReaderOptionSupported(), resetDiscoveryTechnology(), setDiscoveryTechnology(), setObserveModeEnabled(); **36**: disable(), enable(), isTagIntentAllowed(), isTagIntentAppPreferenceSupported() |
| `android.nfc.cardemulation.CardEmulation` | **35**: registerPollingLoopFilterForService(), registerPollingLoopPatternFilterForService(), removePollingLoopFilterForService(), removePollingLoopPatternFilterForService(), setShouldDefaultToObserveModeForService(); **36**: getDefaultNfcSubscriptionId(), isEuiccSupported(), registerNfcEventCallback(), unregisterNfcEventCallback() |
| `android.nfc.cardemulation.HostApduService` | **35**: processPollingFrames() |
| `android.os.PerformanceHintManager$Session` | **35**: reportActualWorkDuration() |
| `android.os.ProfilingResult` | **35**: CREATOR, describeContents(), writeToParcel() |
| `android.service.wallpaper.WallpaperService` | **36**: onCreateEngine() |
| `android.service.wallpaper.WallpaperService$Engine` | **36**: onApplyWallpaper() |
| `android.system.OsConstants` | **34**: IFA_F_MANAGETEMPADDR, IFA_F_NOPREFIXROUTE; **35**: IPPROTO_ESP, IPV6_PKTINFO, NETLINK_XFRM, PACKET_IGNORE_OUTGOING, SOL_PACKET |
| `android.text.BoringLayout$Metrics` | **35**: getDrawingBoundingBox() |
| `android.text.Layout` | **35**: computeDrawingBoundingBox(), getBreakStrategy(), getEllipsize(), getHyphenationFrequency(), getJustificationMode(), getLeftIndents(), getLineBreakConfig(), getLineLetterSpacingUnitCount(), getMaxLines(), getRightIndents(), isFontPaddingIncluded() |
| `android.view.AttachedSurfaceControl` | **35**: getInputTransferToken(); **36**: registerOnJankDataListener() |
| `android.view.Display` | **36**: getHighestHdrSdrRatio(), getSuggestedFrameRate(), hasArrSupport() |
| `android.view.SurfaceControl$Transaction` | **35**: addTransactionCompletedListener(); **36**: setLuts() |
| `android.view.SurfaceControlViewHost` | **35**: constructor (Landroid/content/Context;Landroid/view/Display;Landroid/window/InputTransferToken;) |
| `android.view.SurfaceControlViewHost$SurfacePackage` | **35**: getInputTransferToken() |
| `android.view.View` | **35**: clearPendingCredentialRequest(), getPendingCredentialCallback(), getPendingCredentialRequest(), setPendingCredentialRequest(); **36**: getSupplementalDescription(), reportAppJankStats(), setSupplementalDescription() |
| `android.view.ViewGroup` | **36**: propagateRequestedFrameRate() |
| `android.view.ViewStructure` | **35**: clearCredentialManagerRequest(), getPendingCredentialCallback(), getPendingCredentialRequest(), setPendingCredentialRequest() |
| `android.view.WindowManager` | **35**: addScreenRecordingCallback(), registerBatchedSurfaceControlInputReceiver(), registerTrustedPresentationListener(), registerUnbatchedSurfaceControlInputReceiver(), removeScreenRecordingCallback(), transferTouchGesture(), unregisterSurfaceControlInputReceiver(), unregisterTrustedPresentationListener() |
| `android.view.accessibility.AccessibilityManager` | **36**: addHighContrastTextStateChangeListener(), isHighContrastTextEnabled(), removeHighContrastTextStateChangeListener() |
| `android.view.accessibility.AccessibilityNodeInfo` | **36**: addLabeledBy(), getChecked(), getExpandedState(), getLabeledByList(), getSupplementalDescription(), isFieldRequired(), removeLabeledBy(), setChecked(), setExpandedState(), setFieldRequired(), setSupplementalDescription() |
| `android.view.accessibility.AccessibilityNodeInfo$RangeInfo` | **36**: INDETERMINATE |
| `android.view.autofill.AutofillId` | **36**: getAutofillVirtualId(), isInAutofillSession(), isVirtual() |
| `android.view.contentcapture.ContentCaptureSession` | **36**: flush() |
| `android.view.inputmethod.EditorInfo` | **36**: getAutofillId(), isWritingToolsEnabled(), setAutofillId(), setWritingToolsEnabled() |
| `android.view.inputmethod.InputMethodInfo` | **36**: createImeLanguageSettingsActivityIntent() |
| `android.view.inputmethod.InputMethodManager` | **35**: startConnectionlessStylusHandwriting(), startConnectionlessStylusHandwritingForDelegation() |
| `android.view.inputmethod.InputMethodSubtype` | **36**: getLayoutDisplayName(), getLayoutLabelNonLocalized(), getLayoutLabelResource() |
| `android.view.inputmethod.InputMethodSubtype$InputMethodSubtypeBuilder` | **36**: setLayoutLabelNonLocalized(), setLayoutLabelResource() |
| `android.widget.RemoteViews` | **35**: constructor (Landroid/widget/RemoteViews$DrawInstructions;), setOnStylusHandwritingPendingIntent() |
| `android.window.BackEvent` | **36**: constructor (FFFIJ), getFrameTimeMillis() |

## Members that exist but would not link or would not run as on Android (any level)

| Level | Member | Problem |
|---:|---|---|
| 21 | `android.app.job.JobService.onBind(Landroid/content/Intent;)Landroid/os/IBinder;` | abstract |
| 24 | `android.icu.text.Collator.getDecomposition()I` | abstract |
| 24 | `android.icu.text.Collator.getStrength()I` | abstract |
| 24 | `android.icu.text.Collator.setDecomposition(I)V` | abstract |
| 24 | `android.icu.text.Collator.setStrength(I)V` | abstract |
| 24 | `android.icu.text.DateFormat.format(Ljava/lang/Object;Ljava/lang/StringBuffer;Ljava/text/FieldPosition;)Ljava/lang/StringBuffer;` | abstract |
| 24 | `android.icu.text.DateFormat.format(Ljava/util/Date;Ljava/lang/StringBuffer;Ljava/text/FieldPosition;)Ljava/lang/StringBuffer;` | abstract |
| 24 | `android.icu.text.DateFormat.isLenient()Z` | abstract |
| 24 | `android.icu.text.DateFormat.parse(Ljava/lang/String;)Ljava/util/Date;` | abstract |
| 24 | `android.icu.text.DateFormat.parse(Ljava/lang/String;Ljava/text/ParsePosition;)Ljava/util/Date;` | abstract |
| 24 | `android.icu.text.DateFormat.parseObject(Ljava/lang/String;Ljava/text/ParsePosition;)Ljava/lang/Object;` | abstract |
| 24 | `android.icu.text.DateFormat.setLenient(Z)V` | abstract |
| 24 | `android.icu.text.Normalizer2.normalize(Ljava/lang/CharSequence;)Ljava/lang/String;` | abstract |
| 24 | `android.icu.text.NumberFormat.format(Ljava/lang/Object;Ljava/lang/StringBuffer;Ljava/text/FieldPosition;)Ljava/lang/StringBuffer;` | abstract |
| 24 | `android.icu.text.NumberFormat.getCurrency()Landroid/icu/util/Currency;` | abstract |
| 24 | `android.icu.text.NumberFormat.getMaximumFractionDigits()I` | abstract |
| 24 | `android.icu.text.NumberFormat.getMaximumIntegerDigits()I` | abstract |
| 24 | `android.icu.text.NumberFormat.getMinimumFractionDigits()I` | abstract |
| 24 | `android.icu.text.NumberFormat.getMinimumIntegerDigits()I` | abstract |
| 24 | `android.icu.text.NumberFormat.isGroupingUsed()Z` | abstract |
| 24 | `android.icu.text.NumberFormat.isParseIntegerOnly()Z` | abstract |
| 24 | `android.icu.text.NumberFormat.parse(Ljava/lang/String;)Ljava/lang/Number;` | abstract |
| 24 | `android.icu.text.NumberFormat.parseObject(Ljava/lang/String;Ljava/text/ParsePosition;)Ljava/lang/Object;` | abstract |
| 24 | `android.icu.text.NumberFormat.setCurrency(Landroid/icu/util/Currency;)V` | abstract |
| 24 | `android.icu.text.NumberFormat.setGroupingUsed(Z)V` | abstract |
| 24 | `android.icu.text.NumberFormat.setMaximumFractionDigits(I)V` | abstract |
| 24 | `android.icu.text.NumberFormat.setMaximumIntegerDigits(I)V` | abstract |
| 24 | `android.icu.text.NumberFormat.setMinimumFractionDigits(I)V` | abstract |
| 24 | `android.icu.text.NumberFormat.setMinimumIntegerDigits(I)V` | abstract |
| 24 | `android.icu.text.NumberFormat.setParseIntegerOnly(Z)V` | abstract |
| 31 | `android.net.ParseException.<init>(Ljava/lang/String;)V` | not-public |
| 26 | `java.lang.reflect.Executable.getParameterCount()I` | abstract |
| 1 | `java.lang.reflect.Modifier.<init>()V` | not-public |
| 1 | `java.net.URLDecoder.<init>()V` | not-public |

## Stubs, level 34 and later

| Level | Method | Body | Jar |
|---:|---|---|---|
| 34 | `android.accessibilityservice.AccessibilityService.attachAccessibilityOverlayToDisplay` | empty | android-framework.jar |
| 34 | `android.accessibilityservice.AccessibilityService.attachAccessibilityOverlayToWindow` | empty | android-framework.jar |
| 34 | `android.app.NotificationManager.canUseFullScreenIntent` | returns-const | android-framework.jar |
| 34 | `android.app.UiAutomation.clearCache` | returns-const | android-framework.jar |
| 34 | `android.app.UiAutomation.takeScreenshot` | returns-const | android-framework.jar |
| 34 | `android.app.UiModeManager.addContrastChangeListener` | empty | android-framework.jar |
| 34 | `android.app.UiModeManager.getContrast` | returns-const | android-framework.jar |
| 34 | `android.app.UiModeManager.removeContrastChangeListener` | empty | android-framework.jar |
| 34 | `android.app.admin.DevicePolicyManager.getCredentialManagerPolicy` | returns-const | android-framework.jar |
| 34 | `android.app.job.JobService.onNetworkChanged` | empty | android-framework.jar |
| 34 | `android.app.job.JobService.setNotification` | empty | android-framework.jar |
| 34 | `android.app.job.JobService.updateEstimatedNetworkBytes` | empty | android-framework.jar |
| 34 | `android.app.job.JobService.updateEstimatedNetworkBytes` | empty | android-framework.jar |
| 34 | `android.app.job.JobService.updateTransferredNetworkBytes` | empty | android-framework.jar |
| 34 | `android.app.job.JobService.updateTransferredNetworkBytes` | empty | android-framework.jar |
| 34 | `android.app.job.JobServiceEngine.setNotification` | empty | android-framework.jar |
| 34 | `android.app.job.JobServiceEngine.updateEstimatedNetworkBytes` | empty | android-framework.jar |
| 34 | `android.app.job.JobServiceEngine.updateTransferredNetworkBytes` | empty | android-framework.jar |
| 34 | `android.app.sdksandbox.sdkprovider.SdkSandboxController.getSandboxedSdks` | returns-const | android-framework.jar |
| 34 | `android.companion.AssociatedDevice.writeToParcel` | empty | android-framework.jar |
| 34 | `android.companion.CompanionDeviceManager.buildAssociationCancellationIntent` | returns-const | android-framework.jar |
| 34 | `android.companion.CompanionDeviceManager.detachSystemDataTransport` | empty | android-framework.jar |
| 34 | `android.companion.CompanionDeviceManager.disableSystemDataSyncForTypes` | empty | android-framework.jar |
| 34 | `android.companion.CompanionDeviceManager.enableSystemDataSyncForTypes` | empty | android-framework.jar |
| 34 | `android.companion.CompanionDeviceService.detachSystemDataTransport` | empty | android-framework.jar |
| 34 | `android.companion.virtual.VirtualDeviceManager.getVirtualDevices` | returns-const | android-framework.jar |
| 34 | `android.content.RestrictionsManager.getApplicationRestrictionsPerAdmin` | returns-const | android-framework.jar |
| 34 | `android.credentials.CredentialManager.isEnabledCredentialProviderService` | returns-const | android-framework.jar |
| 34 | `android.credentials.PrepareGetCredentialResponse.hasAuthenticationResults` | returns-const | android-framework.jar |
| 34 | `android.credentials.PrepareGetCredentialResponse.hasRemoteResults` | returns-const | android-framework.jar |
| 34 | `android.hardware.camera2.CameraExtensionCharacteristics.getPostviewSupportedSizes` | returns-const | android-framework.jar |
| 34 | `android.hardware.camera2.CameraExtensionCharacteristics.isCaptureProcessProgressAvailable` | returns-const | android-framework.jar |
| 34 | `android.hardware.camera2.CameraExtensionCharacteristics.isPostviewAvailable` | returns-const | android-framework.jar |
| 34 | `android.media.AudioManager.adjustVolumeGroupVolume` | empty | android-framework.jar |
| 34 | `android.media.AudioManager.isVolumeGroupMuted` | returns-const | android-framework.jar |
| 34 | `android.media.MediaRouter2.showSystemOutputSwitcher` | returns-const | android-framework.jar |
| 34 | `android.net.http.HttpEngine$Builder.getDefaultUserAgent` | returns-const | android-framework.jar |
| 34 | `android.net.wifi.WifiManager.getMaxNumberOfChannelsPerNetworkSpecifierRequest` | returns-const | android-framework.jar |
| 34 | `android.net.wifi.WifiManager.isDualBandSimultaneousSupported` | returns-const | android-framework.jar |
| 34 | `android.net.wifi.WifiManager.isTidToLinkMappingNegotiationSupported` | returns-const | android-framework.jar |
| 34 | `android.net.wifi.WifiManager.isTlsMinimumVersionSupported` | returns-const | android-framework.jar |
| 34 | `android.net.wifi.WifiManager.isTlsV13Supported` | returns-const | android-framework.jar |
| 34 | `android.os.PowerManager.isAllowedInLowPowerStandby` | returns-const | android-framework.jar |
| 34 | `android.os.PowerManager.isAllowedInLowPowerStandby` | returns-const | android-framework.jar |
| 34 | `android.os.PowerManager.isExemptFromLowPowerStandby` | returns-const | android-framework.jar |
| 34 | `android.os.Process.isIsolatedUid` | returns-const | android-framework.jar |
| 34 | `android.os.UserManager.isAdminUser` | returns-const | android-framework.jar |
| 34 | `android.os.Vibrator.getQFactor` | returns-const | android-framework.jar |
| 34 | `android.os.Vibrator.getResonantFrequency` | returns-const | android-framework.jar |
| 34 | `android.service.credentials.CredentialProviderService.onBind` | returns-const | android-framework.jar |
| 34 | `android.service.credentials.CredentialProviderService.onCreate` | empty | aosp-app.jar |
| 34 | `android.service.notification.NotificationListenerService.requestUnbind` | empty | android-framework.jar |
| 34 | `android.service.quicksettings.TileService.startActivityAndCollapse` | empty | android-framework.jar |
| 34 | `android.service.voice.VoiceInteractionSession.startAssistantActivity` | empty | android-framework.jar |
| 34 | `android.speech.RecognitionService$Callback.languageDetection` | empty | android-framework.jar |
| 34 | `android.telephony.SubscriptionManager.getAllSubscriptionInfoList` | returns-const | android-framework.jar |
| 34 | `android.telephony.SubscriptionManager.getSubscriptionId` | returns-const | android-framework.jar |
| 34 | `android.telephony.TelephonyManager.getPrimaryImei` | returns-const | android-framework.jar |
| 34 | `android.telephony.TelephonyManager.isPremiumCapabilityAvailableForPurchase` | returns-const | android-framework.jar |
| 35 | `android.adservices.topics.EncryptedTopic.toString` | returns-const | android-framework.jar |
| 35 | `android.app.ActivityManager.addStartInfoTimestamp` | empty | android-framework.jar |
| 35 | `android.app.ActivityManager.getHistoricalProcessStartReasons` | returns-const | android-framework.jar |
| 35 | `android.app.ApplicationStartInfo.getStartupTimestamps` | returns-const | android-framework.jar |
| 35 | `android.app.ApplicationStartInfo.wasForceStopped` | returns-const | android-framework.jar |
| 35 | `android.app.ApplicationStartInfo.writeToParcel` | empty | android-framework.jar |
| 35 | `android.app.GrammaticalInflectionManager.getSystemGrammaticalGender` | returns-const | android-framework.jar |
| 35 | `android.app.NotificationChannel.getVibrationEffect` | returns-const | android-framework.jar |
| 35 | `android.app.NotificationChannel.setVibrationEffect` | empty | android-framework.jar |
| 35 | `android.app.NotificationManager.areAutomaticZenRulesUserManaged` | returns-const | android-framework.jar |
| 35 | `android.app.admin.DevicePolicyManager.isMtePolicyEnforced` | returns-const | android-framework.jar |
| 35 | `android.app.usage.UsageEventsQuery.describeContents` | returns-const | android-framework.jar |
| 35 | `android.appwidget.AppWidgetManager.getWidgetPreview` | returns-const | android-framework.jar |
| 35 | `android.appwidget.AppWidgetManager.removeWidgetPreview` | empty | android-framework.jar |
| 35 | `android.appwidget.AppWidgetManager.setWidgetPreview` | returns-const | android-framework.jar |
| 35 | `android.companion.CompanionDeviceManager.isPermissionTransferUserConsented` | returns-const | android-framework.jar |
| 35 | `android.companion.virtual.VirtualDevice.hasCustomSensorSupport` | returns-const | android-framework.jar |
| 35 | `android.companion.virtual.VirtualDeviceManager.getVirtualDevice` | returns-const | android-framework.jar |
| 35 | `android.content.Intent.isMismatchingFilter` | returns-const | android-framework.jar |
| 35 | `android.content.pm.CrossProfileApps.isManagedProfile` | returns-const | android-framework.jar |
| 35 | `android.content.pm.CrossProfileApps.isProfile` | returns-const | android-framework.jar |
| 35 | `android.content.pm.LauncherApps.getAppMarketActivityIntent` | returns-const | android-framework.jar |
| 35 | `android.content.pm.LauncherApps.getPreInstalledSystemPackages` | returns-const | android-framework.jar |
| 35 | `android.content.pm.LauncherApps.setArchiveCompatibility` | empty | android-framework.jar |
| 35 | `android.content.pm.LauncherUserInfo.describeContents` | returns-const | android-framework.jar |
| 35 | `android.content.pm.PackageInstaller.installPackageArchived` | says-unsupported | android-framework.jar |
| 35 | `android.content.pm.PackageInstaller$SessionInfo.isUnarchival` | returns-const | android-framework.jar |
| 35 | `android.content.pm.PackageManager.getArchivedPackage` | returns-const | android-framework.jar |
| 35 | `android.hardware.OverlayProperties.isCombinationSupported` | returns-const | android-framework.jar |
| 35 | `android.hardware.OverlayProperties.isMixedColorSpacesSupported` | returns-const | android-framework.jar |
| 35 | `android.hardware.OverlayProperties.writeToParcel` | empty | android-framework.jar |
| 35 | `android.hardware.biometrics.PromptContentItemBulletedText.describeContents` | returns-const | android-framework.jar |
| 35 | `android.hardware.biometrics.PromptContentItemPlainText.describeContents` | returns-const | android-framework.jar |
| 35 | `android.hardware.biometrics.PromptContentViewWithMoreOptionsButton.describeContents` | returns-const | android-framework.jar |
| 35 | `android.hardware.biometrics.PromptVerticalListContentView.describeContents` | returns-const | android-framework.jar |
| 35 | `android.hardware.biometrics.PromptVerticalListContentView.getMaxEachItemCharacterNumber` | returns-const | android-framework.jar |
| 35 | `android.hardware.biometrics.PromptVerticalListContentView.getMaxItemCount` | returns-const | android-framework.jar |
| 35 | `android.hardware.camera2.CameraCharacteristics.getAvailableSessionCharacteristicsKeys` | returns-const | android-framework.jar |
| 35 | `android.hardware.camera2.CameraExtensionCharacteristics.get` | returns-const | android-framework.jar |
| 35 | `android.hardware.camera2.CameraExtensionCharacteristics.getKeys` | returns-const | android-framework.jar |
| 35 | `android.hardware.camera2.CameraExtensionSession$ExtensionCaptureCallback.onCaptureFailed` | empty | android-framework.jar |
| 35 | `android.inputmethodservice.InputMethodService.onStartConnectionlessStylusHandwriting` | returns-const | android-framework.jar |
| 35 | `android.media.LoudnessCodecController.addMediaCodec` | returns-const | android-framework.jar |
| 35 | `android.media.LoudnessCodecController.close` | empty | android-framework.jar |
| 35 | `android.media.LoudnessCodecController.removeMediaCodec` | empty | android-framework.jar |
| 35 | `android.media.MediaCodec.detachOutputSurface` | empty | android-framework.jar |
| 35 | `android.media.MediaCodec$Callback.onOutputBuffersAvailable` | empty | android-framework.jar |
| 35 | `android.media.MediaCodec$OutputFrame.getBufferInfos` | returns-const | android-framework.jar |
| 35 | `android.media.MediaRouter2.setRouteVolume` | throws | android-framework.jar |
| 35 | `android.media.MediaRouter2$RoutingController.wasTransferInitiatedBySelf` | returns-const | android-framework.jar |
| 35 | `android.media.metrics.EditingEndedEvent.describeContents` | returns-const | android-framework.jar |
| 35 | `android.media.metrics.MediaItemInfo.describeContents` | returns-const | android-framework.jar |
| 35 | `android.media.midi.MidiUmpDeviceService.getDeviceInfo` | returns-const | android-framework.jar |
| 35 | `android.media.midi.MidiUmpDeviceService.getOutputPortReceivers` | returns-const | android-framework.jar |
| 35 | `android.media.midi.MidiUmpDeviceService.onBind` | returns-const | android-framework.jar |
| 35 | `android.media.midi.MidiUmpDeviceService.onClose` | empty | android-framework.jar |
| 35 | `android.media.midi.MidiUmpDeviceService.onCreate` | empty | aosp-app.jar |
| 35 | `android.media.midi.MidiUmpDeviceService.onDeviceStatusChanged` | empty | android-framework.jar |
| 35 | `android.net.wifi.ScanResult.is80211azNtbResponder` | returns-const | android-framework.jar |
| 35 | `android.net.wifi.ScanResult.isTwtResponder` | returns-const | android-framework.jar |
| 35 | `android.net.wifi.WifiManager.isAggressiveRoamingModeSupported` | returns-const | android-framework.jar |
| 35 | `android.net.wifi.WifiManager.isD2dSupportedWhenInfraStaDisabled` | returns-const | android-framework.jar |
| 35 | `android.net.wifi.WifiManager.isWepSupported` | returns-const | android-framework.jar |
| 35 | `android.net.wifi.WifiManager.isWpaPersonalSupported` | returns-const | android-framework.jar |
| 35 | `android.os.BatteryManager.getStringProperty` | returns-const | android-framework.jar |
| 35 | `android.os.PowerManager.getThermalHeadroomThresholds` | returns-const | android-framework.jar |
| 35 | `android.os.PowerMonitor.describeContents` | returns-const | android-framework.jar |
| 35 | `android.os.ProfilingManager.registerForAllProfilingResults` | empty | android-framework.jar |
| 35 | `android.os.ProfilingManager.requestProfiling` | empty | android-framework.jar |
| 35 | `android.os.ProfilingManager.unregisterForAllProfilingResults` | empty | android-framework.jar |
| 35 | `android.os.ProfilingResult.getErrorCode` | returns-const | android-framework.jar |
| 35 | `android.os.ProfilingResult.getErrorMessage` | says-unsupported | android-framework.jar |
| 35 | `android.os.ProfilingResult.getResultFilePath` | returns-const | android-framework.jar |
| 35 | `android.os.ProfilingResult.getTag` | returns-const | android-framework.jar |
| 35 | `android.service.chooser.ChooserResult.describeContents` | returns-const | android-framework.jar |
| 35 | `android.service.notification.ZenDeviceEffects.describeContents` | returns-const | android-framework.jar |
| 35 | `android.service.persistentdata.PersistentDataBlockManager.isFactoryResetProtectionActive` | returns-const | android-framework.jar |
| 35 | `android.telecom.CallEventCallback.onVideoStateChanged` | empty | android-framework.jar |
| 35 | `android.telephony.TelephonyManager.isDeviceSmsCapable` | returns-const | android-framework.jar |
| 35 | `android.telephony.TelephonyManager.isDeviceVoiceCapable` | returns-const | android-framework.jar |
| 35 | `android.telephony.euicc.EuiccManager.getAvailableMemoryInBytes` | returns-const | android-framework.jar |
| 35 | `android.text.style.LineBreakConfigSpan.describeContents` | returns-const | android-framework.jar |
| 35 | `android.view.InputDevice$ViewBehavior.shouldSmoothScroll` | returns-const | android-framework.jar |
| 35 | `android.view.MotionEvent$PointerCoords.isResampled` | returns-const | android-framework.jar |
| 35 | `android.window.InputTransferToken.describeContents` | returns-const | android-framework.jar |
| 35 | `android.window.TrustedPresentationThresholds.describeContents` | returns-const | android-framework.jar |
| 36 | `android.app.ApplicationStartInfo.getStartComponent` | returns-const | android-framework.jar |
| 36 | `android.app.Notification.hasPromotableCharacteristics` | returns-const | android-framework.jar |
| 36 | `android.app.NotificationManager.canPostPromotedNotifications` | returns-const | android-framework.jar |
| 36 | `android.app.job.JobScheduler.getPendingJobReasonsHistory` | returns-const | android-framework.jar |
| 36 | `android.app.job.PendingJobReasonsInfo.describeContents` | returns-const | android-framework.jar |
| 36 | `android.companion.CompanionDeviceManager.removeBond` | returns-const | android-framework.jar |
| 36 | `android.companion.CompanionDeviceManager.setDeviceId` | empty | android-framework.jar |
| 36 | `android.companion.CompanionDeviceManager$Callback.onFailure` | empty | android-framework.jar |
| 36 | `android.companion.CompanionDeviceService.onDevicePresenceEvent` | empty | android-framework.jar |
| 36 | `android.companion.DeviceId.describeContents` | returns-const | android-framework.jar |
| 36 | `android.companion.DevicePresenceEvent.describeContents` | returns-const | android-framework.jar |
| 36 | `android.companion.ObservingDevicePresenceRequest.describeContents` | returns-const | android-framework.jar |
| 36 | `android.content.Intent.removeLaunchSecurityProtection` | empty | android-framework.jar |
| 36 | `android.content.pm.LauncherApps.getPrivateSpaceSettingsIntent` | returns-const | android-framework.jar |
| 36 | `android.content.pm.LauncherApps$Callback.onUserConfigChanged` | empty | android-framework.jar |
| 36 | `android.content.pm.SharedLibraryInfo.getCertDigests` | returns-const | android-framework.jar |
| 36 | `android.graphics.RuntimeXfermode.setColorUniform` | empty | android-framework.jar |
| 36 | `android.graphics.RuntimeXfermode.setColorUniform` | empty | android-framework.jar |
| 36 | `android.graphics.RuntimeXfermode.setColorUniform` | empty | android-framework.jar |
| 36 | `android.graphics.RuntimeXfermode.setFloatUniform` | empty | android-framework.jar |
| 36 | `android.graphics.RuntimeXfermode.setFloatUniform` | empty | android-framework.jar |
| 36 | `android.graphics.RuntimeXfermode.setFloatUniform` | empty | android-framework.jar |
| 36 | `android.graphics.RuntimeXfermode.setFloatUniform` | empty | android-framework.jar |
| 36 | `android.graphics.RuntimeXfermode.setFloatUniform` | empty | android-framework.jar |
| 36 | `android.graphics.RuntimeXfermode.setInputColorFilter` | empty | android-framework.jar |
| 36 | `android.graphics.RuntimeXfermode.setInputShader` | empty | android-framework.jar |
| 36 | `android.graphics.RuntimeXfermode.setInputXfermode` | empty | android-framework.jar |
| 36 | `android.graphics.RuntimeXfermode.setIntUniform` | empty | android-framework.jar |
| 36 | `android.graphics.RuntimeXfermode.setIntUniform` | empty | android-framework.jar |
| 36 | `android.graphics.RuntimeXfermode.setIntUniform` | empty | android-framework.jar |
| 36 | `android.graphics.RuntimeXfermode.setIntUniform` | empty | android-framework.jar |
| 36 | `android.graphics.RuntimeXfermode.setIntUniform` | empty | android-framework.jar |
| 36 | `android.hardware.OverlayProperties.getLutProperties` | returns-const | android-framework.jar |
| 36 | `android.inputmethodservice.AbstractInputMethodService.onShouldVerifyKeyEvent` | returns-const | android-framework.jar |
| 36 | `android.inputmethodservice.InputMethodService.onCustomImeSwitcherButtonRequestedVisible` | empty | android-framework.jar |
| 36 | `android.media.MediaCodec$Callback.onMetricsFlushed` | empty | android-framework.jar |
| 36 | `android.media.MediaCodecInfo.getSecurityModel` | returns-const | android-framework.jar |
| 36 | `android.media.Spatializer.getSpatializedChannelMasks` | returns-const | android-framework.jar |
| 36 | `android.os.IBinder.addFrozenStateChangeCallback` | throws | android-framework.jar |
| 36 | `android.os.IBinder.removeFrozenStateChangeCallback` | throws | android-framework.jar |
| 36 | `android.os.ProfilingManager.addProfilingTriggers` | empty | android-framework.jar |
| 36 | `android.os.ProfilingManager.clearProfilingTriggers` | empty | android-framework.jar |
| 36 | `android.os.ProfilingManager.removeProfilingTriggersByType` | empty | android-framework.jar |
| 36 | `android.os.ProfilingResult.getTriggerType` | returns-const | android-framework.jar |
| 36 | `android.os.Vibrator.areEnvelopeEffectsSupported` | returns-const | android-framework.jar |
| 36 | `android.os.Vibrator.getFrequencyProfile` | returns-const | android-framework.jar |
| 36 | `android.os.health.SystemHealthManager.getCpuHeadroom` | throws | android-framework.jar |
| 36 | `android.os.health.SystemHealthManager.getCpuHeadroomCalculationWindowRange` | throws | android-framework.jar |
| 36 | `android.os.health.SystemHealthManager.getCpuHeadroomMinIntervalMillis` | throws | android-framework.jar |
| 36 | `android.os.health.SystemHealthManager.getGpuHeadroom` | throws | android-framework.jar |
| 36 | `android.os.health.SystemHealthManager.getGpuHeadroomCalculationWindowRange` | throws | android-framework.jar |
| 36 | `android.os.health.SystemHealthManager.getGpuHeadroomMinIntervalMillis` | throws | android-framework.jar |
| 36 | `android.os.health.SystemHealthManager.getMaxCpuHeadroomTidsSize` | throws | android-framework.jar |
| 36 | `android.os.vibrator.VibratorEnvelopeEffectInfo.describeContents` | returns-const | android-framework.jar |
| 36 | `android.security.NetworkSecurityPolicy.isCertificateTransparencyVerificationRequired` | returns-const | android-framework.jar |
| 36 | `android.security.advancedprotection.AdvancedProtectionManager.isAdvancedProtectionEnabled` | returns-const | android-framework.jar |
| 36 | `android.service.autofill.AutofillService.onSessionDestroyed` | empty | android-framework.jar |
| 36 | `android.service.notification.NotificationListenerService.createConversationNotificationChannelForPackage` | returns-const | android-framework.jar |
| 36 | `android.service.quickaccesswallet.QuickAccessWalletService.getGestureTargetActivityPendingIntent` | returns-const | android-framework.jar |
| 36 | `android.service.settings.preferences.GetValueRequest.describeContents` | returns-const | android-framework.jar |
| 36 | `android.service.settings.preferences.GetValueResult.describeContents` | returns-const | android-framework.jar |
| 36 | `android.service.settings.preferences.MetadataRequest.describeContents` | returns-const | android-framework.jar |
| 36 | `android.service.settings.preferences.MetadataRequest.writeToParcel` | empty | android-framework.jar |
| 36 | `android.service.settings.preferences.MetadataResult.describeContents` | returns-const | android-framework.jar |
| 36 | `android.service.settings.preferences.SetValueRequest.describeContents` | returns-const | android-framework.jar |
| 36 | `android.service.settings.preferences.SetValueResult.describeContents` | returns-const | android-framework.jar |
| 36 | `android.service.settings.preferences.SettingsPreferenceMetadata.describeContents` | returns-const | android-framework.jar |
| 36 | `android.service.settings.preferences.SettingsPreferenceService.onBind` | returns-const | android-framework.jar |
| 36 | `android.service.settings.preferences.SettingsPreferenceValue.describeContents` | returns-const | android-framework.jar |
| 36 | `android.telephony.SmsMessage.getRecipientAddress` | returns-const | android-framework.jar |
| 36 | `android.text.style.NoWritingToolsSpan.describeContents` | returns-const | android-framework.jar |
| 36 | `android.text.style.NoWritingToolsSpan.toString` | returns-const | android-framework.jar |
| 36 | `android.view.SurfaceControl$OnJankDataListenerRegistration.flush` | empty | android-framework.jar |
| 36 | `android.view.SurfaceControl$OnJankDataListenerRegistration.removeAfter` | empty | android-framework.jar |

## What the apps' code uses that fails or reaches a stub

| Problem | Member | Level | Apps | Called from |
|---|---|---|---|---|
| no-class | `android.webkit.WebViewFactory.getLoadedPackageInfo` | hidden | name.boyle.chris.sgtpuzzles, org.jellyfin.mobile | androidx.webkit |
| no-class | `android.support.v4.app.Fragment.<init>` | hidden | dev.lonami.klooni | com.badlogic.gdx |
| no-class | `android.support.v4.app.Fragment.getParentFragment` | hidden | dev.lonami.klooni | com.badlogic.gdx |
| no-class | `android.support.v4.app.Fragment.isRemoving` | hidden | dev.lonami.klooni | com.badlogic.gdx |
| no-class | `android.support.v4.app.Fragment.onActivityResult` | hidden | dev.lonami.klooni | com.badlogic.gdx |
| no-class | `android.support.v4.app.Fragment.onAttach` | hidden | dev.lonami.klooni | com.badlogic.gdx |
| no-class | `android.support.v4.app.Fragment.onConfigurationChanged` | hidden | dev.lonami.klooni | com.badlogic.gdx |
| no-class | `android.support.v4.app.Fragment.onDetach` | hidden | dev.lonami.klooni | com.badlogic.gdx |
| no-class | `android.support.v4.app.Fragment.onPause` | hidden | dev.lonami.klooni | com.badlogic.gdx |
| no-class | `android.support.v4.app.Fragment.onResume` | hidden | dev.lonami.klooni | com.badlogic.gdx |
| no-class | `android.support.v4.app.FragmentActivity.getFilesDir` | hidden | dev.lonami.klooni | com.badlogic.gdx |
| no-class | `android.support.v4.app.FragmentActivity.getSharedPreferences` | hidden | dev.lonami.klooni | com.badlogic.gdx |
| no-class | `android.support.v4.app.FragmentActivity.getWindow` | hidden | dev.lonami.klooni | com.badlogic.gdx |
| no-class | `android.support.v4.app.FragmentActivity.isFinishing` | hidden | dev.lonami.klooni | com.badlogic.gdx |
| no-class | `android.support.v4.app.FragmentActivity.runOnUiThread` | hidden | dev.lonami.klooni | com.badlogic.gdx |
| no-class | `android.webkit.WebViewFactory.getWebViewPackageName` | hidden | org.jellyfin.mobile | org.jellyfin.mobile |
| no-class | `android.webkit.WebViewUpdateService.getCurrentWebViewPackageName` | hidden | org.jellyfin.mobile | org.jellyfin.mobile |
| no-class | `com.android.installreferrer.api.InstallReferrerClient.-` | hidden | org.jitsi.meet | com.learnium.RNDeviceInfo |
| no-class | `com.android.installreferrer.api.InstallReferrerStateListener.-` | hidden | org.jitsi.meet | com.learnium.RNDeviceInfo |
| no-class | `com.android.installreferrer.api.ReferrerDetails.-` | hidden | org.jitsi.meet | com.learnium.RNDeviceInfo |
| no-class | `com.android.okhttp.internal.http.HttpTransport$ChunkedInputStream.-` | hidden | org.jitsi.meet | androidx.media3.datasource |
| no-class | `com.android.okhttp.internal.http.HttpTransport$FixedLengthInputStream.unexpectedEndOfInput` | hidden | org.jitsi.meet | androidx.media3.datasource |
| no-method | `android.view.accessibility.AccessibilityNodeInfo.getChecked` | 36 | name.boyle.chris.sgtpuzzles, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile | H1, androidx.core.view |
| no-method | `android.view.accessibility.AccessibilityNodeInfo.getExpandedState` | 36 | name.boyle.chris.sgtpuzzles, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile | H1, androidx.core.view |
| no-method | `android.view.accessibility.AccessibilityNodeInfo.getSupplementalDescription` | 36 | name.boyle.chris.sgtpuzzles, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile | H1, androidx.core.view |
| no-method | `android.view.accessibility.AccessibilityNodeInfo.isFieldRequired` | 36 | name.boyle.chris.sgtpuzzles, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile | H1, androidx.core.view |
| no-method | `android.window.BackEvent.getFrameTimeMillis` | 36 | org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile | L2 |
| no-method | `android.app.ActivityOptions.setAllowPassThroughOnTouchOutside` | 36 | com.jerboa | androidx.browser.customtabs |
| no-method | `android.view.accessibility.AccessibilityNodeInfo.addLabeledBy` | 36 | name.boyle.chris.sgtpuzzles | androidx.core.view |
| no-field | `android.view.accessibility.AccessibilityNodeInfo$AccessibilityAction.ACTION_SET_EXTENDED_SELECTION` | hidden | org.fossify.gallery |  |
| no-member | `android.graphics.FontFamily.addFontWeightStyle` | hidden | com.jerboa, name.boyle.chris.sgtpuzzles, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile, org.jitsi.meet, org.moire.opensudoku, org.ppsspp.ppsspp | androidx.core.graphics, x1 |
| no-member | `android.app.ActivityThread.mMainThread` | hidden | com.jerboa, name.boyle.chris.sgtpuzzles, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile, org.moire.opensudoku | androidx.core.app, u1 |
| no-member | `android.graphics.Insets.getOpticalInsets` | 29 | name.boyle.chris.sgtpuzzles, org.fossify.gallery, org.fossify.notes, org.jitsi.meet, org.moire.opensudoku | androidx.appcompat.widget, p |
| no-member | `android.view.DisplayInfo.getDisplayInfo` | hidden | com.jerboa, org.fossify.gallery, org.fossify.notes, org.moire.opensudoku | J0, androidx.window.layout |
| stub-empty | `android.media.AudioManager.playSoundEffect` | 1 | com.jerboa, name.boyle.chris.sgtpuzzles, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile, org.jitsi.meet, org.moire.opensudoku, org.ppsspp.ppsspp | androidx.appcompat.app, com.facebook.react, j |
| stub-returns-const | `android.os.PowerManager.isPowerSaveMode` | 21 | com.jerboa, name.boyle.chris.sgtpuzzles, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile, org.jitsi.meet, org.moire.opensudoku, org.ppsspp.ppsspp | androidx.activity, androidx.appcompat.app, com.learnium.RNDeviceInfo, j, kotlin.uuid |
| stub-empty | `android.os.StrictMode.setThreadPolicy` | 9 | com.jerboa, name.boyle.chris.sgtpuzzles, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile, org.jitsi.meet, org.moire.opensudoku, org.ppsspp.ppsspp | D1, I2, M2, androidx.core.graphics, androidx.core.os, com.facebook.soloader, kotlin, w4 |
| stub-empty | `android.os.Trace.beginSection` | 18 | com.jerboa, name.boyle.chris.sgtpuzzles, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile, org.jitsi.meet, org.moire.opensudoku, org.ppsspp.ppsspp | A, A5, G, J0, Q0, Q1, T, Y2, androidx.activity, androidx.activity.compose, androidx.compos |
| stub-empty | `android.os.Trace.endSection` | 18 | com.jerboa, name.boyle.chris.sgtpuzzles, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile, org.jitsi.meet, org.moire.opensudoku, org.ppsspp.ppsspp | A, A5, D1, G, J0, Q0, T, Y2, androidx.activity, androidx.activity.compose, androidx.compos |
| stub-empty | `android.view.MotionEvent.recycle` | 1 | com.jerboa, name.boyle.chris.sgtpuzzles, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile, org.jitsi.meet, org.moire.opensudoku, org.ppsspp.ppsspp | D2, H, I0, J0, androidx.appcompat.widget, androidx.compose.ui, androidx.coordinatorlay |
| stub-returns-const | `android.app.UiModeManager.getCurrentModeType` | 8 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet, org.ppsspp.ppsspp, se.voxelmanip.tensy | R4, T, androidx.core.view, androidx.media3.common, com.facebook.react, org.libsdl.app |
| stub-empty | `android.media.MediaDrm.release` | 18 | com.jerboa, name.boyle.chris.sgtpuzzles, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile, org.jitsi.meet | A, V1, androidx.compose.ui, androidx.media3.exoplayer, com.bumptech.glide, com.jerboa |
| stub-empty | `android.media.AudioManager.unregisterAudioDeviceCallback` | 23 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet, se.voxelmanip.tensy | androidx.media3.exoplayer, org.jitsi.meet, org.libsdl.app |
| stub-empty | `android.view.DisplayListCanvas.drawRenderNode` | hidden | com.jerboa, name.boyle.chris.sgtpuzzles, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile | androidx.compose.ui, t0 |
| stub-empty | `android.view.RenderNode.end` | hidden | com.jerboa, name.boyle.chris.sgtpuzzles, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile | androidx.compose.ui, t0 |
| stub-empty | `android.database.sqlite.SQLiteDatabase.setMaxSqlCacheSize` | 11 | com.jerboa, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile | androidx.sqlite.db, u2 |
| stub-empty | `android.icu.text.DateFormat.setContext` | 24 | name.boyle.chris.sgtpuzzles, org.fossify.notes, org.jitsi.meet, org.moire.opensudoku | androidx.core.os, com.google.android |
| stub-returns-const | `android.media.AudioManager.abandonAudioFocusRequest` | 26 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.media3.common, androidx.media3.exoplayer |
| stub-returns-const | `android.media.AudioManager.getDirectProfilesForAttributes` | 33 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | c0, com.jerboa |
| stub-returns-const | `android.media.AudioManager.getStreamMaxVolume` | 1 | org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet, org.moire.opensudoku | androidx.media3.exoplayer, com.zmxv.RNSound, org.fossify.gallery, org.jellyfin.mobi |
| stub-returns-const | `android.media.AudioManager.requestAudioFocus` | 26 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.media3.common, androidx.media3.exoplayer, org.jitsi.meet |
| stub-empty | `android.media.AudioTrack.setOffloadDelayPadding` | 29 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.compose.ui, c0 |
| stub-empty | `android.media.AudioTrack.setOffloadEndOfStream` | 29 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.compose.ui, c0 |
| stub-empty | `android.media.MediaCodec.setOnFrameRenderedListener` | 23 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.compose.ui, androidx.loader.app, androidx.media3.exoplayer |
| stub-returns-const | `android.media.MediaCodecInfo$CodecCapabilities.isFeatureRequired` | 21 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.media3.exoplayer |
| stub-returns-const | `android.media.MediaCodecInfo$CodecCapabilities.isFeatureSupported` | 19 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.media3.exoplayer, org.webrtc |
| stub-empty | `android.media.Spatializer.addOnSpatializerStateChangedListener` | 32 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.media3.common, r0 |
| stub-returns-const | `android.media.Spatializer.canBeSpatialized` | 32 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.media3.common, r0 |
| stub-returns-const | `android.media.Spatializer.getImmersiveAudioLevel` | 32 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.media3.common, r0 |
| stub-returns-const | `android.media.Spatializer.isAvailable` | 32 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.media3.common, r0 |
| stub-returns-const | `android.media.Spatializer.isEnabled` | 32 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.media3.common, r0 |
| stub-empty | `android.media.Spatializer.removeOnSpatializerStateChangedListener` | 32 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.media3.common, r0 |
| stub-empty | `android.media.metrics.PlaybackSession.reportNetworkEvent` | 31 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.media3.common, b0 |
| stub-empty | `android.media.metrics.PlaybackSession.reportPlaybackErrorEvent` | 31 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.media3.common, b0 |
| stub-empty | `android.media.metrics.PlaybackSession.reportPlaybackMetrics` | 31 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.media3.exoplayer, b0 |
| stub-empty | `android.media.metrics.PlaybackSession.reportPlaybackStateEvent` | 31 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.media3.exoplayer, b0 |
| stub-empty | `android.media.metrics.PlaybackSession.reportTrackChangeEvent` | 31 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | androidx.media3.exoplayer, b0 |
| stub-returns-const | `android.net.NetworkInfo.getSubtype` | 3 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | T, W4, androidx.media3.ui, org.webrtc |
| stub-returns-const | `android.os.StatFs.getBlockSizeLong` | 18 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | coil, com.learnium.RNDeviceInfo, g2, org.fossify.commons |
| stub-empty | `android.os.StrictMode.setVmPolicy` | 9 | com.jerboa, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile | J0, androidx.compose.ui |
| stub-empty | `android.os.Trace.setCounter` | 29 | com.jerboa, org.jellyfin.mobile, org.jitsi.meet, org.ppsspp.ppsspp | androidx.core.graphics, androidx.core.os, l1, org.webrtc |
| stub-empty | `android.telephony.TelephonyManager.unregisterTelephonyCallback` | 31 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile, org.jitsi.meet | T, androidx.media3.common |
| stub-returns-const | `android.view.MotionEvent.getClassification` | 29 | com.jerboa, org.fossify.gallery, org.fossify.notes, org.jellyfin.mobile | C0, androidx.compose.ui, androidx.core.graphics |
| stub-returns-const | `android.content.pm.ShortcutManager.isRequestPinShortcutSupported` | 26 | org.fossify.gallery, org.fossify.notes, org.ppsspp.ppsspp | A5, androidx.core.graphics, org.fossify.gallery |
| stub-returns-const | `android.content.pm.ShortcutManager.requestPinShortcut` | 26 | org.fossify.gallery, org.fossify.notes, org.ppsspp.ppsspp | A5, androidx.core.graphics, org.fossify.gallery |
| stub-returns-const | `android.hardware.fingerprint.FingerprintManager.hasEnrolledFingerprints` | 23 | org.fossify.gallery, org.fossify.notes, org.ppsspp.ppsspp | androidx.core.os, com.github.ajalt, o1, s |
| stub-returns-const | `android.hardware.fingerprint.FingerprintManager.isHardwareDetected` | 23 | org.fossify.gallery, org.fossify.notes, org.ppsspp.ppsspp | androidx.core.os, com.github.ajalt, o1, s |
| stub-returns-const | `android.hardware.usb.UsbManager.getDeviceList` | 12 | org.fossify.gallery, org.fossify.notes, se.voxelmanip.tensy | o5, org.fossify.commons, org.libsdl.app |
| stub-returns-const | `android.media.AudioManager.abandonAudioFocus` | 8 | org.jellyfin.mobile, org.jitsi.meet, org.ppsspp.ppsspp | androidx.media3.exoplayer, com.brentvatne.exoplayer, com.zmxv.RNSound, org.jitsi.me |
| stub-returns-const | `android.media.AudioManager.requestAudioFocus` | 8 | org.jellyfin.mobile, org.jitsi.meet, org.ppsspp.ppsspp | androidx.media3.exoplayer, com.brentvatne.exoplayer, com.zmxv.RNSound, org.ppsspp.p |
| stub-returns-const | `android.media.LoudnessCodecController.addMediaCodec` | 35 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile | androidx.media3.exoplayer |
| stub-empty | `android.media.LoudnessCodecController.close` | 35 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile | androidx.media3.exoplayer |
| stub-empty | `android.media.LoudnessCodecController.removeMediaCodec` | 35 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile | androidx.media3.exoplayer |
| stub-empty | `android.media.MediaCodec.detachOutputSurface` | 35 | com.jerboa, org.fossify.gallery, org.jellyfin.mobile | androidx.media3.exoplayer |
| stub-empty | `android.net.TrafficStats.clearThreadStatsTag` | 14 | com.jerboa, org.fossify.gallery, org.ppsspp.ppsspp | androidx.core.os, androidx.media3.datasource |
| stub-empty | `android.net.TrafficStats.setThreadStatsTag` | 14 | com.jerboa, org.fossify.gallery, org.ppsspp.ppsspp | androidx.core.os, androidx.media3.datasource, com.jerboa.api |
| stub-empty | `android.net.ssl.SSLSockets.setUseSessionTickets` | 29 | com.jerboa, org.jellyfin.mobile, org.jitsi.meet | Y5, androidx.core.graphics |
| stub-empty | `android.os.Trace.beginAsyncSection` | 29 | org.jellyfin.mobile, org.jitsi.meet, org.ppsspp.ppsspp | androidx.core.os, l1 |
| stub-empty | `android.os.Trace.endAsyncSection` | 29 | org.jellyfin.mobile, org.jitsi.meet, org.ppsspp.ppsspp | androidx.core.os, l1 |
| stub-returns-const | `android.os.UserManager.isUserUnlocked` | 24 | org.fossify.gallery, org.jellyfin.mobile, org.ppsspp.ppsspp | androidx.core.os, org.fossify.commons |
| stub-empty | `android.os.Vibrator.vibrate` | 26 | dev.lonami.klooni, org.jitsi.meet, se.voxelmanip.tensy | com.badlogic.gdx, com.facebook.react, org.libsdl.app |
| stub-returns-const | `android.os.ext.SdkExtensions.getExtensionVersion` | 30 | org.fossify.gallery, org.fossify.notes, org.jitsi.meet | G1, androidx.core.os |
| stub-returns-const | `android.security.NetworkSecurityPolicy.isCleartextTrafficPermitted` | 24 | com.jerboa, org.jellyfin.mobile, org.jitsi.meet | X5, okhttp3.internal.platform |
| stub-returns-const | `android.app.ActivityManager.getHistoricalProcessExitReasons` | 30 | com.jerboa, org.jellyfin.mobile | coil.util |
| stub-returns-const | `android.app.NotificationManager.getImportance` | 24 | org.jitsi.meet, org.ppsspp.ppsspp | androidx.core.app |
| stub-returns-const | `android.app.role.RoleManager.isRoleAvailable` | 29 | org.fossify.gallery, org.fossify.notes | U0 |
| stub-returns-const | `android.app.role.RoleManager.isRoleHeld` | 29 | org.fossify.gallery, org.fossify.notes | U0 |
| stub-empty | `android.appwidget.AppWidgetManager.updateAppWidget` | 3 | org.fossify.gallery, org.fossify.notes | B5, org.fossify.gallery, u5 |
| stub-returns-const | `android.media.AudioDeviceInfo.getAudioDescriptors` | 31 | com.jerboa, org.fossify.gallery | coil.decode |
| stub-returns-const | `android.media.AudioManager.getStreamMinVolume` | 28 | org.jellyfin.mobile, org.jitsi.meet | a0, org.jellyfin.mobile |
| stub-returns-const | `android.media.AudioManager.isStreamMute` | 23 | org.jitsi.meet, org.moire.opensudoku | androidx.media3.exoplayer, org.moire.opensudoku, org.webrtc.audio |
| stub-returns-const | `android.media.AudioManager.isVolumeFixed` | 21 | org.jellyfin.mobile, org.jitsi.meet | org.jellyfin.mobile, org.webrtc.audio |
| stub-empty | `android.media.MediaCodec.subscribeToVendorParameters` | 31 | com.jerboa, org.fossify.gallery | coil.decode |
| stub-empty | `android.media.MediaCodec.unsubscribeFromVendorParameters` | 31 | com.jerboa, org.fossify.gallery | coil.decode |
| stub-empty | `android.media.MediaDrm.setOnEventListener` | 18 | org.jellyfin.mobile, org.jitsi.meet | androidx.media3.exoplayer |
| stub-empty | `android.media.MediaPlayer.setAudioStreamType` | 1 | org.fossify.gallery, org.jitsi.meet | com.zmxv.RNSound, org.fossify.commons |
| stub-returns-const | `android.media.Spatializer.getSpatializedChannelMasks` | 36 | com.jerboa, org.fossify.gallery | androidx.media3.exoplayer |
| stub-returns-const | `android.net.ConnectivityManager.getRestrictBackgroundStatus` | 24 | com.jerboa, org.ppsspp.ppsspp | androidx.core.os, com.jerboa |
| stub-empty | `android.net.wifi.WifiManager$WifiLock.acquire` | 1 | org.jellyfin.mobile, org.jitsi.meet | androidx.media3.exoplayer |
| stub-empty | `android.net.wifi.WifiManager$WifiLock.release` | 1 | org.jellyfin.mobile, org.jitsi.meet | androidx.media3.exoplayer |
| stub-empty | `android.net.wifi.WifiManager$WifiLock.setReferenceCounted` | 1 | org.jellyfin.mobile, org.jitsi.meet | androidx.media3.exoplayer |
| stub-empty | `android.opengl.GLSurfaceView.setPreserveEGLContextOnPause` | 11 | com.jerboa, org.jellyfin.mobile | androidx.media3.exoplayer |
| stub-returns-const | `android.os.Environment.getExternalStorageState` | 1 | dev.lonami.klooni, org.ppsspp.ppsspp | androidx.core.os, com.badlogic.gdx, org.ppsspp.ppsspp |
| stub-returns-const | `android.os.PowerManager.isIgnoringBatteryOptimizations` | 23 | com.jerboa, org.jellyfin.mobile | kotlin.uuid, org.jellyfin.mobile |
| stub-returns-const | `android.os.PowerManager.isSustainedPerformanceModeSupported` | 24 | com.jerboa, org.ppsspp.ppsspp | kotlin.uuid, org.ppsspp.ppsspp |
| stub-empty | `android.os.Vibrator.vibrate` | 1 | dev.lonami.klooni, se.voxelmanip.tensy | com.badlogic.gdx, org.libsdl.app |
| stub-returns-const | `android.provider.MediaStore$Images$Thumbnails.getThumbnail` | 5 | org.billthefarmer.editor, org.fossify.gallery | org.billthefarmer.editor |
| stub-returns-const | `android.provider.MediaStore$Video$Thumbnails.getThumbnail` | 5 | org.billthefarmer.editor, org.fossify.gallery | org.billthefarmer.editor |
| stub-returns-const | `android.telecom.TelecomManager.getDefaultDialerPackage` | 23 | org.fossify.gallery, org.fossify.notes | com.bumptech.glide, org.fossify.commons |
| stub-empty | `android.view.RenderNode.destroyDisplayListData` | hidden | name.boyle.chris.sgtpuzzles, org.jellyfin.mobile | androidx.compose.ui |
| stub-empty | `android.animation.AnimatorSet.reverse` | 26 | org.jellyfin.mobile |  |
| stub-empty | `android.animation.AnimatorSet.setCurrentPlayTime` | 26 | org.jellyfin.mobile |  |
| stub-empty | `android.animation.AnimatorSet.setTarget` | 11 | org.jellyfin.mobile | androidx.fragment.app |
| stub-returns-const | `android.animation.ValueAnimator.getFrameDelay` | 11 | org.ppsspp.ppsspp | androidx.core.view |
| stub-returns-const | `android.app.ActivityManager.getRunningServices` | 1 | com.jerboa | kotlin.uuid |
| stub-returns-const | `android.app.ActivityManager.isUserAMonkey` | 8 | org.jitsi.meet | com.facebook.react |
| stub-returns-const | `android.app.AlarmManager.canScheduleExactAlarms` | 31 | org.fossify.notes | q3 |
| stub-returns-const | `android.app.NotificationManager.canUseFullScreenIntent` | 34 | org.fossify.gallery |  |
| stub-empty | `android.app.RemoteInput.addDataResultToIntent` | 26 | org.ppsspp.ppsspp | androidx.core.app |
| stub-returns-const | `android.app.RemoteInput.getDataResultsFromIntent` | 26 | org.ppsspp.ppsspp | androidx.core.app |
| stub-returns-const | `android.app.RemoteInput.getResultsSource` | 28 | org.ppsspp.ppsspp | androidx.core.app |
| stub-empty | `android.app.RemoteInput.setResultsSource` | 28 | org.ppsspp.ppsspp | androidx.core.app |
| stub-returns-const | `android.app.WallpaperManager.getDesiredMinimumHeight` | 5 | org.fossify.gallery | org.fossify.gallery |
| stub-returns-const | `android.app.WallpaperManager.getDesiredMinimumWidth` | 5 | org.fossify.gallery | org.fossify.gallery |
| stub-empty | `android.appwidget.AppWidgetManager.notifyAppWidgetViewDataChanged` | 11 | org.fossify.notes | B5 |
| stub-returns-const | `android.bluetooth.BluetoothManager.getConnectedDevices` | 18 | se.voxelmanip.tensy | org.libsdl.app |
| stub-returns-const | `android.bluetooth.BluetoothManager.getConnectionState` | 18 | se.voxelmanip.tensy | org.libsdl.app |
| stub-returns-const | `android.content.RestrictionsManager.getManifestRestrictions` | 21 | org.jitsi.meet | org.jitsi.meet |
| stub-returns-const | `android.content.pm.ShortcutManager.addDynamicShortcuts` | 25 | org.ppsspp.ppsspp | androidx.core.graphics |
| stub-returns-const | `android.content.pm.ShortcutManager.getDynamicShortcuts` | 25 | org.ppsspp.ppsspp | androidx.core.graphics |
| stub-returns-const | `android.content.pm.ShortcutManager.getMaxShortcutCountPerActivity` | 25 | org.ppsspp.ppsspp | androidx.core.graphics |
| stub-empty | `android.content.pm.ShortcutManager.removeAllDynamicShortcuts` | 25 | org.ppsspp.ppsspp | androidx.core.graphics |
| stub-empty | `android.content.pm.ShortcutManager.removeDynamicShortcuts` | 25 | org.ppsspp.ppsspp | androidx.core.graphics |
| stub-returns-const | `android.content.pm.ShortcutManager.setDynamicShortcuts` | 25 | org.fossify.notes | org.fossify.notes |
| stub-returns-const | `android.content.pm.ShortcutManager.updateShortcuts` | 25 | org.ppsspp.ppsspp | androidx.core.graphics |
| stub-returns-const | `android.hardware.ConsumerIrManager.getCarrierFrequencies` | 19 | org.ppsspp.ppsspp | org.ppsspp.ppsspp |
| stub-returns-const | `android.hardware.ConsumerIrManager.hasIrEmitter` | 19 | org.ppsspp.ppsspp | org.ppsspp.ppsspp |
| stub-empty | `android.hardware.ConsumerIrManager.transmit` | 19 | org.ppsspp.ppsspp | org.ppsspp.ppsspp |
| stub-empty | `android.hardware.display.VirtualDisplay.resize` | 21 | org.jitsi.meet | org.webrtc |
| stub-returns-const | `android.hardware.usb.UsbDeviceConnection.bulkTransfer` | 12 | se.voxelmanip.tensy | org.libsdl.app |
| stub-returns-const | `android.hardware.usb.UsbDeviceConnection.claimInterface` | 12 | se.voxelmanip.tensy | org.libsdl.app |
| stub-empty | `android.hardware.usb.UsbDeviceConnection.close` | 12 | se.voxelmanip.tensy | org.libsdl.app |
| stub-returns-const | `android.hardware.usb.UsbDeviceConnection.controlTransfer` | 18 | se.voxelmanip.tensy | org.libsdl.app |
| stub-returns-const | `android.hardware.usb.UsbDeviceConnection.releaseInterface` | 12 | se.voxelmanip.tensy | org.libsdl.app |
| stub-returns-const | `android.hardware.usb.UsbManager.hasPermission` | 12 | se.voxelmanip.tensy | org.libsdl.app |
| stub-returns-const | `android.hardware.usb.UsbManager.openDevice` | 12 | se.voxelmanip.tensy | org.libsdl.app |
| stub-returns-const | `android.location.LocationManager.addGpsStatusListener` | 3 | org.ppsspp.ppsspp | org.ppsspp.ppsspp |
| stub-empty | `android.location.LocationManager.removeNmeaListener` | 24 | org.ppsspp.ppsspp | org.ppsspp.ppsspp |
| stub-empty | `android.location.LocationManager.unregisterGnssStatusCallback` | 24 | org.ppsspp.ppsspp | org.ppsspp.ppsspp |
| stub-returns-const | `android.media.AudioDeviceInfo.getProductName` | 23 | se.voxelmanip.tensy | org.libsdl.app |
| stub-returns-const | `android.media.AudioManager.getRingerMode` | 1 | org.moire.opensudoku | org.moire.opensudoku |
| stub-returns-const | `android.media.AudioManager.isBluetoothA2dpOn` | 3 | org.jitsi.meet | com.learnium.RNDeviceInfo |
| stub-returns-const | `android.media.AudioManager.isBluetoothScoOn` | 1 | org.jitsi.meet | org.webrtc.audio |
| stub-returns-const | `android.media.AudioManager.isMicrophoneMute` | 1 | org.jitsi.meet | org.webrtc.audio |
| stub-returns-const | `android.media.AudioManager.isMusicActive` | 1 | org.jitsi.meet | org.webrtc.audio |
| stub-returns-const | `android.media.AudioManager.isSpeakerphoneOn` | 1 | org.jitsi.meet | org.webrtc.audio |
| stub-returns-const | `android.media.AudioManager.isWiredHeadsetOn` | 5 | org.jitsi.meet | com.learnium.RNDeviceInfo |
| stub-empty | `android.media.AudioManager.setBluetoothScoOn` | 1 | org.jitsi.meet | org.jitsi.meet |
| stub-empty | `android.media.AudioManager.setMicrophoneMute` | 1 | org.jitsi.meet | org.jitsi.meet |
| stub-empty | `android.media.AudioManager.setSpeakerphoneOn` | 1 | org.jitsi.meet | com.brentvatne.exoplayer, com.zmxv.RNSound, org.jitsi.meet |
| stub-empty | `android.media.AudioManager.startBluetoothSco` | 8 | org.jitsi.meet | org.jitsi.meet |
| stub-empty | `android.media.AudioManager.stopBluetoothSco` | 8 | org.jitsi.meet | org.jitsi.meet |
| stub-returns-const | `android.media.AudioRecordingConfiguration.getAudioDevice` | 24 | org.jitsi.meet | org.webrtc.audio |
| stub-returns-const | `android.media.ExifInterface.getThumbnail` | 5 | org.jitsi.meet | com.facebook.imagepipeline |
| stub-returns-const | `android.media.MediaDrm.requiresSecureDecoder` | 31 | org.jitsi.meet | f0 |
| stub-empty | `android.media.SoundPool.release` | 1 | dev.lonami.klooni | com.badlogic.gdx |
| stub-returns-const | `android.media.ThumbnailUtils.createVideoThumbnail` | 8 | org.jitsi.meet | com.facebook.imagepipeline |
| stub-returns-const | `android.media.audiofx.AcousticEchoCanceler.create` | 16 | org.jitsi.meet | org.webrtc.audio |
| stub-returns-const | `android.media.audiofx.NoiseSuppressor.create` | 16 | org.jitsi.meet | org.webrtc.audio |
| stub-empty | `android.media.projection.MediaProjection.stop` | 21 | org.jitsi.meet | org.webrtc |
| stub-returns-const | `android.media.projection.MediaProjectionManager.getMediaProjection` | 21 | org.jitsi.meet | org.webrtc |
| stub-returns-const | `android.media.session.MediaController.dispatchMediaButtonEvent` | 21 | org.jellyfin.mobile |  |
| stub-empty | `android.media.session.MediaController.sendCommand` | 21 | org.jellyfin.mobile |  |
| stub-empty | `android.media.session.MediaController$TransportControls.fastForward` | 21 | org.jellyfin.mobile | org.jellyfin.mobile |
| stub-empty | `android.media.session.MediaController$TransportControls.pause` | 21 | org.jellyfin.mobile | org.jellyfin.mobile |
| stub-empty | `android.media.session.MediaController$TransportControls.play` | 21 | org.jellyfin.mobile | org.jellyfin.mobile |
| stub-empty | `android.media.session.MediaController$TransportControls.rewind` | 21 | org.jellyfin.mobile | org.jellyfin.mobile |
| stub-empty | `android.media.session.MediaController$TransportControls.sendCustomAction` | 21 | org.jellyfin.mobile |  |
| stub-empty | `android.media.session.MediaController$TransportControls.skipToNext` | 21 | org.jellyfin.mobile | org.jellyfin.mobile |
| stub-empty | `android.media.session.MediaController$TransportControls.skipToPrevious` | 21 | org.jellyfin.mobile | org.jellyfin.mobile |
| stub-empty | `android.media.session.MediaController$TransportControls.stop` | 21 | org.jellyfin.mobile | org.jellyfin.mobile |
| stub-empty | `android.media.session.MediaSession.setPlaybackToLocal` | 21 | org.jellyfin.mobile | org.jellyfin.mobile |
| stub-empty | `android.media.session.MediaSession.setPlaybackToRemote` | 21 | org.jellyfin.mobile | org.jellyfin.mobile |
| stub-returns-const | `android.media.session.MediaSession$Callback.onMediaButtonEvent` | 21 | org.jellyfin.mobile | org.jellyfin.mobile |
| stub-returns-const | `android.net.NetworkInfo.isRoaming` | 3 | org.jellyfin.mobile |  |
| stub-returns-const | `android.net.TrafficStats.getThreadStatsTag` | 14 | org.ppsspp.ppsspp | androidx.core.os |
| stub-empty | `android.net.TrafficStats.incrementOperationCount` | 14 | org.ppsspp.ppsspp | androidx.core.os |
| stub-empty | `android.net.TrafficStats.incrementOperationCount` | 14 | org.ppsspp.ppsspp | androidx.core.os |
| stub-empty | `android.net.TrafficStats.tagDatagramSocket` | 24 | org.ppsspp.ppsspp | androidx.core.os |
| stub-empty | `android.net.TrafficStats.tagSocket` | 14 | org.ppsspp.ppsspp | androidx.core.os |
| stub-empty | `android.net.TrafficStats.untagDatagramSocket` | 24 | org.ppsspp.ppsspp | androidx.core.os |
| stub-empty | `android.net.TrafficStats.untagSocket` | 14 | org.ppsspp.ppsspp | androidx.core.os |
| stub-returns-const | `android.os.BatteryManager.computeChargeTimeRemaining` | 28 | com.jerboa | com.jerboa.ui |
| stub-returns-const | `android.os.Build.getRadioVersion` | 14 | com.jerboa | kotlin.uuid |
| stub-returns-const | `android.os.Build.getSerial` | 26 | org.jitsi.meet | com.learnium.RNDeviceInfo |
| stub-returns-const | `android.os.Environment.getExternalStorageState` | 21 | org.ppsspp.ppsspp | androidx.core.os |
| stub-returns-const | `android.os.Environment.isExternalStorageEmulated` | 11 | org.ppsspp.ppsspp | org.ppsspp.ppsspp |
| stub-returns-const | `android.os.PowerManager.getCurrentThermalStatus` | 29 | com.jerboa | coil.fetch |
| stub-returns-const | `android.os.PowerManager.getLocationPowerSaveMode` | 28 | com.jerboa | com.jerboa.ui |
| stub-returns-const | `android.os.PowerManager.isDeviceIdleMode` | 23 | com.jerboa | kotlin.uuid |
| stub-returns-const | `android.os.PowerManager.isInteractive` | 20 | com.jerboa | kotlin.uuid |
| stub-empty | `android.os.SharedMemory.unmap` | 27 | org.jitsi.meet | U2 |
| stub-returns-const | `android.os.StatFs.getBlockSize` | 1 | org.jitsi.meet | com.learnium.RNDeviceInfo |
| stub-empty | `android.os.Vibrator.vibrate` | 1 | dev.lonami.klooni | com.badlogic.gdx |
| stub-empty | `android.service.dreams.DreamService.onDreamingStarted` | 17 | dev.lonami.klooni | com.badlogic.gdx |
| stub-empty | `android.service.dreams.DreamService.onDreamingStopped` | 17 | dev.lonami.klooni | com.badlogic.gdx |
| stub-returns-const | `android.service.media.MediaBrowserService.getCurrentBrowserInfo` | 28 | org.jellyfin.mobile |  |
| stub-empty | `android.service.media.MediaBrowserService.notifyChildrenChanged` | 21 | org.jellyfin.mobile |  |
| stub-empty | `android.service.media.MediaBrowserService.notifyChildrenChanged` | 24 | org.jellyfin.mobile |  |
| stub-returns-const | `android.service.media.MediaBrowserService.onBind` | 21 | org.jellyfin.mobile |  |
| stub-empty | `android.telecom.Connection.setAudioRoute` | 26 | org.jitsi.meet | org.jitsi.meet |
| stub-throws | `android.telecom.TelecomManager.placeCall` | 23 | org.jitsi.meet | org.jitsi.meet |
| stub-throws | `android.telecom.TelecomManager.registerPhoneAccount` | 23 | org.jitsi.meet | org.jitsi.meet |
| stub-empty | `android.telecom.TelecomManager.unregisterPhoneAccount` | 23 | org.jitsi.meet | org.jitsi.meet |
| stub-returns-const | `android.telephony.TelephonyManager.getNetworkOperatorName` | 1 | org.jitsi.meet | V4, com.learnium.RNDeviceInfo |
| stub-returns-const | `android.telephony.mbms.ServiceInfo.getNameForLocale` | 28 | org.ppsspp.ppsspp | androidx.core.os |
| stub-returns-const | `android.telephony.mbms.ServiceInfo.getNamedContentLocales` | 28 | org.ppsspp.ppsspp | androidx.core.os |
| stub-empty | `android.view.SurfaceView.onDetachedFromWindow` | 1 | dev.lonami.klooni | com.badlogic.gdx |
| stub-empty | `android.webkit.WebView.clearCache` | 1 | org.jitsi.meet | com.reactnativecommunity.webview |
| stub-empty | `android.webkit.WebView.clearFormData` | 1 | org.jitsi.meet | com.reactnativecommunity.webview |
| stub-empty | `android.webkit.WebView.clearHistory` | 1 | org.jitsi.meet | com.reactnativecommunity.webview |
| stub-returns-const | `android.webkit.WebView.findAddress` | 1 | org.ppsspp.ppsspp | androidx.core.text |
| stub-empty | `android.webkit.WebView.onSizeChanged` | 1 | org.jitsi.meet | com.reactnativecommunity.webview |
| stub-empty | `android.webkit.WebView.setWebContentsDebuggingEnabled` | 19 | org.jitsi.meet | com.reactnativecommunity.webview |

## In-process services: methods with no answer

| Interface | Answered | Unanswered methods |
|---|---:|---|
| `android.app.IActivityClientController` | 21/66 | activityLocalRelaunch, activityRefreshed, activityRelaunched, clearOverrideActivityTransition, convertFromTranslucent, convertToTranslucent, dismissKeyguard, enableTaskLocaleOverride, enterPictureInPictureMode, finishActivityAffinity, finishSubActivity, getActivityTokenBelow, getTaskConfiguration, invalidateHomeTaskSnapshot, isImmersive, isRequestedToLaunchInTaskFragment, isRootVoiceInteraction, overrideActivityTransition, overridePendingTransition, registerRemoteAnimations, releaseActivityInstance, reportActivityFullyDrawn, reportSizeConfigurations, requestCompatCameraControl, requestMultiwindowFullscreen, setAllowCrossUidActivitySwitchFromBelow, setForceSendResultForMediaProjection, setImmersive, setInheritShowWhenLocked, setPictureInPictureParams, setRecentsScreenshotEnabled, setShouldDockBigOverlays, setShowWhenLocked, setTaskDescription, setTurnScreenOn, setVrMode, showAssistFromActivity, showLockTaskEscapeMessage, splashScreenAttached, startLocalVoiceInteraction, startLockTaskModeByToken, stopLocalVoiceInteraction, stopLockTaskModeByToken, toggleFreeformWindowingMode, unregisterRemoteAnimations |
| `android.app.IActivityManager` | 29/249 | addInstrumentationResults, addPackageDependency, addUidToObserver, appNotResponding, appNotRespondingViaProvider, backgroundAllowlistUid, backupAgentCreated, bindBackupAgent, bootAnimationComplete, cancelIntentSender, cancelTaskWindowTransition, checkPermission, checkUriPermissions, clearApplicationUserData, closeSystemDialogs, crashApplicationWithType, crashApplicationWithTypeWithExtras, dumpHeap, dumpHeapFinished, enableAppFreezer, enableFgsNotificationRateLimit, enterSafeMode, finishActivity, finishHeavyWeightApp, finishInstrumentation, forceDelayBroadcastDelivery, forceStopPackage, forceStopPackageEvenWhenStopping, getAllRootTaskInfos, getBackgroundRestrictionExemptionReason, getBugreportWhitelistedPackages, getConfiguration, getContentProviderExternal, getCurrentUser, getCurrentUserId, getDelegatedShellPermissions, getDisplayIdsForStartingVisibleBackgroundUsers, getFocusedRootTaskInfo, getHistoricalProcessExitReasons, getHistoricalProcessStartReasons, getInfoForIntentSender, getIntentForIntentSender, getIntentSender, getIntentSenderWithFeature, getLaunchedFromPackage, getLaunchedFromUid, getLifeMonitor, getLockTaskModeState, getMemoryInfo, getMemoryTrimLevel, getMimeTypeFilterAsync, getMyMemoryState, getPackageProcessState, getProcessLimit, getProcessMemoryInfo, getProcessPss, getProcessesInErrorState, getRecentTasks, getRunningAppProcesses, getRunningExternalApplications, getRunningServiceControlPanel, getRunningUserIds, getServices, getSwitchingFromUserMessage, getSwitchingToUserMessage, getTagForIntentSender, getTaskBounds, getTaskForActivity, getTasks, getUidFrozenState, getUidProcessCapabilities, getUidProcessState, handleApplicationCrash, handleApplicationStrictModeViolation, handleApplicationWtf, handleIncomingUser, hang, holdLock, isAppFreezerEnabled, isAppFreezerSupported, isBackgroundRestricted, isInLockTaskMode, isIntentSenderAnActivity, isIntentSenderTargetedToPackage, isModernBroadcastQueueEnabled, isProcessFrozen, isTopActivityImmersive, isTopOfTask, isUidActive, isUserAMonkey, isUserRunning, isVrModePackageEnabled, killAllBackgroundProcesses, killApplication, killApplicationProcess, killBackgroundProcesses, killPackageDependents, killPids, killProcessesBelowForeground, killProcessesWhenImperceptible, killUid, killUidForPermissionChange, launchBugReportHandlerApp, logFgsApiBegin, logFgsApiEnd, logFgsApiStateChanged, makePackageIdle, moveActivityTaskToBack, moveTaskToFront, moveTaskToRootTask, noteAlarmFinish, noteAlarmStart, noteWakeupAlarm, notifyCleartextNetwork, notifyLockedProfile, openContentUri, performIdleMaintenance, profileControl, queryIntentComponentsForIntentSender, registerForegroundServiceObserver, registerIntentSenderCancelListenerEx, registerProcessObserver, registerStrictModeCallback, registerTaskStackListener, registerUidFrozenStateChangedCallback, registerUidObserver, registerUidObserverForUids, registerUserSwitchObserver, removeApplicationStartInfoCompleteListener, removeContentProviderExternal, removeContentProviderExternalAsUser, removeTask, removeUidFromObserver, requestBugReport, requestBugReportWithDescription, requestFullBugReport, requestInteractiveBugReport, requestInteractiveBugReportWithDescription, requestRemoteBugReport, requestSystemServerHeapDump, requestTelephonyBugReport, requestWifiBugReport, resetAppErrors, resizeTask, restart, restartUserInBackground, resumeAppSwitches, scheduleApplicationInfoChanged, sendIdleJobTrigger, sendIntentSender, setActivityController, setActivityLocusContext, setAgentApp, setAlwaysFinish, setApplicationStartInfoCompleteListener, setDebugApp, setDeterministicUidIdle, setDumpHeapDebugLimit, setFocusedRootTask, setHasTopUi, setPackageScreenCompatMode, setPersistentVrThread, setProcessImportant, setProcessLimit, setProcessMemoryTrimLevel, setProcessStateSummary, setRenderThread, setRequestedOrientation, setStopUserOnSwitch, setTaskResizeable, setUserIsMonkey, shouldServiceTimeOut, showBootMessage, showWaitingForDebugger, shutdown, signalPersistentProcesses, startActivity, startActivityAsUser, startActivityAsUserWithFeature, startActivityFromRecents, startActivityWithFeature, startBinderTracking, startConfirmDeviceCredentialIntent, startDelegateShellPermissionIdentity, startInstrumentation, startProfile, startProfileWithListener, startSystemLockTaskMode, startUserInBackground, startUserInBackgroundVisibleOnDisplay, startUserInBackgroundWithListener, startUserInForegroundWithListener, stopAppForUser, stopAppSwitches, stopBinderTrackingAndDump, stopDelegateShellPermissionIdentity, stopProfile, stopUser, stopUserWithDelayedLocking, suppressResizeConfigChanges, switchUser, unbindBackupAgent, unhandledBack, unlockUser, unlockUser2, unregisterIntentSenderCancelListener, unregisterProcessObserver, unregisterTaskStackListener, unregisterUidFrozenStateChangedCallback, unregisterUidObserver, unregisterUserSwitchObserver, unstableProviderDied, updateConfiguration, updateLockTaskPackages, updateMccMncConfiguration, updatePersistentConfiguration, updatePersistentConfigurationWithAttribution, waitForBroadcastBarrier, waitForBroadcastIdle, waitForNetworkStateUpdate |
| `android.app.IActivityTaskManager` | 3/95 | addAppTask, alwaysShowUnsupportedCompileSdkWarning, cancelRecentsAnimation, cancelTaskWindowTransition, clearLaunchParamsForPackages, detachNavigationBarFromApp, finishVoiceTask, focusTopTask, getAllRootTaskInfos, getAllRootTaskInfosOnDisplay, getAppTaskThumbnailSize, getAppTasks, getAssistContextExtras, getDeviceConfigurationInfo, getFocusedRootTaskInfo, getFrontActivityScreenCompatMode, getLastResumedActivityUserId, getLockTaskModeState, getPackageAskScreenCompat, getPackageScreenCompatMode, getRecentTasks, getRootTaskInfo, getRootTaskInfoOnDisplay, getTaskBounds, getTaskDescription, getTaskDescriptionIcon, getTaskSnapshot, getTasks, getVoiceInteractorPackageName, getWindowOrganizerController, isActivityStartAllowedOnDisplay, isAssistDataAllowedOnCurrentActivity, isInLockTaskMode, isTopActivityImmersive, keyguardGoingAway, moveRootTaskToDisplay, moveTaskToFront, moveTaskToRootTask, onPictureInPictureStateChanged, onSplashScreenViewCopyFinished, registerRemoteAnimationForNextActivityStart, registerRemoteAnimationsForDisplay, registerScreenCaptureObserver, registerTaskStackListener, releaseSomeActivities, removeAllVisibleRecentTasks, removeRootTasksInWindowingModes, removeRootTasksWithActivityTypes, removeTask, reportAssistContextExtras, requestAssistContextExtras, requestAssistDataForTask, requestAutofillData, resizeTask, resumeAppSwitches, setActivityController, setFocusedRootTask, setFocusedTask, setFrontActivityScreenCompatMode, setLockScreenShown, setPackageAskScreenCompat, setPackageScreenCompatMode, setPersistentVrThread, setRunningRemoteTransitionDelegate, setTaskResizeable, setVoiceKeepAwake, setVrThread, startActivityAndWait, startActivityAsCaller, startActivityAsUser, startActivityFromGameSession, startActivityFromRecents, startActivityIntentSender, startActivityWithConfig, startAssistantActivity, startBackNavigation, startDreamActivity, startNextMatchingActivity, startRecentsActivity, startSystemLockTaskMode, startVoiceActivity, stopAppSwitches, stopSystemLockTaskMode, supportsLocalVoiceInteraction, suppressResizeConfigChanges, takeTaskSnapshot, unhandledBack, unregisterScreenCaptureObserver, unregisterTaskStackListener, updateConfiguration, updateLockTaskFeatures, updateLockTaskPackages |
| `android.app.INotificationManager` | 4/148 | addAutomaticZenRule, applyAdjustmentFromAssistant, applyAdjustmentsFromAssistant, applyEnqueuedAdjustmentFromAssistant, applyRestore, areBubblesAllowed, areBubblesEnabled, areChannelsBypassingDnd, areNotificationsEnabled, areNotificationsEnabledForPackage, canNotifyAsPackage, canShowBadge, canUseFullScreenIntent, cancelAllNotifications, cancelNotificationFromListener, cancelNotificationWithTag, cancelNotificationsFromListener, cleanUpCallersAfter, clearData, clearRequestedListenerHints, createConversationNotificationChannelForPackage, createNotificationChannelGroups, createNotificationChannels, createNotificationChannelsForPackage, deleteNotificationChannel, deleteNotificationChannelGroup, deleteNotificationHistoryItem, enqueueNotificationWithTag, getActiveNotifications, getActiveNotificationsFromListener, getActiveNotificationsWithAttribution, getAllowedAssistantAdjustments, getAllowedNotificationAssistant, getAllowedNotificationAssistantForUser, getAppActiveNotifications, getAutomaticZenRule, getBackupPayload, getBlockedChannelCount, getBubblePreferenceForPackage, getConsolidatedNotificationPolicy, getConversationNotificationChannel, getConversations, getConversationsForPackage, getDefaultNotificationAssistant, getDeletedChannelCount, getEffectsSuppressor, getEnabledNotificationListenerPackages, getEnabledNotificationListeners, getHintsFromListener, getHintsFromListenerNoToken, getHistoricalNotifications, getHistoricalNotificationsWithAttribution, getInterruptionFilterFromListener, getListenerFilter, getNotificationChannel, getNotificationChannelForPackage, getNotificationChannelGroup, getNotificationChannelGroupForPackage, getNotificationChannelGroups, getNotificationChannelGroupsForPackage, getNotificationChannelGroupsFromPrivilegedListener, getNotificationChannels, getNotificationChannelsBypassingDnd, getNotificationChannelsForPackage, getNotificationChannelsFromPrivilegedListener, getNotificationDelegate, getNotificationHistory, getNotificationPolicy, getNumNotificationChannelsForPackage, getPackageImportance, getPopulatedNotificationChannelGroupForPackage, getPrivateNotificationsAllowed, getRuleInstanceCount, getSnoozedNotificationsFromListener, getZenMode, getZenModeConfig, getZenRules, hasEnabledNotificationListener, hasSentValidBubble, hasSentValidMsg, hasUserDemotedInvalidMsgApp, isImportanceLocked, isInCall, isInInvalidMsgState, isNotificationAssistantAccessGranted, isNotificationListenerAccessGranted, isNotificationListenerAccessGrantedForUser, isNotificationPolicyAccessGranted, isNotificationPolicyAccessGrantedForPackage, isPackagePaused, isPermissionFixed, isSystemConditionProviderEnabled, matchesCallFilter, migrateNotificationFilter, notifyConditions, onlyHasDefaultChannel, pullStats, registerListener, removeAutomaticZenRule, removeAutomaticZenRules, requestBindListener, requestBindProvider, requestHintsFromListener, requestInterruptionFilterFromListener, requestUnbindListener, requestUnbindListenerComponent, requestUnbindProvider, setAutomaticZenRuleState, setBubblesAllowed, setHideSilentStatusIcons, setInterruptionFilter, setInvalidMsgAppDemoted, setListenerFilter, setNASMigrationDoneAndResetDefault, setNotificationAssistantAccessGranted, setNotificationAssistantAccessGrantedForUser, setNotificationDelegate, setNotificationListenerAccessGranted, setNotificationListenerAccessGrantedForUser, setNotificationPolicy, setNotificationPolicyAccessGranted, setNotificationPolicyAccessGrantedForUser, setNotificationsEnabledForPackage, setNotificationsEnabledWithImportanceLockForPackage, setNotificationsShownFromListener, setOnNotificationPostedTrimFromListener, setPrivateNotificationsAllowed, setShowBadge, setToastRateLimitingEnabled, setZenMode, shouldHideSilentStatusIcons, silenceNotificationSound, snoozeNotificationUntilContextFromListener, snoozeNotificationUntilFromListener, unlockAllNotificationChannels, unlockNotificationChannel, unregisterListener, unsnoozeNotificationFromAssistant, unsnoozeNotificationFromSystemListener, updateAutomaticZenRule, updateNotificationChannelForPackage, updateNotificationChannelFromPrivilegedListener, updateNotificationChannelGroupForPackage, updateNotificationChannelGroupFromPrivilegedListener |
| `android.content.pm.IPackageManager` | 2/206 | activitySupportsIntentAsUser, addCrossProfileIntentFilter, addPermission, addPermissionAsync, addPersistentPreferredActivity, addPreferredActivity, canForwardTo, canPackageQuery, canRequestPackageInstalls, canonicalToCurrentPackageNames, checkPackageStartable, checkPermission, checkSignatures, checkUidPermission, checkUidSignatures, clearApplicationProfileData, clearApplicationUserData, clearCrossProfileIntentFilters, clearPackagePersistentPreferredActivities, clearPackagePreferredActivities, clearPersistentPreferredActivity, currentToCanonicalPackageNames, deleteApplicationCacheFiles, deleteApplicationCacheFilesAsUser, deleteExistingPackageAsUser, deletePackageAsUser, deletePackageVersioned, deletePreloadsFileCache, enterSafeMode, extendVerificationTimeout, findPersistentPreferredActivity, finishPackageInstall, flushPackageRestrictionsAsUser, freeStorage, freeStorageAndNotify, getActivityInfo, getAllIntentFilters, getAllPackages, getAppMetadataFd, getAppOpPermissionPackages, getAppPredictionServicePackageName, getApplicationEnabledSetting, getApplicationHiddenSettingAsUser, getArtManager, getAttentionServicePackageName, getBlockUninstallForUser, getChangedPackages, getComponentEnabledSetting, getDeclaredSharedLibraries, getDefaultAppsBackup, getDefaultTextClassifierPackageName, getDomainVerificationBackup, getFlagsForUid, getHarmfulAppWarning, getHoldLockToken, getHomeActivities, getIncidentReportApproverPackageName, getInitialNonStoppedSystemPackages, getInstallLocation, getInstallReason, getInstallSourceInfo, getInstalledApplications, getInstalledModules, getInstalledPackages, getInstallerPackageName, getInstantAppAndroidId, getInstantAppCookie, getInstantAppIcon, getInstantAppInstallerComponent, getInstantAppResolverComponent, getInstantAppResolverSettingsComponent, getInstantApps, getInstrumentationInfoAsUser, getIntentFilterVerifications, getIntentVerificationStatus, getKeySetByAlias, getLastChosenActivity, getLaunchIntentSenderForPackage, getMimeGroup, getModuleInfo, getMoveStatus, getNameForUid, getNamesForUids, getPackageGids, getPackageInfo, getPackageInfoVersioned, getPackageInstaller, getPackageSizeInfo, getPackageUid, getPackagesHoldingPermissions, getPermissionControllerPackageName, getPermissionGroupInfo, getPersistentApplications, getPreferredActivities, getPreferredActivityBackup, getPrivateFlagsForUid, getPropertyAsUser, getProviderInfo, getReceiverInfo, getRotationResolverPackageName, getRuntimePermissionsVersion, getSdkSandboxPackageName, getServiceInfo, getServicesSystemSharedLibraryPackageName, getSetupWizardPackageName, getSharedLibraries, getSharedSystemSharedLibraryPackageName, getSigningKeySet, getSplashScreenTheme, getSuspendedPackageAppExtras, getSystemAvailableFeatures, getSystemCaptionsServicePackageName, getSystemSharedLibraryNames, getSystemTextClassifierPackageName, getTargetSdkVersion, getUidForSharedUser, getUnsuspendablePackagesForUser, getVerifierDeviceIdentity, getWellbeingPackageName, grantRuntimePermission, hasSigningCertificate, hasSystemFeature, hasSystemUidErrors, hasUidSigningCertificate, holdLock, installExistingPackageAsUser, isAutoRevokeWhitelisted, isDeviceUpgrading, isFirstBoot, isInstantApp, isPackageAvailable, isPackageDeviceAdminOnAnyUser, isPackageSignedByKeySet, isPackageSignedByKeySetExactly, isPackageStateProtected, isPackageSuspendedForUser, isProtectedBroadcast, isSafeMode, isStorageLow, isUidPrivileged, logAppProcessStartIfNeeded, makeProviderVisible, makeUidVisible, movePackage, movePrimaryStorage, notifyDexLoad, notifyPackageUse, notifyPackagesReplacedReceived, overrideLabelAndIcon, performDexOptMode, performDexOptSecondary, queryContentProviders, queryInstrumentationAsUser, queryIntentActivities, queryIntentActivityOptions, queryIntentContentProviders, queryIntentReceivers, queryIntentServices, queryProperty, querySyncProviders, registerDexModule, registerMoveCallback, relinquishUpdateOwnership, removeCrossProfileIntentFilter, removePermission, replacePreferredActivity, requestPackageChecksums, resetApplicationPreferences, resolveContentProvider, resolveIntent, resolveService, restoreDefaultApps, restoreDomainVerification, restoreLabelAndIcon, restorePreferredActivities, sendDeviceCustomizationReadyBroadcast, setApplicationCategoryHint, setApplicationEnabledSetting, setApplicationHiddenSettingAsUser, setBlockUninstallForUser, setComponentEnabledSetting, setComponentEnabledSettings, setDistractingPackageRestrictionsAsUser, setHarmfulAppWarning, setHomeActivity, setInstallLocation, setInstallerPackageName, setInstantAppCookie, setKeepUninstalledPackages, setLastChosenActivity, setMimeGroup, setPackageStoppedState, setPackagesSuspendedAsUser, setRequiredForSystemUser, setRuntimePermissionsVersion, setSplashScreenTheme, setSystemAppHiddenUntilInstalled, setSystemAppInstallState, setUpdateAvailable, unregisterMoveCallback, updateIntentVerificationStatus, verifyIntentFilter, verifyPendingInstall, waitForHandler |
| `android.hardware.display.IDisplayManager` | 5/55 | areUserDisabledHdrTypesAllowed, connectWifiDisplay, createVirtualDisplay, disconnectWifiDisplay, forgetWifiDisplay, getAmbientBrightnessStats, getBrightness, getBrightnessConfigurationForDisplay, getBrightnessConfigurationForUser, getBrightnessEvents, getBrightnessInfo, getDefaultBrightnessConfiguration, getDisplayDecorationSupport, getHdrConversionMode, getHdrConversionModeSetting, getMinimumBrightnessCurve, getOverlaySupport, getRefreshRateSwitchingType, getSupportedHdrOutputTypes, getSystemPreferredDisplayMode, getUserDisabledHdrTypes, getUserPreferredDisplayMode, getWifiDisplayStatus, isMinimalPostProcessingRequested, overrideHdrTypes, pauseWifiDisplay, registerCallback, registerCallbackWithEventMask, releaseVirtualDisplay, renameWifiDisplay, requestColorMode, resizeVirtualDisplay, resumeWifiDisplay, setAreUserDisabledHdrTypesAllowed, setBrightness, setBrightnessConfigurationForDisplay, setBrightnessConfigurationForUser, setDisplayIdToMirror, setHdrConversionMode, setRefreshRateSwitchingType, setShouldAlwaysRespectAppRequestedMode, setTemporaryAutoBrightnessAdjustment, setTemporaryBrightness, setUserDisabledHdrTypes, setUserPreferredDisplayMode, setVirtualDisplayState, setVirtualDisplaySurface, shouldAlwaysRespectAppRequestedMode, startWifiDisplayScan, stopWifiDisplayScan |
| `android.hardware.input.IInputManager` | 7/67 | addKeyboardLayoutForInputDevice, addPortAssociation, addUniqueIdAssociation, cancelCurrentTouch, cancelVibrate, clearAllModifierKeyRemappings, closeLightSession, disableInputDevice, disableSensor, enableInputDevice, enableSensor, flushSensor, getBatteryState, getCurrentKeyboardLayoutForInputDevice, getEnabledKeyboardLayoutsForInputDevice, getHostUsiVersionFromDisplayConfig, getInputDeviceBluetoothAddress, getKeyCodeForKeyLocation, getKeyboardLayout, getKeyboardLayoutForInputDevice, getKeyboardLayoutListForInputDevice, getKeyboardLayouts, getKeyboardLayoutsForInputDevice, getLightState, getLights, getModifierKeyRemapping, getSensorList, getTouchCalibrationForInputDevice, getVelocityTrackerStrategy, getVibratorIds, hasKeys, injectInputEvent, injectInputEventToTarget, isInputDeviceEnabled, isMicMuted, isVibrating, monitorGestureInput, openLightSession, pilferPointers, registerBatteryListener, registerKeyboardBacklightListener, registerSensorListener, registerTabletModeChangedListener, registerVibratorStateListener, remapModifierKey, removeKeyboardLayoutForInputDevice, removePortAssociation, removeUniqueIdAssociation, setCurrentKeyboardLayoutForInputDevice, setKeyboardLayoutForInputDevice, setLightStates, setTouchCalibrationForInputDevice, tryPointerSpeed, unregisterBatteryListener, unregisterKeyboardBacklightListener, unregisterSensorListener, unregisterVibratorStateListener, verifyInputEvent, vibrate, vibrateCombined |
| `android.view.IWindowManager` | 16/144 | addKeyguardLockedStateListener, addShellRoot, addToSurfaceSyncGroup, addWindowToken, attachToDisplayContent, attachWindowContextToDisplayArea, attachWindowContextToWindowToken, captureDisplay, clearForcedDisplayDensityForUser, clearForcedDisplaySize, clearTaskTransitionSpec, clearWindowContentFrameStats, closeSystemDialogs, createInputConsumer, destroyInputConsumer, detachWindowContextFromWindowContainer, disableKeyguard, dismissKeyguard, endProlongedAnimations, exitKeyguardSecurely, freezeDisplayRotation, freezeRotation, getCurrentImeTouchRegion, getDisplayIdByUniqueId, getDisplayImePolicy, getImeDisplayId, getLetterboxBackgroundColorInArgb, getPossibleDisplayInfo, getPreferredOptionsPanelGravity, getRemoveContentMode, getStableInsets, getSupportedDisplayHashAlgorithms, getWindowContentFrameStats, getWindowInsets, getWindowingMode, hideTransientBars, holdLock, isDisplayRotationFrozen, isGlobalKey, isLayerTracing, isLetterboxBackgroundMultiColored, isRotationFrozen, isSafeModeEnabled, isTransitionTraceEnabled, isViewServerRunning, isWindowToken, isWindowTraceEnabled, lockNow, markSurfaceSyncGroupReady, mirrorDisplay, mirrorWallpaperSurface, notifyScreenshotListeners, overridePendingAppTransitionMultiThumbFuture, overridePendingAppTransitionRemote, reenableKeyguard, refreshScreenCaptureDisabled, registerCrossWindowBlurEnabledListener, registerDisplayFoldListener, registerDisplayWindowListener, registerPinnedTaskListener, registerProposedRotationListener, registerShortcutKey, registerSystemGestureExclusionListener, registerTaskFpsCallback, registerWallpaperVisibilityListener, removeKeyguardLockedStateListener, removeRotationWatcher, removeWindowToken, requestAppKeyboardShortcuts, requestAssistScreenshot, requestScrollCapture, saveWindowTraceToFile, screenshotWallpaper, setActiveTransactionTracing, setAnimationScale, setAnimationScales, setDisplayChangeWindowController, setDisplayHashThrottlingEnabled, setDisplayImePolicy, setDisplayWindowInsetsController, setEventDispatching, setFixedToUserRotation, setForcedDisplayDensityForUser, setForcedDisplayScalingMode, setForcedDisplaySize, setIgnoreOrientationRequest, setInTouchMode, setInTouchModeOnAllDisplays, setLayerTracing, setLayerTracingFlags, setNavBarVirtualKeyHapticFeedbackEnabled, setRecentsAppBehindSystemBars, setRecentsVisibility, setRemoveContentMode, setShellRootAccessibilityWindow, setShouldShowSystemDecors, setShouldShowWithInsecureKeyguard, setStrictModeVisualIndicatorPreference, setSwitchingUser, setTaskSnapshotEnabled, setTaskTransitionSpec, setWindowingMode, shouldShowSystemDecors, shouldShowWithInsecureKeyguard, showGlobalActions, showStrictModeViolation, snapshotTaskForRecents, startFreezingScreen, startTransitionTrace, startViewServer, startWindowTrace, stopFreezingScreen, stopTransitionTrace, stopViewServer, stopWindowTrace, syncInputTransactions, thawDisplayRotation, thawRotation, unregisterCrossWindowBlurEnabledListener, unregisterDisplayFoldListener, unregisterDisplayWindowListener, unregisterSystemGestureExclusionListener, unregisterTaskFpsCallback, unregisterWallpaperVisibilityListener, updateDisplayWindowRequestedVisibleTypes, updateStaticPrivacyIndicatorBounds, verifyDisplayHash, watchRotation |
| `android.view.IWindowSession` | 11/42 | cancelDragAndDrop, clearTouchableRegion, dragRecipientEntered, dragRecipientExited, dropForAccessibility, finishMovingTask, generateDisplayHash, getWindowId, grantEmbeddedWindowFocus, grantInputChannel, onRectangleOnScreenRequested, performDrag, performHapticFeedbackAsync, pokeDrawLock, reportDropResult, reportKeepClearAreasChanged, reportSystemGestureExclusionChanged, sendWallpaperCommand, setInsets, setOnBackInvokedCallbackInfo, setShouldZoomOutWallpaper, setWallpaperDisplayOffset, setWallpaperPosition, setWallpaperZoomOut, startMovingTask, transferEmbeddedTouchFocusToHost, updateInputChannel, updatePointerIcon, updateTapExcludeRegion, wallpaperCommandComplete, wallpaperOffsetsComplete |
| `android.view.accessibility.IAccessibilityManager` | 10/40 | associateEmbeddedHierarchy, disassociateEmbeddedHierarchy, getAccessibilityShortcutTargets, getAccessibilityWindowId, getWindowToken, getWindowTransformationSpec, injectInputEventToInputFilter, interrupt, isAccessibilityTargetAllowed, isAudioDescriptionByDefaultEnabled, isSystemAudioCaptioningUiEnabled, notifyAccessibilityButtonClicked, notifyAccessibilityButtonVisibilityChanged, performAccessibilityShortcut, registerProxyForDisplay, registerSystemAction, registerUiTestAutomationService, sendFingerprintGesture, sendRestrictedDialogIntent, setAccessibilityWindowAttributes, setPictureInPictureActionReplacingConnection, setSystemAudioCaptioningEnabled, setSystemAudioCaptioningUiEnabled, setWindowMagnificationConnection, startFlashNotificationEvent, startFlashNotificationSequence, stopFlashNotificationSequence, unregisterProxyForDisplay, unregisterSystemAction, unregisterUiTestAutomationService |
| `com.android.internal.textservice.ITextServicesManager` | 6/6 |  |
| `com.android.internal.view.IInputMethodManager` | 6/31 | acceptStylusHandwritingDelegation, addClient, addVirtualStylusIdForTestSession, getCurrentInputMethodInfoAsUser, getCurrentInputMethodSubtype, getImeTrackerService, getInputMethodWindowVisibleHeight, getLastInputMethodSubtype, isImeTraceEnabled, isInputMethodPickerShownForTest, isStylusHandwritingAvailableAsUser, prepareStylusHandwritingDelegation, removeImeSurface, removeImeSurfaceFromWindowAsync, reportPerceptibleAsync, reportVirtualDisplayGeometryAsync, setAdditionalInputMethodSubtypes, setExplicitlyEnabledInputMethodSubtypes, setStylusWindowIdleTimeoutForTest, showInputMethodPickerFromClient, showInputMethodPickerFromSystem, startImeTrace, startProtoDump, startStylusHandwriting, stopImeTrace |


## System services, every name

| # | Constant | Name | Since | Win.apk returns (where) | What it means for a caller | Android returns | Apps referencing (of 11) | Severity |
|---|---|---|---|---|---|---|---|---|
| 1 | `TELECOM_SERVICE` | `telecom` | 21 | **null** (no case; `android.telecom.TelecomManager` exists in android-framework.jar but is unreachable) — runtime/java/android/content/Context.java:1601 (default) | `(TelecomManager) getSystemService(TELECOM_SERVICE)` then any call NPEs; Kotlin `as TelecomManager` throws. Jitsi's call integration (`JitsiConnectionService`) registers a PhoneAccount through it; its unregister path null-checks (`telecom is null` log string), registration path does not visibly | Non-null on every phone/tablet (`TelecomManager`) | 6/11 — class: sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, jitsi, ppsspp | **possible crash** (guaranteed service, 6 apps name the class) |
| 2 | `AUDIO_SERVICE` | `audio` | 1 | real class, Java model: `new android.media.AudioManager()` per call — runtime/java/android/content/Context.java:1517 | Volumes are a per-instance array filled with 7/15 (runtime/java/android/media/AudioManager.java:20-24,38-50); `setStreamVolume` changes nothing audible and is lost by the next `getSystemService`; Windows' volume is never read; `isMusicActive` false (:56); focus always granted (:72-75); one output/input device (:99-103); `getProperty` 48000/256 (:151-155) | System volume per stream, shared, audible | 11/11 — class: jerboa, klooni, sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, jitsi, opensudoku, ppsspp, tensy; string only: editor | **wrong answer** (volume UI/gestures in players do nothing) |
| 3 | `POWER_SERVICE` | `power` | 1 | real class, Java only: `new android.os.PowerManager()` — runtime/java/android/content/Context.java:1511 | Wake locks are counted (runtime/java/android/os/PowerManager.java:287-306) but nothing tells Windows (no `SetThreadExecutionState` anywhere in windows/host): a video app's wake lock or `FLAG_KEEP_SCREEN_ON` does not stop the PC's display sleep; `isInteractive` true, `isPowerSaveMode` false, thermal NONE (:44-81) | Wake locks keep the CPU/screen on | 9/11 — class: jerboa, sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, jitsi, opensudoku, ppsspp; string only: klooni | **wrong answer** (screen sleeps during playback/calls) |
| 4 | `KEYGUARD_SERVICE` | `keyguard` | 1 | real class: `new android.app.KeyguardManager()` — runtime/java/android/content/Context.java:1389 | `isDeviceSecure()`/`isKeyguardSecure()` ask `UserVerificationBridge.canAuthenticate`, whose `available()` is hard-coded false (runtime/java/com/macapk/loader/UserVerificationBridge.java:25), so always **false** (runtime/java/android/app/KeyguardManager.java:27-29); `isKeyguardLocked` false | true on any phone with a PIN/pattern (most) | 6/11 — class: sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, jitsi, ppsspp | **wrong answer** (apps that require a secure lock screen refuse; Windows Hello not wired) |
| 5 | `BIOMETRIC_SERVICE` | `biometric` | 29 | real class: `new BiometricManager()` over `UserVerificationBridge` — runtime/java/android/content/Context.java:1328 | `canAuthenticate` -> 12 `BIOMETRIC_ERROR_NO_HARDWARE` (UserVerificationBridge.java:25-26), natives declared but bridge disabled | SUCCESS / NONE_ENROLLED on most phones | 2/11 — class: fossify-gallery, fossify-notes | wrong answer (honest-looking 'no hardware'; Windows Hello exists) |
| 6 | `TELEPHONY_SERVICE` | `phone` | 1 | real class, constants: `new TelephonyManager()` — runtime/java/android/content/Context.java:1424 | `getNetworkOperatorName()` returns **"Mac.apk"** (runtime/java/android/telephony/TelephonyManager.java:274) — upstream's name leaks to apps; otherwise a no-SIM GSM phone: SIM_STATE_ABSENT, IDs null, country ISO from the host region (:265-312) | "" with no SIM | 7/11 — class: jerboa, sgtpuzzles, fossify-gallery, jellyfin, jitsi, ppsspp; string only: fossify-notes | **wrong answer** (string shown/logged by apps) |
| 7 | `ACTIVITY_SERVICE` | `activity` | 1 | real class, mostly Java: `new android.app.ActivityManager()` — runtime/java/android/content/Context.java:1246 | Memory classes/`getMemoryInfo` from the host's RAM (runtime/java/android/app/ActivityManager.java:68-72,133); `getRunningAppProcesses` self only; `getAppTasks()` **empty** (:186) though the app has a task; `clearApplicationUserData()` returns **true and deletes nothing** (:196); `getHistoricalProcessExitReasons` empty (:853); GLES reported 2.0 (:19-22) | Tasks list holds the current task; clearApplicationUserData wipes and kills | 7/11 — class: jerboa, sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, jitsi, ppsspp | wrong answer (minor; `getAppTasks().get(0)` would throw IOOBE) |
| 8 | `PRINT_SERVICE` | `print` | 19 | real class: `new PrintManager(this)` runs the adapter protocol — runtime/java/android/content/Context.java:1457 | `print()` runs onLayout/onWrite and **silently saves a PDF in the app's filesDir/print/** — no print dialog, no Windows printer (runtime/java/android/print/PrintManager.java:1-30); its header still names macOS CUPS and a `MACAPK_PRINT_DESTINATION` switch | System print UI (printer or Save as PDF chosen by the user) | 9/11 — class: sgtpuzzles, editor, fossify-gallery, fossify-notes, jellyfin, ppsspp; string only: jerboa, jitsi, opensudoku | wrong answer (user sees nothing happen) |
| 9 | `NSD_SERVICE` | `servicediscovery` | 16 | refusing object: `new NsdManager(this)` — runtime/java/android/content/Context.java:1475 | register/discover/resolve post `FAILURE_INTERNAL_ERROR` (runtime/java/android/net/nsd/NsdManager.java:113-138); no mDNS though Windows has DnsServiceBrowse/Register | Working mDNS/DNS-SD | 3/11 — class: sgtpuzzles, jellyfin, ppsspp | wrong answer (LAN discovery — e.g. media servers, casting — never finds anything) |
| 10 | `SHORTCUT_SERVICE` | `shortcut` | 25 | empty object: `new ShortcutManager()` — runtime/java/android/content/Context.java:1334 | `setDynamicShortcuts` returns **false** (runtime/java/android/content/pm/ShortcutManager.java:36), lists empty, pinning unsupported (:54-58) | true; launcher shows shortcuts | 4/11 — class: fossify-gallery, fossify-notes, ppsspp; string only: jitsi | wrong answer (minor; false means 'rate limited' to callers) |
| 11 | `WALLPAPER_SERVICE` | `wallpaper` | 1 | partial: `WallpaperManager.getInstance(this)` — runtime/java/android/content/Context.java:1395 | `getDrawable()/peekDrawable()` **null** (runtime/java/android/app/WallpaperManager.java:33-36); `getWallpaperColors` real-ish from the host palette (com.macapk.loader.monet) | Drawable (or SecurityException without permission on 33+) | 4/11 — class: sgtpuzzles, fossify-gallery, jellyfin, ppsspp | wrong answer (apps that draw the wallpaper NPE if unguarded) |
| 12 | `MEDIA_PROJECTION_SERVICE` | `media_projection` | 21 | refusing object: `new MediaProjectionManager(this)` — runtime/java/android/content/Context.java:1451 | `createScreenCaptureIntent()` returns an **empty Intent** (runtime/java/android/media/projection/MediaProjectionManager.java:28-30) — `startActivityForResult` on it finds no activity; `getMediaProjection` null (:53-55) | Intent to the system's consent dialog | 4/11 — class: sgtpuzzles, jellyfin, jitsi, ppsspp | wrong answer (screen sharing, e.g. Jitsi, fails; ActivityNotFoundException possible) |
| 13 | `SENSOR_SERVICE` | `sensor` | 1 | empty object (cached per Context): `SensorManager.macapkNew()` — runtime/java/android/content/Context.java:1306 | No sensors at all: catalog empty, `getDefaultSensor` null (runtime/java/android/hardware/SensorManager.java:161-192); Context.java's comment ('synthetic flat, still') is stale | Accelerometer, gyro... on a phone | 7/11 — class: jerboa, klooni, sgtpuzzles, jellyfin, jitsi, ppsspp, tensy | honest (PC has none); apps must null-check as on a sensorless device |
| 14 | `DEVICE_POLICY_SERVICE` | `device_policy` | 8 | empty object: `new DevicePolicyManager(this)` — runtime/java/android/content/Context.java:1460 | No admins; `getStorageEncryptionStatus` UNSUPPORTED (runtime/java/android/app/admin/DevicePolicyManager.java:220); admin-only calls throw SecurityException | Non-null; encryption ACTIVE_PER_USER on 29+ | 3/11 — class: sgtpuzzles, jellyfin, ppsspp | wrong answer (minor: encryption) |
| 15 | `CARRIER_CONFIG_SERVICE` | `carrier_config` | 23 | **null** (class `android.telephony.CarrierConfigManager` exists, unreachable) — runtime/java/android/content/Context.java:1601 (default) | `getConfig()` NPEs; typed lookup maps correctly (Context.java:1830) to a name with no case | Non-null on phones | 0/11 | null where Android never is (unused by these apps) |
| 16 | `NETWORK_STATS_SERVICE` | `netstats` | 23 | **null** (class `android.app.usage.NetworkStatsManager` exists, 12 lines, unreachable) — runtime/java/android/content/Context.java:1601 (default) | `querySummary` NPEs | Non-null | 0/11 | null where Android never is (unused) |
| 17 | `CROSS_PROFILE_APPS_SERVICE` | `crossprofileapps` | 28 | **null** (class `android.content.pm.CrossProfileApps` exists, unreachable) — runtime/java/android/content/Context.java:1601 (default) | `getTargetUserProfiles()` NPEs instead of an empty list | Non-null | 0/11 | null where Android never is (unused) |
| 18 | `EUICC_SERVICE` | `euicc` | 28 | **null** (class `EuiccManager` exists, unreachable) — runtime/java/android/content/Context.java:1601 (default) | `isEnabled()` NPEs | Non-null (isEnabled false without eSIM) | 0/11 | null where Android never is (unused) |
| 19 | `VPN_MANAGEMENT_SERVICE` | `vpn_management` | 30 | **null** (class `android.net.VpnManager` exists, unreachable) — runtime/java/android/content/Context.java:1601 (default) | VPN-profile apps NPE | Non-null | 0/11 | null where Android never is (unused) |
| 20 | `DOMAIN_VERIFICATION_SERVICE` | `domain_verification` | 31 | **null** (class exists, 51 lines; deliberately absent per Context.java:1544-1552) — runtime/java/android/content/Context.java:1601 (default) | Context.java's own comment cites Aurora Store NPE'ing on `getSystemService(DomainVerificationManager.class).getClass()` | Non-null on 31+ | 0/11 | null where Android never is (known crash shape; unused here) |
| 21 | `BUGREPORT_SERVICE` | `bugreport` | 31 | **null** (class `android.os.BugreportManager` exists, unreachable) — runtime/java/android/content/Context.java:1601 (default) | Rare in apps | Non-null | 0/11 | null (unused) |
| 22 | `PEOPLE_SERVICE` | `people` | 31 | **null** (no class in the stage) — runtime/java/android/content/Context.java:1601 (default) | AndroidX/conversation APIs NPE if called | Non-null on 31+ | 1/11 — string only: jellyfin | null where Android never is (jellyfin: common-word string only) |
| 23 | `OVERLAY_SERVICE` | `overlay` | 34 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | OverlayManager for fabricated overlays | Non-null on 34+ | 1/11 — string only: jitsi | null (jitsi: common-word string only) |
| 24 | `STATUS_BAR_SERVICE` | `statusbar` | 33 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | `requestAddTileService` (Quick Settings tile) NPEs | Non-null | 0/11 | null (unused) |
| 25 | `APP_SEARCH_SERVICE` | `app_search` | 31 | **null** (no class; deliberately absent, Context.java:1544-1552) — runtime/java/android/content/Context.java:1601 (default) | AndroidX AppSearch platform storage fails | Non-null on 31+ | 0/11 | null (unused) |
| 26 | `BLOB_STORE_SERVICE` | `blob_store` | 30 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | — | Non-null on 30+ | 0/11 | null (unused) |
| 27 | `CONNECTIVITY_DIAGNOSTICS_SERVICE` | `connectivity_diagnostics` | 30 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | — | Non-null on 30+ | 0/11 | null (unused) |
| 28 | `TELEPHONY_IMS_SERVICE` | `telephony_ims` | 30 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | — | Non-null on phones | 0/11 | null (unused) |
| 29 | `MEDIA_COMMUNICATION_SERVICE` | `media_communication` | 31 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | — | Non-null on 31+ | 0/11 | null (unused) |
| 30 | `DISPLAY_HASH_SERVICE` | `display_hash` | 31 | **null** (`DisplayHashManager` is in aosp-views.jar but neither switch answers it) — runtime/java/android/content/Context.java:1601 (default) | — | Non-null on 31+ | 0/11 | null (unused) |
| 31 | `HEALTHCONNECT_SERVICE` | `healthconnect` | 34 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | Health Connect clients fall back to 'unavailable' | Non-null on 34+ | 0/11 | null (unused; AndroidX health checks availability first) |
| 32 | `CONTACT_KEYS_SERVICE` | `contact_keys` | 35 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | — | Non-null on 35+ | 0/11 | null (unused) |
| 33 | `TETHERING_SERVICE` | `tethering` | 36 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | — | Non-null (public since 36) | 0/11 | null (unused) |
| 34 | `APP_FUNCTION_SERVICE` | `app_function` | 36 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | — | Non-null on 36 devices with the module (not verified) | 0/11 | null (unused) |
| 35 | `TV_INPUT_SERVICE` | `tv_input` | 21 | **null** (class `TvInputManager` exists, unreachable) — runtime/java/android/content/Context.java:1601 (default) | Callers already handle null | **null on phones** (no tv_input binder) | 3/11 — class: sgtpuzzles, jellyfin, ppsspp | matches a phone (sgtpuzzles/ppsspp via AndroidX map, jellyfin TV code) |
| 36 | `WIFI_AWARE_SERVICE` | `wifiaware` | 26 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | — | null without FEATURE_WIFI_AWARE | 0/11 | matches a device without the feature |
| 37 | `WIFI_RTT_RANGING_SERVICE` | `wifirtt` | 28 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | — | null without FEATURE_WIFI_RTT | 0/11 | matches a device without the feature |
| 38 | `TV_INTERACTIVE_APP_SERVICE` | `tv_interactive_app` | 33 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | — | null on phones (TV feature) | 0/11 | matches a phone |
| 39 | `DEVICE_LOCK_SERVICE` | `device_lock` | 34 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | — | null without FEATURE_DEVICE_LOCK | 0/11 | matches most devices |
| 40 | `MEDIA_QUALITY_SERVICE` | `media_quality` | 36 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | — | TV only | 0/11 | matches a phone |
| 41 | `TV_AD_SERVICE` | `tv_ad` | 36 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | — | TV only | 0/11 | matches a phone |
| 42 | `SATELLITE_SERVICE` | `satellite` | 36 | **null** (no class) — runtime/java/android/content/Context.java:1601 (default) | — | null without satellite support | 0/11 | matches most devices |
| 43 | `PERSISTENT_DATA_BLOCK_SERVICE` | `persistent_data_block` | 35 | **null** (class exists, unreachable) — runtime/java/android/content/Context.java:1601 (default) | — | Non-null only for privileged callers in practice | 0/11 | unused |
| 44 | `LAYOUT_INFLATER_SERVICE` | `layout_inflater` | 1 | real: AOSP `PhoneLayoutInflater`, cached per Context — AospViewHost (windows/java/aosp_views/com/winapk/aospviews/AospViewHost.java:123) | Works | PhoneLayoutInflater | 9/11 — class: jerboa, sgtpuzzles, editor, fossify-gallery, fossify-notes, jellyfin, jitsi, opensudoku, ppsspp | real |
| 45 | `WINDOW_SERVICE` | `window` | 1 | real: AOSP `WindowManagerImpl` (aosp-views.jar) over Win.apk's in-process window manager (`AospWindows`) — AospViewHost (windows/java/aosp_views/com/winapk/aospviews/AospViewHost.java:121) | Works; Activity overrides with its own window's manager | WindowManagerImpl | 10/11 — class: jerboa, klooni, sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, jitsi, opensudoku, ppsspp, tensy | real |
| 46 | `INPUT_METHOD_SERVICE` | `input_method` | 3 | real: AOSP `InputMethodManager.forContext` over `ImeService` + TSF (`tsf_session.h`) — AospViewHost (windows/java/aosp_views/com/winapk/aospviews/AospViewHost.java:132) | Windows' IME types into the focused editor; no on-screen keyboard | InputMethodManager | 11/11 — class: jerboa, klooni, sgtpuzzles, editor, fossify-gallery, fossify-notes, jellyfin, jitsi, opensudoku, ppsspp, tensy | real |
| 47 | `ACCESSIBILITY_SERVICE` | `accessibility` | 4 | real: AOSP `AccessibilityManager.getInstance` over in-process `AccessibilityService`; UIA bridge (uia_provider.cpp) — AospViewHost (windows/java/aosp_views/com/winapk/aospviews/AospViewHost.java:134) | Screen readers read the views | AccessibilityManager | 8/11 — class: jerboa, sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, jitsi, opensudoku, ppsspp | real |
| 48 | `TEXT_SERVICES_MANAGER_SERVICE` | `textservices` | 14 | real class: AOSP `TextServicesManager` over in-process `TextServicesService` — AospViewHost (windows/java/aosp_views/com/winapk/aospviews/AospViewHost.java:140) | No spell checker sessions of substance | TextServicesManager | 3/11 — class: sgtpuzzles, jellyfin, ppsspp | real class, empty answers |
| 49 | `CAPTIONING_SERVICE` | `captioning` | 19 | real class: AOSP `CaptioningManager` — AospViewHost (windows/java/aosp_views/com/winapk/aospviews/AospViewHost.java:136) | Captions off (default settings) | CaptioningManager | 6/11 — class: jerboa, sgtpuzzles, fossify-gallery, jellyfin, jitsi, ppsspp | real (defaults) |
| 50 | `TEXT_CLASSIFICATION_SERVICE` | `textclassification` | 26 | real class: AOSP `TextClassificationManager` — AospViewHost (windows/java/aosp_views/com/winapk/aospviews/AospViewHost.java:138) | Local classifier only | TextClassificationManager | 8/11 — class: jerboa, sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, jitsi, opensudoku, ppsspp | real |
| 51 | `CLIPBOARD_SERVICE` | `clipboard` | 1 | real: `android.content.ClipboardManager` over `HostPasteboard` (windows/host/native_clipboard.cpp) — runtime/java/android/content/Context.java:1294 | Copy/paste with Windows' clipboard | ClipboardManager | 10/11 — class: jerboa, klooni, sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, jitsi, opensudoku, ppsspp, tensy | real |
| 52 | `CONNECTIVITY_SERVICE` | `connectivity` | 1 | real: `ConnectivityManager` over `MacApkHostNetwork` -> `HostRadios.defaultNetwork` (windows/host/native_radios.cpp: GetBestInterfaceEx/GetAdaptersAddresses) — runtime/java/android/content/Context.java:1508 | Active network, type, DNS, MTU from the host's default route | ConnectivityManager | 7/11 — class: jerboa, sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, jitsi, ppsspp | real |
| 53 | `LOCATION_SERVICE` | `location` | 1 | real (cached per Context): `LocationManager` over `LocationBridge` (windows/host/native_location.cpp) — runtime/java/android/content/Context.java:1316 | Windows location | LocationManager | 8/11 — class: jerboa, sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, jitsi, opensudoku, ppsspp | real |
| 54 | `NOTIFICATION_SERVICE` | `notification` | 1 | real: `NotificationManager` over `HostNotifications` (native_notifications.cpp, Windows toasts) — runtime/java/android/content/Context.java:1377 | Notifications appear as toasts; channels kept | NotificationManager | 8/11 — class: jerboa, sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, jitsi, opensudoku, ppsspp | real |
| 55 | `WIFI_SERVICE` | `wifi` | 1 | real: `WifiManager` over `HostRadios` (native_radios.cpp) — runtime/java/android/content/Context.java:1553 | Wi-Fi state, connection info, scan results from the host | WifiManager | 4/11 — class: sgtpuzzles, jellyfin, jitsi, ppsspp | real |
| 56 | `BLUETOOTH_SERVICE` | `bluetooth` | 18 | real: `BluetoothManager.getAdapter()` -> `BluetoothAdapter` over native_bluetooth.cpp (RFCOMM, LE scan, GATT client) — runtime/java/android/content/Context.java:1433 | Works on the PC's radio | BluetoothManager | 5/11 — class: sgtpuzzles, jellyfin, ppsspp, tensy; string only: jitsi | real |
| 57 | `CAMERA_SERVICE` | `camera` | 21 | real: `CameraManager.getCameraIdList` -> `CameraBackend` over `CameraBridge` (native_camera.cpp) — runtime/java/android/content/Context.java:1324 | PC cameras (Context.java:1325's 'no cameras' comment is stale) | CameraManager | 6/11 — class: sgtpuzzles, jellyfin, jitsi, ppsspp; string only: klooni, fossify-gallery | real |
| 58 | `ALARM_SERVICE` | `alarm` | 1 | real, in-process: `AlarmManager` on a ScheduledExecutorService (runtime/java/android/app/AlarmManager.java:33-68) — runtime/java/android/content/Context.java:1374 | Alarms fire only while the app's process runs; no wake from closed | AlarmManager (system-held) | 5/11 — class: sgtpuzzles, fossify-notes, jellyfin, ppsspp; string only: fossify-gallery | real while running |
| 59 | `ACCOUNT_SERVICE` | `account` | 5 | real: `AccountManager.get` over `SystemAccounts`/`AccountsService` (the Google account with Google services on) — runtime/java/android/content/Context.java:1442 | Accounts list and tokens via the broker | AccountManager | 6/11 — class: jerboa, sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, ppsspp | real |
| 60 | `DOWNLOAD_SERVICE` | `download` | 9 | real, in-process: `DownloadManager` downloads on its own thread (runtime/java/android/app/DownloadManager.java:177-189) — runtime/java/android/content/Context.java:1383 | Downloads work while the app runs; `getUriForDownloadedFile` gives file:// not content:// | Download provider | 5/11 — class: sgtpuzzles, jellyfin, jitsi, ppsspp; string only: fossify-gallery | real (differs: file:// Uri) |
| 61 | `INPUT_SERVICE` | `input` | 16 | real: `InputManager` with gamepads from `GamepadBridge` (native_gamepad.cpp) — runtime/java/android/content/Context.java:1556 | Input devices incl. controllers | InputManager | 7/11 — class: sgtpuzzles, jellyfin, ppsspp; string only: jerboa, klooni, fossify-notes, jitsi | real |
| 62 | `DISPLAY_SERVICE` | `display` | 17 | real: `DisplayManager` over `DisplayConfig` (the window's size/density) — runtime/java/android/content/Context.java:1300 | One display = the app window | DisplayManager | 6/11 — class: jerboa, sgtpuzzles, fossify-gallery, jellyfin, jitsi, ppsspp | real |
| 63 | `APP_OPS_SERVICE` | `appops` | 19 | real: `AppOpsManager` over `SpecialAccessStore` (the launcher's signed grants) — runtime/java/android/content/Context.java:1392 | Special access answered from the user's grants | AppOpsManager | 8/11 — class: jerboa, sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, jitsi, opensudoku, ppsspp | real |
| 64 | `BATTERY_SERVICE` | `batterymanager` | 21 | real: `BatteryManager` over `HostPower` (native_radios.cpp) — runtime/java/android/content/Context.java:1430 | Capacity/charging from the PC | BatteryManager | 4/11 — class: jerboa, sgtpuzzles, jellyfin, ppsspp | real |
| 65 | `JOB_SCHEDULER_SERVICE` | `jobscheduler` | 21 | real: `JobScheduler.STUB` = `MacApkJobScheduler` (1363 lines) + the broker's `BrokerJobs` when Google services are on — runtime/java/android/content/Context.java:1243 | Jobs run in-process; held by the system broker across exits | JobScheduler | 4/11 — class: sgtpuzzles, fossify-gallery, jellyfin, ppsspp | real |
| 66 | `UI_MODE_SERVICE` | `uimode` | 8 | Java model: `new UiModeManager()` — runtime/java/android/content/Context.java:1386 | MODE_TYPE_NORMAL; night mode from the host theme / app request | UiModeManager | 9/11 — class: jerboa, sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, jitsi, opensudoku, ppsspp, tensy | real enough |
| 67 | `STORAGE_SERVICE` | `storage` | 9 | Java model: `new StorageManager()` — runtime/java/android/content/Context.java:1280 | One primary volume; allocatable bytes from the disk | StorageManager | 6/11 — class: sgtpuzzles, jellyfin, ppsspp; string only: jerboa, editor, fossify-gallery | real enough |
| 68 | `USER_SERVICE` | `user` | 17 | Java model: `UserManager.get(this)` — runtime/java/android/content/Context.java:1367 | Single unlocked user, no profiles, no restrictions; `getUserName` = Windows user name | UserManager | 7/11 — class: sgtpuzzles, fossify-gallery, jellyfin, ppsspp; string only: jerboa, fossify-notes, jitsi | real enough |
| 69 | `STORAGE_STATS_SERVICE` | `storagestats` | 26 | real-ish: `StorageStatsManager` from the data dir's disk (runtime/java/android/app/usage/StorageStatsManager.java:33-40) — runtime/java/android/content/Context.java:1466 | Total/free bytes of the host disk | StorageStatsManager | 0/11 | real |
| 70 | `LOCALE_SERVICE` | `locale` | 33 | real (cached per Context): `LocaleManager` persisting the app's locales — runtime/java/android/content/Context.java:1398 | Per-app language round-trips | LocaleManager | 10/11 — class: jerboa, sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, jitsi, opensudoku; string only: klooni, ppsspp, tensy | real |
| 71 | `SEARCH_SERVICE` | `search` | 1 | Java model: `new SearchManager(this)` (Activity answers its own) — runtime/java/android/content/Context.java:1380 | SearchableInfo parsed; `startSearch` no-op; no global search | SearchManager | 7/11 — class: sgtpuzzles, jellyfin, ppsspp; string only: jerboa, editor, fossify-gallery, jitsi | partial |
| 72 | `MEDIA_ROUTER_SERVICE` | `media_router` | 16 | Java model: one default route — runtime/java/android/content/Context.java:1448 | No cast routes | MediaRouter | 3/11 — class: sgtpuzzles, jellyfin, ppsspp | honest |
| 73 | `APPWIDGET_SERVICE` | `appwidget` | 21 | empty object: `AppWidgetManager.getInstance` — runtime/java/android/content/Context.java:1342 | No hosts: ids empty, pin unsupported (runtime/java/android/appwidget/AppWidgetManager.java:87-132) | Launcher with widgets | 5/11 — class: sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, ppsspp | honest empty |
| 74 | `LAUNCHER_APPS_SERVICE` | `launcherapps` | 21 | partial: `LauncherApps` over the package manager — runtime/java/android/content/Context.java:1487 | Own activities; no shortcut host permission | LauncherApps | 3/11 — class: sgtpuzzles, jellyfin, ppsspp | partial |
| 75 | `MEDIA_SESSION_SERVICE` | `media_session` | 21 | empty object — runtime/java/android/content/Context.java:1445 | `getActiveSessions` empty (runtime/java/android/media/session/MediaSessionManager.java:53) | Lists sessions (needs listener permission) | 3/11 — class: sgtpuzzles, jellyfin, ppsspp | honest empty |
| 76 | `VIBRATOR_SERVICE` | `vibrator` | 1 | empty object: `new Vibrator()` — runtime/java/android/content/Context.java:1523 | `hasVibrator()` false (runtime/java/android/os/Vibrator.java:22) | Vibrator | 8/11 — class: klooni, sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, jitsi, ppsspp, tensy | honest (PC) |
| 77 | `DROPBOX_SERVICE` | `dropbox` | 8 | empty object — runtime/java/android/content/Context.java:1496 | Drops entries | DropBoxManager | 3/11 — class: sgtpuzzles, jellyfin, ppsspp | honest |
| 78 | `NFC_SERVICE` | `nfc` | 10 | empty object — runtime/java/android/content/Context.java:1436 | `getDefaultAdapter()` null (runtime/java/android/nfc/NfcManager.java:20) | null adapter without NFC | 3/11 — class: sgtpuzzles, jellyfin, ppsspp | honest |
| 79 | `USB_SERVICE` | `usb` | 12 | empty object — runtime/java/android/content/Context.java:1439 | No devices (runtime/java/android/hardware/usb/UsbManager.java:36-51) | UsbManager | 6/11 — class: sgtpuzzles, fossify-gallery, fossify-notes, jellyfin, ppsspp, tensy | honest |
| 80 | `WIFI_P2P_SERVICE` | `wifip2p` | 14 | refusing object — runtime/java/android/content/Context.java:1478 | Every action fails (runtime/java/android/net/wifi/p2p/WifiP2pManager.java:210-212) | WifiP2pManager | 4/11 — class: sgtpuzzles, jellyfin, jitsi, ppsspp | honest refusal |
| 81 | `CONSUMER_IR_SERVICE` | `consumer_ir` | 19 | empty object — runtime/java/android/content/Context.java:1582 | `hasIrEmitter` false | ConsumerIrManager | 3/11 — class: sgtpuzzles, jellyfin, ppsspp | honest |
| 82 | `RESTRICTIONS_SERVICE` | `restrictions` | 21 | empty object — runtime/java/android/content/Context.java:1578 | Empty Bundle | RestrictionsManager | 5/11 — class: sgtpuzzles, jellyfin, jitsi, ppsspp; string only: jerboa | honest |
| 83 | `TELEPHONY_SUBSCRIPTION_SERVICE` | `telephony_subscription_service` | 22 | empty object — runtime/java/android/content/Context.java:1427 | No subscriptions (runtime/java/android/telephony/SubscriptionManager.java:77-91) | Subscriptions of the SIM(s) | 3/11 — class: sgtpuzzles, jellyfin, ppsspp | honest (no SIM) |
| 84 | `USAGE_STATS_SERVICE` | `usagestats` | 22 | empty object — runtime/java/android/content/Context.java:1463 | No stats; bucket ACTIVE | UsageStatsManager | 3/11 — class: sgtpuzzles, jellyfin, ppsspp | honest |
| 85 | `FINGERPRINT_SERVICE` | `fingerprint` | 23 | empty object — runtime/java/android/content/Context.java:1331 | No hardware (runtime/java/android/hardware/fingerprint/FingerprintManager.java:24-25) | FingerprintManager | 3/11 — class: fossify-gallery, fossify-notes, ppsspp | honest |
| 86 | `MIDI_SERVICE` | `midi` | 23 | empty object — runtime/java/android/content/Context.java:1454 | No devices | MidiManager | 2/11 — string only: fossify-gallery, fossify-notes | honest |
| 87 | `COMPANION_DEVICE_SERVICE` | `companiondevice` | 26 | refusing object — runtime/java/android/content/Context.java:1481 | associate() reports failure | CompanionDeviceManager | 0/11 | honest |
| 88 | `ROLE_SERVICE` | `role` | 29 | empty object — runtime/java/android/content/Context.java:1490 | `isRoleAvailable/isRoleHeld` false (runtime/java/android/app/role/RoleManager.java) | Roles available (browser, dialer, SMS...) | 6/11 — class: fossify-gallery, fossify-notes; string only: jerboa, sgtpuzzles, jellyfin, jitsi | minor wrong answer |
| 89 | `VIBRATOR_MANAGER_SERVICE` | `vibrator_manager` | 31 | empty object — runtime/java/android/content/Context.java:1535 | No vibrator ids (runtime/java/android/os/VibratorManager.java:15) | VibratorManager | 2/11 — class: jitsi, tensy | honest |
| 90 | `HARDWARE_PROPERTIES_SERVICE` | `hardware_properties` | 24 | empty object — runtime/java/android/content/Context.java:1472 | No temperatures | SecurityException for non-owners | 0/11 | honest |
| 91 | `SYSTEM_HEALTH_SERVICE` | `systemhealth` | 24 | empty object — runtime/java/android/content/Context.java:1493 | Empty HealthStats | SystemHealthManager | 0/11 | honest |
| 92 | `IPSEC_SERVICE` | `ipsec` | 28 | empty object — runtime/java/android/content/Context.java:1499 | — | IpSecManager | 0/11 | unused |
| 93 | `FILE_INTEGRITY_SERVICE` | `file_integrity` | 30 | refusing object — runtime/java/android/content/Context.java:1237 | apk-verity unsupported | FileIntegrityManager | 0/11 | honest |
| 94 | `GAME_SERVICE` | `game` | 31 | empty object — runtime/java/android/content/Context.java:1541 | GAME_MODE_STANDARD | GameManager | 3/11 — string only: klooni, sgtpuzzles, opensudoku | honest |
| 95 | `MEDIA_METRICS_SERVICE` | `media_metrics` | 31 | empty object — runtime/java/android/content/Context.java:1564 | Sessions accept and drop | MediaMetricsManager | 4/11 — class: jerboa, fossify-gallery, jellyfin, jitsi | honest |
| 96 | `PERFORMANCE_HINT_SERVICE` | `performance_hint` | 31 | empty object — runtime/java/android/content/Context.java:1538 | Preferred rate 0; sessions no-op | PerformanceHintManager | 0/11 | honest |
| 97 | `CREDENTIAL_SERVICE` | `credential` | 34 | refusing object — runtime/java/android/content/Context.java:1593 | get/create report 'no credential' via onError | CredentialManager | 1/11 — string only: jitsi | honest (no providers) |
| 98 | `GRAMMATICAL_INFLECTION_SERVICE` | `grammatical_inflection` | 34 | empty object — runtime/java/android/content/Context.java:1570 | — | GrammaticalInflectionManager | 0/11 | honest |
| 99 | `VIRTUAL_DEVICE_SERVICE` | `virtualdevice` | 34 | empty object — runtime/java/android/content/Context.java:1484 | No virtual devices | VirtualDeviceManager | 0/11 | honest |
| 100 | `PROFILING_SERVICE` | `profiling` | 35 | empty object — runtime/java/android/content/Context.java:1259 | Requests do nothing | ProfilingManager | 0/11 | honest |
| 101 | `SECURITY_STATE_SERVICE` | `security_state` | 35 | real-ish object — runtime/java/android/content/Context.java:1267 | Security patch + host kernel version | SecurityStateManager | 0/11 | honest |
| 102 | `ADVANCED_PROTECTION_SERVICE` | `advanced_protection` | 36 | refusing object — runtime/java/android/content/Context.java:1240 | Off | AdvancedProtectionManager | 0/11 | honest |
| 103 | `KEYSTORE_SERVICE` | `keystore` | 36 | refusing object — runtime/java/android/content/Context.java:1234 | Grants ERROR_UNIMPLEMENTED | KeyStoreManager | 0/11 | honest |

## Native code: the NDK's libraries

### Per library

Public = in the library's `*.map.txt`, not `llndk`/`apex`/`systemapi`/`platform-only`, present on x86_64 or arm64. A symbol two maps list (libGLESv2 and v3 share the ES 2.0 functions; ANativeWindow_* is in libandroid and libnativewindow) counts in each.

| Library | Public symbols | Implemented | Stubbed | Missing | Map (android.googlesource.com) |
|---|---:|---:|---:|---:|---|
| libc | 1443 | 848 | 55 | 540 | bionic/+/refs/heads/android16-release/libc/libc.map.txt |
| libm | 286 | 133 | 0 | 153 | bionic/+/refs/heads/android16-release/libm/libm.map.txt |
| libdl | 12 | 8 | 0 | 4 | bionic/+/refs/heads/android16-release/libdl/libdl.map.txt |
| libstdc++ | 13 | 12 | 0 | 1 | bionic/+/refs/heads/android16-release/libc/libstdc++.map.txt |
| liblog | 19 | 6 | 0 | 13 | system/logging/+/refs/heads/android16-release/liblog/liblog.map.txt |
| libz | 102 | 85 | 0 | 17 | external/zlib/+/refs/heads/android16-release/libz.map.txt |
| libandroid | 401 | 147 | 41 | 213 | frameworks/base/+/refs/heads/android16-release/native/android/libandroid.map.txt |
| libjnigraphics | 39 | 3 | 0 | 36 | frameworks/base/+/refs/heads/android16-release/native/graphics/jni/libjnigraphics.map.txt |
| libamidi | 14 | 0 | 0 | 14 | frameworks/base/+/refs/heads/android16-release/media/native/midi/libamidi.map.txt |
| libnativewindow | 31 | 6 | 9 | 16 | frameworks/native/+/refs/heads/android16-release/libs/nativewindow/libnativewindow.map.txt |
| libEGL | 73 | 52 | 2 | 19 | frameworks/native/+/refs/heads/android16-release/opengl/libs/libEGL.map.txt |
| libGLESv1_CM | 278 | 184 | 0 | 94 | frameworks/native/+/refs/heads/android16-release/opengl/libs/libGLESv1_CM.map.txt |
| libGLESv2 | 204 | 204 | 0 | 0 | frameworks/native/+/refs/heads/android16-release/opengl/libs/libGLESv2.map.txt |
| libGLESv3 | 418 | 416 | 2 | 0 | frameworks/native/+/refs/heads/android16-release/opengl/libs/libGLESv3.map.txt |
| libvulkan | 251 | 0 | 0 | 251 | frameworks/native/+/refs/heads/android16-release/vulkan/libvulkan/libvulkan.map.txt |
| libbinder_ndk | 150 | 0 | 0 | 150 | frameworks/native/+/refs/heads/android16-release/libs/binder/ndk/libbinder_ndk.map.txt |
| libsync | 3 | 0 | 0 | 3 | system/core/+/refs/heads/android16-release/libsync/libsync.map.txt |
| libmediandk | 364 | 162 | 36 | 166 | frameworks/av/+/refs/heads/android16-release/media/ndk/libmediandk.map.txt |
| libcamera2ndk | 72 | 0 | 0 | 72 | frameworks/av/+/refs/heads/android16-release/camera/ndk/libcamera2ndk.map.txt |
| libaaudio | 78 | 36 | 20 | 22 | frameworks/av/+/refs/heads/android16-release/media/libaaudio/src/libaaudio.map.txt |
| libOpenSLES | 56 | 56 | 0 | 0 | frameworks/wilhelm/+/refs/heads/android16-release/src/libOpenSLES.map.txt |
| libOpenMAXAL | 44 | 0 | 0 | 44 | frameworks/wilhelm/+/refs/heads/android16-release/src/libOpenMAXAL.map.txt |
| libneuralnetworks | 69 | 0 | 0 | 69 | packages/modules/NeuralNetworks/+/refs/heads/android16-release/runtime/libneuralnetworks.map.txt |
| libicu | 306 | 0 | 0 | 306 | external/icu/+/refs/heads/android16-release/libicu/libicu.map.txt |

### Missing or stubbed symbols the apps import

| Symbol | Library | Level | Win.apk today | What Android does | Apps importing it (.so) | Severity: reasoning |
|---|---|---:|---|---|---|---|
| `tmpnam` | libc | 21 | missing -> 0-returning stub | Returns a unique temporary path name (Bionic: under /data/local/tmp/). | org.ppsspp.ppsspp (libppsspp_jni.so) | **certain crash**: If PPSSPP reaches it, the stub's NULL is used as a path (strcpy/fopen of NULL). Rare: not on PPSSPP's normal paths. |
| `chmod` | libc | 21 | stub: returns 0 (windows/src/bionic/stdio.cpp:816) | Changes a file's mode; ENOENT for a missing path. | org.ppsspp.ppsspp (libppsspp_jni.so) | **wrong answer**: Always 0, also for a path that does not exist; PPSSPP only sets modes on files it just made. |
| `pthread_setschedparam` | libc | 21 | stub: returns 0 (windows/src/bionic/pthread.cpp:689) | Sets a thread's policy/priority; an app asking for SCHED_FIFO/RR gets EPERM. | org.jitsi.meet (libjingle_peerconnection_so.so); se.voxelmanip.tensy (libtensy.so) | **wrong answer**: Returns 0 and changes nothing: WebRTC's and tensy's audio/real-time threads believe they got the priority (on a phone they would get EPERM and fall back). |
| `swprintf` | libc | 21 | missing -> 0-returning stub | Formats into a wchar_t buffer. | org.fossify.gallery (libjxlcoder.so); org.jitsi.meet (libc++_shared.so); org.moire.opensudoku (libc++_shared.so); org.ppsspp.ppsspp (libppsspp_jni.so) | **wrong answer**: std::to_wstring (libc++) and PPSSPP/libjxl format with it; the stub returns 0 and writes nothing, so the wstring holds whatever was in the buffer. Reached only by wide-string code. |
| `syslog` | libc | 21 | stub: does nothing (windows/src/bionic/misc.cpp:2581) | Writes a line to logcat at the given priority. | org.fossify.gallery (libavif_android.so, libglide-webp.so, libjxl.so, libjxl_cms.so, libjxl_threads.so, libjxlcoder.so); org.jitsi.meet (libc++_shared.so, libjingle_peerconnection_so.so, libjsi.so, libnative-imagetranscoder.so, libreactnative.so); org.moire.opensudoku (libc++_shared.so, libtensorflowlite_jni.so); org.ppsspp.ppsspp (libppsspp_jni.so) | **wrong answer**: The line is dropped instead of reaching the run log; libc++abi reports an uncaught exception through it before abort(), so that reason is lost from the user's log. Diagnostics only. |
| `feclearexcept` | libm | 21 | missing -> 0-returning stub | Clears the FP exception flags (MXCSR on x86_64). | org.jitsi.meet (libreactnative.so) | **wrong answer**: Called from a stripped internal function of libreactnative.so that clears and then tests the flags; the stub clears nothing, and fetestexcept's 0 then reports no exception. |
| `fesetround` | libm | 21 | missing -> 0-returning stub | Sets the rounding mode; 0 on success. | org.moire.opensudoku (libopencv_java5.so) | **wrong answer**: OpenCV's dnn module (code beside ShrinkLayer::create) sets a rounding mode and is told 0 while the mode stays round-to-nearest: a .5 case rounds differently. dnn is not on OpenSudoku's path unless it runs a network. |
| `fetestexcept` | libm | 21 | missing -> 0-returning stub | Reports the raised FP exception flags. | org.jitsi.meet (libreactnative.so) | **wrong answer**: Always 0 in libreactnative.so: an FE_INVALID/FE_OVERFLOW a phone would report (an out-of-range double conversion) is never seen. Only matters for such values. |
| `ANativeWindow_release` | libandroid | 21 | stub: does nothing (windows/host/native_window.cpp:96) | Drops a reference; the window is freed at zero. | org.fossify.gallery (libpl_droidsonroids_gif.so); se.voxelmanip.tensy (libtensy.so) | **unused**: Win.apk has one ANativeWindow for every Surface (native_window.cpp, g_window and from_surface), so there is no count to drop. The real gap is fromSurface itself: android-gif-drawable's GifTextureView and tensy's surface all get the same window. |
| `__ctype_get_mb_cur_max` | libc | 21 | stub: returns 4 (windows/src/bionic/locale.cpp:93) | MB_CUR_MAX: 4 in Bionic's default C.UTF-8 locale (1 only after setlocale/uselocale to plain "C"). | org.jitsi.meet (libc++_shared.so, libjingle_peerconnection_so.so); org.moire.opensudoku (libc++_shared.so); org.ppsspp.ppsspp (libppsspp_jni.so) | **unused**: Returns 4, which is what a phone answers unless the app switched to "C". |
| `__libc_init` | libc | 21 | missing -> 0-returning stub | Bionic's start-up for an executable: _start (crtbegin) calls it, it runs the constructors and main. | name.boyle.chris.sgtpuzzles (libpuzzlesgen.so) | **unused**: Imported only by libpuzzlesgen.so, a PIE executable (INTERP /system/bin/linker64) that SGT Puzzles runs as a child process; Win.apk never runs an ELF executable (no runner in windows/; the guest exec family fails with ENOSYS), so the real gap is running it, not this symbol. |
| `__register_atfork` | libc | 23 | stub: returns 0 (windows/src/bionic/misc.cpp:2545) | Registers fork handlers (pthread_atfork). | org.fossify.gallery (libNativeImageProcessor.so); org.jitsi.meet (libhermes.so, libhermestooling.so, libjsctooling.so, libjsi.so, libreactnative.so, librnscreens.so, librnworklets.so) | **unused**: fork fails on Win.apk, so no handler would ever run; 0 is the right answer here. |
| `closelog` | libc | 21 | stub: does nothing (windows/src/bionic/misc.cpp:2581) | Closes the syslog connection (Bionic: nothing to close). | org.fossify.gallery (libavif_android.so, libglide-webp.so, libjxl.so, libjxl_cms.so, libjxl_threads.so, libjxlcoder.so); org.jitsi.meet (libc++_shared.so, libjingle_peerconnection_so.so, libjsi.so, libnative-imagetranscoder.so, libreactnative.so); org.moire.opensudoku (libc++_shared.so, libtensorflowlite_jni.so); org.ppsspp.ppsspp (libppsspp_jni.so) | **unused**: No-op on a phone too. |
| `fchmod` | libc | 21 | stub: returns 0 (windows/src/bionic/stdio.cpp:817) | Changes an open file's mode. | org.jitsi.meet (libc++_shared.so); org.moire.opensudoku (libc++_shared.so) | **unused**: libc++ std::filesystem::permissions; Windows has no POSIX modes, 0 matches what callers expect. |
| `fchmodat` | libc | 21 | stub: returns 0 (windows/src/bionic/stdio.cpp:822) | Changes a file's mode relative to a directory fd. | org.jitsi.meet (libc++_shared.so); org.moire.opensudoku (libc++_shared.so) | **unused**: libc++ std::filesystem::permissions; as fchmod. |
| `fputwc` | libc | 21 | missing -> 0-returning stub | Writes a wide character to a FILE. | org.jitsi.meet (libc++_shared.so, libjingle_peerconnection_so.so); org.moire.opensudoku (libc++_shared.so) | **unused**: libc++'s wide streams (std::wcout/wcerr) and WebRTC's copy of them; no wide console I/O in these apps. If reached: returns 0 (a phone returns the character or WEOF), nothing written. |
| `freelocale` | libc | 21 | stub: does nothing (windows/src/bionic/locale.cpp:62) | Frees a locale_t from newlocale. | org.jitsi.meet (libc++_shared.so, libjingle_peerconnection_so.so); org.moire.opensudoku (libc++_shared.so, libtensorflowlite_jni.so); org.ppsspp.ppsspp (libppsspp_jni.so) | **unused**: newlocale/duplocale return the one shared locale object (locale.cpp), so there is nothing to free. |
| `getegid` | libc | 21 | stub: returns 10100 (windows/src/bionic/misc.cpp:1325) | As getgid. | org.jitsi.meet (libjingle_peerconnection_so.so) | **unused**: Fixed 10100. |
| `geteuid` | libc | 21 | stub: returns 10100 (windows/src/bionic/misc.cpp:1325) | As getuid. | org.jitsi.meet (libjingle_peerconnection_so.so) | **unused**: Fixed 10100. |
| `getgid` | libc | 21 | stub: returns 10100 (windows/src/bionic/misc.cpp:1325) | The app's gid (same as its uid for an app). | org.jitsi.meet (libjingle_peerconnection_so.so) | **unused**: Fixed 10100. |
| `getpagesize` | libc | 21 | stub: returns 4096 (windows/src/bionic/misc.cpp:1495) | The page size: 4096 on x86_64; 4096 or 16384 on arm64 devices. | org.jitsi.meet (libhermes.so); org.moire.opensudoku (libtensorflowlite_jni.so); org.ppsspp.ppsspp (libppsspp_jni.so); se.voxelmanip.tensy (libtensy.so) | **unused**: 4096 is the host's real page size and what mmap uses here. |
| `getuid` | libc | 21 | stub: returns 10100 (windows/src/bionic/misc.cpp:1325) | The app's uid, 10000 + app id. | org.jitsi.meet (libjingle_peerconnection_so.so); org.moire.opensudoku (libtensorflowlite_jni.so) | **unused**: A fixed 10100: plausible and stable. |
| `getwc` | libc | 21 | missing -> 0-returning stub | Reads a wide character from a FILE. | org.jitsi.meet (libc++_shared.so, libjingle_peerconnection_so.so); org.moire.opensudoku (libc++_shared.so) | **unused**: libc++'s std::wcin buffer only. If reached: 0 is L'\0', not WEOF, so a read loop would spin on NULs instead of ending. |
| `openlog` | libc | 21 | stub: does nothing (windows/src/bionic/misc.cpp:2581) | Sets syslog's tag and options. | org.fossify.gallery (libavif_android.so, libglide-webp.so, libjxl.so, libjxl_cms.so, libjxl_threads.so, libjxlcoder.so); org.jitsi.meet (libc++_shared.so, libjingle_peerconnection_so.so, libjsi.so, libnative-imagetranscoder.so, libreactnative.so); org.moire.opensudoku (libc++_shared.so, libtensorflowlite_jni.so); org.ppsspp.ppsspp (libppsspp_jni.so) | **unused**: Bionic keeps only the tag; nothing observable. |
| `pthread_attr_destroy` | libc | 21 | stub: returns 0 (windows/src/bionic/pthread.cpp:540) | Frees nothing on Bionic either (attributes are plain data). | org.fossify.gallery (libavif_android.so, libjxlcoder.so); org.jitsi.meet (libhermes.so, libjingle_peerconnection_so.so); org.moire.opensudoku (libopencv_java5.so) | **unused**: Correct as it stands. |
| `pthread_cond_destroy` | libc | 21 | stub: returns 0 (windows/src/bionic/pthread.cpp:214) | Bionic: marks the condition destroyed; nothing to free. | org.fossify.gallery (libavif_android.so, libglide-webp.so, libjxl_threads.so, libjxlcoder.so, libpl_droidsonroids_gif.so); org.jellyfin.mobile (libffmpegJNI.so); org.jitsi.meet (libc++_shared.so, libjingle_peerconnection_so.so); org.moire.opensudoku (libc++_shared.so, libopencv_java5.so, libtensorflowlite_jni.so); org.ppsspp.ppsspp (libppsspp_jni.so); se.voxelmanip.tensy (libtensy.so) | **unused**: Win.apk's condition is a word in the guest's own struct; nothing to free. |
| `pthread_condattr_destroy` | libc | 21 | stub: returns 0 (windows/src/bionic/pthread.cpp:201) | Plain data; nothing to free. | org.jitsi.meet (libjingle_peerconnection_so.so) | **unused**: Correct as it stands. |
| `pthread_mutex_destroy` | libc | 21 | stub: returns 0 (windows/src/bionic/pthread.cpp:123) | Bionic: EBUSY if locked, else marks it destroyed. | org.fossify.gallery (libavif_android.so, libglide-webp.so, libjxl_threads.so, libjxlcoder.so, libpl_droidsonroids_gif.so); org.jellyfin.mobile (libffmpegJNI.so); org.jitsi.meet (libc++_shared.so, libjingle_peerconnection_so.so); org.moire.opensudoku (libc++_shared.so, libopencv_java5.so, libtensorflowlite_jni.so); org.ppsspp.ppsspp (libppsspp_jni.so); se.voxelmanip.tensy (libtensy.so) | **unused**: Win.apk's mutex is a word in the guest's struct; only the EBUSY check is missing. |
| `pthread_mutexattr_destroy` | libc | 21 | stub: returns 0 (windows/src/bionic/pthread.cpp:108) | Plain data; nothing to free. | org.fossify.gallery (libjxlcoder.so); org.jitsi.meet (libc++_shared.so, libjingle_peerconnection_so.so); org.moire.opensudoku (libc++_shared.so, libopencv_java5.so); org.ppsspp.ppsspp (libppsspp_jni.so) | **unused**: Correct as it stands. |
| `pthread_rwlock_destroy` | libc | 21 | stub: returns 0 (windows/src/bionic/pthread.cpp:248) | Bionic: EBUSY if held, else nothing to free. | org.jitsi.meet (libjingle_peerconnection_so.so, libjsi.so, libreactnative.so); se.voxelmanip.tensy (libtensy.so) | **unused**: As pthread_mutex_destroy. |
| `sched_setaffinity` | libc | 21 | stub: returns 0 (windows/src/bionic/misc.cpp:2495) | Pins a thread to CPUs. | org.fossify.gallery (libjxlcoder.so) | **unused**: Tuning only (libjxl's thread pool); 0 with no pinning is safe. |
| `sem_destroy` | libc | 21 | stub: returns 0 (windows/src/bionic/pthread.cpp:702) | Nothing to free for an unnamed semaphore. | org.jitsi.meet (libhermes.so, libjingle_peerconnection_so.so); se.voxelmanip.tensy (libtensy.so) | **unused**: Correct as it stands. |
| `ungetwc` | libc | 21 | missing -> 0-returning stub | Pushes a wide character back on a FILE. | org.jitsi.meet (libc++_shared.so, libjingle_peerconnection_so.so); org.moire.opensudoku (libc++_shared.so) | **unused**: libc++'s std::wcin buffer only. |
| `fegetenv` | libm | 21 | missing -> 0-returning stub | Saves the FP environment into *envp. | org.moire.opensudoku (libopencv_java5.so) | **unused**: OpenCV's bundled TBB captures the caller's FP settings for its workers (task_group_context_impl::capture_fp_settings, task_arena_impl::execute); the stub writes nothing and the matching fesetenv does nothing, so workers keep the default environment, which is what the app thread has anyway. |
| `fegetround` | libm | 21 | missing -> 0-returning stub | Returns the current rounding mode. | org.moire.opensudoku (libopencv_java5.so) | **unused**: OpenCV's dnn module saves the mode around a rounding step; 0 is FE_TONEAREST on arm64 and x86_64, which is the mode actually current. |
| `fesetenv` | libm | 21 | missing -> 0-returning stub | Restores the FP environment from *envp. | org.moire.opensudoku (libopencv_java5.so) | **unused**: TBB's restore of what fegetenv (a no-op here) saved: a no-op pair. |
| `AMediaCodec_{configure, delete, dequeueInputBuffer, dequeueOutputBuffer, getInputBuffer, getOutputBuffer, getOutputFormat, queueInputBuffer, releaseOutputBuffer, start, stop}` (11) | libmediandk | 21 | configure: stub: returns kErrInvalidObject (windows/host/native_media.cpp:203)<br>delete: stub: returns kErrInvalidObject (windows/host/native_media.cpp:203)<br>dequeueInputBuffer: stub: returns -1 (windows/host/native_media.cpp:204)<br>dequeueOutputBuffer: stub: returns -1 (windows/host/native_media.cpp:204)<br>getInputBuffer: stub: returns nullptr (windows/host/native_media.cpp:205)<br>getOutputBuffer: stub: returns nullptr (windows/host/native_media.cpp:205)<br>getOutputFormat: stub: returns nullptr (windows/host/native_media.cpp:205)<br>queueInputBuffer: stub: returns kErrInvalidObject (windows/host/native_media.cpp:203)<br>releaseOutputBuffer: stub: returns kErrInvalidObject (windows/host/native_media.cpp:203)<br>start: stub: returns kErrInvalidObject (windows/host/native_media.cpp:203)<br>stop: stub: returns kErrInvalidObject (windows/host/native_media.cpp:203) | A real hardware/software codec. | org.moire.opensudoku (libopencv_java5.so) | **unused**: OpenCV's videoio MediaNDK backend (VideoCapture/VideoWriter of a file); OpenSudoku uses OpenCV on camera frames, not video files. If reached: create* returns NULL ("no codec", native_media.cpp), which OpenCV reports as a failed open. |
| `AMediaExtractor_{advance, getSampleTime, getTrackCount, getTrackFormat, readSampleData, selectTrack, setDataSourceFd}` (7) | libmediandk | 21 | advance: stub: returns false (windows/host/native_media.cpp:217)<br>getSampleTime: stub: returns -1 (windows/host/native_media.cpp:218)<br>getTrackCount: stub: returns 0 (windows/host/native_media.cpp:216)<br>getTrackFormat: stub: returns nullptr (windows/host/native_media.cpp:205)<br>readSampleData: stub: returns -1 (windows/host/native_media.cpp:218)<br>selectTrack: stub: returns kErrUnsupported (windows/host/native_media.cpp:215)<br>setDataSourceFd: stub: returns kErrUnsupported (windows/host/native_media.cpp:215) | A real demuxer (MP4, WebM, ...). | org.moire.opensudoku (libopencv_java5.so) | **unused**: OpenCV's videoio MediaNDK backend; every source is refused (AMEDIA_ERROR_UNSUPPORTED), so VideoCapture.open fails cleanly if ever called. |
| `AMediaMuxer_{addTrack, delete, new, setOrientationHint, start, stop, writeSampleData}` (7) | libmediandk | 21 | missing -> 0-returning stub | Writes MP4/WebM/3GP files from encoded samples. | org.moire.opensudoku (libopencv_java5.so) | **unused**: OpenCV's MediaNDK VideoWriter only. If reached: AMediaMuxer_new's 0 is a NULL muxer that OpenCV checks, then later calls (addTrack/start) get 0 = AMEDIA_OK on NULL: no crash, no file. |

Imports no NDK map lists (so neither "missing" nor "stubbed" above):

- weak, 0 on a phone as here (optional hooks): `JNI_OnLoad_libavif_android` (org.fossify.gallery), `OPENSSL_memory_alloc` (org.jitsi.meet), `OPENSSL_memory_free` (org.jitsi.meet), `OPENSSL_memory_get_size` (org.jitsi.meet), `ZSTD_trace_compress_begin` (org.ppsspp.ppsspp), `ZSTD_trace_compress_end` (org.ppsspp.ppsspp), `ZSTD_trace_decompress_begin` (org.ppsspp.ppsspp), `ZSTD_trace_decompress_end` (org.ppsspp.ppsspp)
- org.jitsi.meet: `JSClassCreate`, `JSContextGetGlobalObject`, `JSEvaluateScript`, `JSGlobalContextCreateInGroup`, `JSGlobalContextRelease`, `JSGlobalContextRetain`, `JSObjectCallAsConstructor`, `JSObjectCallAsFunction`, `JSObjectCopyPropertyNames`, `JSObjectGetArrayBufferByteLength`, `JSObjectGetArrayBufferBytesPtr`, `JSObjectGetPrivate`, `JSObjectGetProperty`, `JSObjectGetPropertyAtIndex`, `JSObjectGetPropertyForKey`, `JSObjectGetPrototype`, `JSObjectHasProperty`, `JSObjectIsFunction`, `JSObjectMake`, `JSObjectMakeArray`, `JSObjectSetPrivate`, `JSObjectSetProperty`, `JSObjectSetPropertyAtIndex`, `JSObjectSetPropertyForKey`, `JSObjectSetPrototype`, `JSPropertyNameAccumulatorAddName`, `JSPropertyNameArrayGetCount`, `JSPropertyNameArrayGetNameAtIndex`, `JSPropertyNameArrayRelease`, `JSStringCreateWithUTF8CString`, `JSStringGetMaximumUTF8CStringSize`, `JSStringGetUTF8CString`, `JSStringIsEqual`, `JSStringRelease`, `JSStringRetain`, `JSValueGetType`, `JSValueGetTypedArrayType`, `JSValueIsArray`, `JSValueIsEqual`, `JSValueIsInstanceOfConstructor`, `JSValueIsObjectOfClass`, `JSValueIsUndefined`, `JSValueMakeBoolean`, `JSValueMakeNull`, `JSValueMakeNumber`, `JSValueMakeString`, `JSValueMakeSymbol`, `JSValueMakeUndefined`, `JSValueProtect`, `JSValueToBoolean`, `JSValueToNumber`, `JSValueToObject`, `JSValueToStringCopy`, `JSValueUnprotect`, `kJSClassDefinitionEmpty` -- from libjsc.so, which the app does not ship (the importing library cannot load on a phone either)

### Missing or stubbed public symbols no surveyed app imports

Grouped by library and API level; `Prefix_*` (n) stands for n names of one family. **Bold** levels are 34 (Android 14), 35 (15), 36 (16) and 37 (in android16-release's maps already), whose symbols are listed in full. Whole libraries that are absent (libvulkan, libcamera2ndk, libbinder_ndk, libamidi, libsync, libOpenMAXAL, libneuralnetworks, libicu) still "load": the name is a system library, so DT_NEEDED and dlopen succeed and every function is a 0-returning stub (dlsym: NULL).

#### libc
- level 21 (262): `__b64_ntop`, `__b64_pton`, `__dn_comp`, `__dn_count_labels`, `__dn_skipname`, `__fp_nquery`, `__fp_query`, `__fpclassify`, `__fpclassifyd`, `__fpclassifyf`, `__fpclassifyl`, `__hostalias`, `__isfinitef`, `__isfinitel`, `__isinff`, `__isinfl`, `__isnanf`, `__isnanl`, `__isnormal`, `__isnormalf`, `__isnormall`, `__loc_aton`, `__loc_ntoa`, `__p_cdname`, `__p_cdnname`, `__p_class`, `__p_class_syms`, `__p_fqname`, `__p_fqnname`, `__p_option`, `__p_query`, `__p_rcode`, `__p_secstodate`, `__p_time`, `__p_type`, `__p_type_syms`, `__putlong`, `__putshort`, `__res_close`, `__res_dnok`, `__res_hnok`, `__res_hostalias`, `__res_isourserver`, `__res_mailok`, `__res_nameinquery`, `__res_nclose`, `__res_ninit`, `__res_nmkquery`, `__res_nquery`, `__res_nquerydomain`, `__res_nsearch`, `__res_nsend`, `__res_ownok`, `__res_queriesmatch`, `__res_querydomain`, `__res_send`, `__res_send_setqhook`, `__res_send_setrhook`, `__sched_cpualloc`, `__sched_cpufree`, `__stack_chk_guard`, `__sym_ntop`, `__sym_ntos`, `__sym_ston`, `__system_property_serial`, `__system_property_wait_any`, `_getlong`, `_getshort`, `_resolv_delete_cache_for_net`, `_resolv_flush_cache_for_net`, `_resolv_set_nameservers_for_net`, `_tolower`, `_toupper`, `acct`, `at_quick_exit`, `bindresvport`, `brk`, `c16rtomb`, `c32rtomb`, `capget`, `capset`, `cfgetispeed`, `cfgetospeed`, `cfmakeraw`, `cfsetispeed`, `cfsetospeed`, `cfsetspeed`, `creat64`, `daemon`, `delete_module`, `dn_expand`, `endmntent`, `endservent`, `endutent`, `err`, `errx`, `ether_*` (4), `fchdir`, `ffs`, `fgetln`, `fgetpos`, `fgetwc`, `fgetws`, `flockfile`, `fpurge`, `fputws`, `fsetpos`, `fstatvfs64`, `ftok`, `ftrylockfile`, `funlockfile`, `fwide`, `fwprintf`, `fwscanf`, `getgrgid`, `getgrnam`, `getgrouplist`, `getgroups`, `gethostbyaddr`, `gethostent`, `getlogin`, `getmntent`, `getmntent_r`, `getnetbyaddr`, `getnetbyname`, `getpgid`, `getpgrp`, `getprotobyname`, `getprotobynumber`, `getpwnam`, `getresgid`, `getresuid`, `gets`, `getservbyname`, `getservbyport`, `getservent`, `getsid`, `getutent`, `getwchar`, `grantpt`, `herror`, `hstrerror`, `imaxabs`, `imaxdiv`, `inet_*` (6), `init_module`, `initgroups`, `insque`, `isfinite`, `isfinitef`, `isfinitel`, `isinfl`, `isnanl`, `isnormal`, `isnormalf`, `isnormall`, `klogctl`, `lfind`, `lsearch`, `mbrtoc16`, `mbrtoc32`, `mknodat`, `mktemp`, `nsdispatch`, `personality`, `posix_fadvise64`, `psiginfo`, `psignal`, `pthread_*` (8), `ptsname`, `pututline`, `putwc`, `putwchar`, `rand_r`, `readahead`, `remque`, `res_*` (4), `sbrk`, `sched_*` (3), `sem_*` (3), `setbuffer`, `setegid`, `seteuid`, `setfsgid`, `setfsuid`, `setgroups`, `setlinebuf`, `setlogmask`, `setmntent`, `setpgrp`, `setprogname`, `setregid`, `setresgid`, `setresuid`, `setreuid`, `setservent`, `setutent`, `sigblock`, `siginterrupt`, `sigsetmask`, `statvfs64`, `swscanf`, `sync`, `tempnam`, `timelocal`, `toascii`, `ttyname`, `utmpname`, `verr`, `verrx`, `vfwprintf`, `vfwscanf`, `vswprintf`, `vswscanf`, `vwarn`, `vwarnx`, `vwprintf`, `vwscanf`, `warn`, `warnx`, `wcpcpy`, `wcpncpy`, `wcscasecmp`, `wcscspn`, `wcsdup`, `wcsftime`, `wcsncasecmp`, `wcsncat`, `wcspbrk`, `wcsspn`, `wcstoimax`, `wcstok`, `wcstold_l`, `wcstoll_l`, `wcstoull_l`, `wcstoumax`, `wcswidth`, `wcwidth`, `wprintf`, `wscanf`
- level 22 (22): `ns_*` (22)
- level 23 (37): `__fbufsize`, `__flbf`, `__fpending`, `__fpurge`, `__freadable`, `__fsetlocking`, `__fwritable`, `__system_property_area_serial`, `_flushlbf`, `clock_getcpuclockid`, `error`, `error_*` (4), `fmemopen`, `get_*` (4), `login_tty`, `malloc_info`, `open_memstream`, `open_wmemstream`, `process_vm_readv`, `process_vm_writev`, `pthread_rwlockattr_getkind_np`, `pthread_rwlockattr_setkind_np`, `seekdir`, `sigqueue`, `strcasecmp_l`, `strerror_l`, `strncasecmp_l`, `telldir`, `wcscasecmp_l`, `wcsncasecmp_l`, `wmempcpy`
- level 24 (16): `adjtimex`, `clock_adjtime`, `fgetpos64`, `freopen64`, `fsetpos64`, `funopen64`, `lockf`, `lockf64`, `pthread_*` (7), `strchrnul`
- level 26 (38): `__system_property_wait`, `catclose`, `catgets`, `catopen`, `ctermid`, `endgrent`, `endpwent`, `futimes`, `futimesat`, `getdomainname`, `getgrent`, `getpwent`, `getsubopt`, `hasmntopt`, `lutimes`, `mallopt`, `msgctl`, `msgget`, `msgrcv`, `msgsnd`, `quotactl`, `semctl`, `semget`, `semop`, `semtimedop`, `setdomainname`, `setgrent`, `setpwent`, `sighold`, `sigignore`, `sigpause`, `sigrelse`, `sigset`, `sync_file_range`, `towctrans`, `towctrans_l`, `wctrans`, `wctrans_l`
- level 28 (57): `__freading`, `__free_hook`, `__fseterr`, `__fwriting`, `__malloc_hook`, `__memalign_hook`, `__realloc_hook`, `endhostent`, `endnetent`, `endprotoent`, `getlogin_r`, `getnetent`, `getprotoent`, `hcreate`, `hcreate_r`, `hdestroy`, `hdestroy_r`, `hsearch`, `hsearch_r`, `posix_*` (23), `pthread_*` (4), `sethostent`, `setnetent`, `setprotoent`, `signalfd64`, `swab`, `syncfs`, `wcsftime_l`, `wcstod_l`, `wcstof_l`, `wcstol_l`, `wcstoul_l`
- level 29 (13): `__res_randomid`, `android_*` (8), `getloadavg`, `pthread_sigqueue`, `reallocarray`, `timespec_get`
- level 30 (46): `_Unwind_Backtrace`, `_Unwind_DeleteException`, `_Unwind_FindEnclosingFunction`, `_Unwind_Find_FDE`, `_Unwind_ForcedUnwind`, `_Unwind_GetCFA`, `_Unwind_GetDataRelBase`, `_Unwind_GetGR`, `_Unwind_GetIP`, `_Unwind_GetIPInfo`, `_Unwind_GetLanguageSpecificData`, `_Unwind_GetRegionStart`, `_Unwind_GetTextRelBase`, `_Unwind_RaiseException`, `_Unwind_Resume`, `_Unwind_Resume_or_Rethrow`, `_Unwind_SetGR`, `_Unwind_SetIP`, `__deregister_frame`, `__register_frame`, `call_once`, `cnd_*` (6), `mtx_*` (6), `statx`, `thrd_*` (8), `tss_*` (4)
- level 31 (11): `__libc_get_static_tls_bounds`, `__libc_iterate_dynamic_tls`, `__libc_register_dynamic_tls_listeners`, `__libc_register_thread_exit_callback`, `android_reset_stack_guards`, `ffsl`, `ffsll`, `pidfd_*` (3), `process_madvise`
- level 33 (3): `backtrace`, `backtrace_symbols`, `backtrace_symbols_fd`
- **level 34** (5): `__freadahead`, `close_range`, `memset_explicit`, `posix_spawn_file_actions_addchdir_np`, `posix_spawn_file_actions_addfchdir_np`
- **level 35** (15): `_Fork`, `android_crash_detail_register`, `android_crash_detail_replace_data`, `android_crash_detail_replace_name`, `android_crash_detail_unregister`, `localtime_rz`, `mbsrtowcs_l`, `mktime_z`, `strerrorname_np`, `tcgetwinsize`, `tcsetwinsize`, `timespec_getres`, `tzalloc`, `tzfree`, `wcsrtombs_l`
- **level 36** (7): `lchmod`, `mseal`, `pthread_getaffinity_np`, `pthread_setaffinity_np`, `qsort_r`, `sig2str`, `str2sig`
- **level 37** (2): `sched_getattr`, `sched_setattr`
- stubbed: does nothing: `globfree`, `regfree`, `vsyslog`; returns 0: `chown`, `fchown`, `fchownat`, `killpg`, `lchown`, `nice`, `posix_fadvise`, `posix_madvise`, `pthread_atfork`, `pthread_attr_setinheritsched`, `pthread_condattr_setpshared`, `pthread_mutexattr_setpshared`, `pthread_spin_destroy`, `sched_getscheduler`, `setgid`, `setpgid`, `setrlimit`, `setrlimit64`, `setuid`, `timer_delete`, `timer_getoverrun`; returns 022: `umask`; returns 1: `getppid`, `regexec`; returns 36: `__libc_current_sigrtmin`; returns 64: `__libc_current_sigrtmax`; returns kProgname: `getprogname`; returns nullptr: `__system_property_find_nth`; returns {}: `mallinfo`

#### libm
- level 21 (80): `__fe_dfl_env`, `__signbit`, `__signbitf`, `__signbitl`, `acoshl`, `acosl`, `asinhl`, `asinl`, `atanhl`, `cbrtl`, `copysignl`, `coshl`, `drem`, `dremf`, `erfcl`, `erfl`, `expm1l`, `fdiml`, `fedisableexcept`, `feenableexcept`, `fegetexcept`, `fegetexceptflag`, `feholdexcept`, `feraiseexcept`, `fesetexceptflag`, `feupdateenv`, `finitef`, `fmal`, `fmaxl`, `fminl`, `frexpl`, `gamma`, `gamma_r`, `gammaf`, `gammaf_r`, `hypotl`, `ilogbl`, `j0`, `j0f`, `j1`, `j1f`, `jn`, `jnf`, `lgamma_r`, `lgammaf_r`, `lgammal`, `log1pl`, `logbl`, `modfl`, `nanl`, `nearbyintl`, `nextafterl`, `nexttoward`, `nexttowardf`, `nexttowardl`, `remainderl`, `remquo`, `remquof`, `remquol`, `rintl`, `scalb`, `scalbf`, `scalbln`, `scalblnf`, `scalblnl`, `scalbnl`, `signgam`, `significand`, `significandf`, `significandl`, `sincosl`, `sinhl`, `tanhl`, `tgammal`, `y0`, `y0f`, `y1`, `y1f`, `yn`, `ynf`
- level 23 (48): `cabs`, `cabsf`, `cabsl`, `cacos`, `cacosf`, `cacosh`, `cacoshf`, `carg`, `cargf`, `cargl`, `casin`, `casinf`, `casinh`, `casinhf`, `catan`, `catanf`, `catanh`, `catanhf`, `ccos`, `ccosf`, `ccosh`, `ccoshf`, `cexp`, `cexpf`, `cimag`, `cimagf`, `cimagl`, `conj`, `conjf`, `conjl`, `cproj`, `cprojf`, `cprojl`, `creal`, `crealf`, `creall`, `csin`, `csinf`, `csinh`, `csinhf`, `csqrt`, `csqrtf`, `csqrtl`, `ctan`, `ctanf`, `ctanh`, `ctanhf`, `lgammal_r`
- level 26 (19): `cacoshl`, `cacosl`, `casinhl`, `casinl`, `catanhl`, `catanl`, `ccoshl`, `ccosl`, `cexpl`, `clog`, `clogf`, `clogl`, `cpow`, `cpowf`, `cpowl`, `csinhl`, `csinl`, `ctanhl`, `ctanl`

#### libdl
- level 24 (1): `android_get_application_target_sdk_version`
- level 27 (3): `__cfi_shadow_size`, `__cfi_slowpath`, `__cfi_slowpath_diag`

#### libstdc++
- level 21 (1): `_ZSt7nothrow`

#### liblog
- level 21 (2): `__android_log_is_loggable`, `__android_log_is_loggable_len`
- level 30 (10): `__android_log_call_aborter`, `__android_log_default_aborter`, `__android_log_get_minimum_priority`, `__android_log_logd_logger`, `__android_log_set_aborter`, `__android_log_set_default_tag`, `__android_log_set_logger`, `__android_log_set_minimum_priority`, `__android_log_stderr_logger`, `__android_log_write_log_message`
- **level 37** (1): `__android_log_logd_logger_with_timestamp`

#### libz
- level 21 (17): `_dist_code`, `_length_code`, `_tr_align`, `_tr_flush_bits`, `_tr_flush_block`, `_tr_init`, `_tr_stored_block`, `_tr_tally`, `deflate_copyright`, `gz_error`, `gz_intmax`, `inflate_*` (3), `z_errmsg`, `zcalloc`, `zcfree`

#### libandroid
- level 21 (11): `AObbInfo_*` (4), `AObbScanner_getObbInfo`, `AStorageManager_*` (6)
- level 23 (2): `android_setprocnetwork`, `android_setsocknetwork`
- level 24 (3): `AChoreographer_*` (3)
- level 26 (15): `AHardwareBuffer_*` (11), `ASensorManager_*` (4)
- level 27 (1): `ASharedMemory_dupFromJava`
- level 28 (8): `ASurfaceTexture_*` (8)
- level 29 (53): `AChoreographer_postFrameCallback64`, `AChoreographer_postFrameCallbackDelayed64`, `AFontMatcher_*` (6), `AFont_*` (9), `AHardwareBuffer_*` (3), `ASurfaceControl_*` (3), `ASurfaceTransactionStats_*` (6), `ASurfaceTransaction_*` (17), `ASystemFontIterator_*` (3), `android_*` (4)
- level 30 (8): `AChoreographer_registerRefreshRateCallback`, `AChoreographer_unregisterRefreshRateCallback`, `ASurfaceTransaction_setFrameRate`, `AThermal_*` (5)
- level 31 (16): `AHardwareBuffer_getId`, `AKeyEvent_fromJava`, `AMotionEvent_fromJava`, `APermissionManager_checkPermission`, `ASurfaceControl_acquire`, `ASurfaceTransaction_*` (7), `AThermal_getThermalHeadroom`, `android_*` (3)
- level 33 (18): `AChoreographerFrameCallbackData_*` (6), `AChoreographer_postVsyncCallback`, `AInputQueue_fromJava`, `APerformanceHint_*` (6), `ASurfaceTransaction_setFrameTimeline`, `android_*` (3)
- **level 34** (7): `AConfiguration_getGrammaticalGender`, `AConfiguration_setGrammaticalGender`, `APerformanceHint_setThreads`, `ASurfaceControl_fromJava`, `ASurfaceTransaction_clearFrameRate`, `ASurfaceTransaction_fromJava`, `ASurfaceTransaction_setExtendedRangeBrightness`
- **level 35** (22): `AInputEvent_toJava`, `AInputReceiverCallbacks_create`, `AInputReceiverCallbacks_release`, `AInputReceiverCallbacks_setKeyEventCallback`, `AInputReceiverCallbacks_setMotionEventCallback`, `AInputReceiver_createBatchedInputReceiver`, `AInputReceiver_createUnbatchedInputReceiver`, `AInputReceiver_getInputTransferToken`, `AInputReceiver_release`, `AInputTransferToken_fromJava`, `AInputTransferToken_release`, `AInputTransferToken_toJava`, `APerformanceHint_reportActualWorkDuration2`, `APerformanceHint_setPreferPowerEfficiency`, `ASurfaceTransaction_setDesiredHdrHeadroom`, `AThermal_getThermalHeadroomThresholds`, `AWorkDuration_create`, `AWorkDuration_release`, `AWorkDuration_setActualCpuDurationNanos`, `AWorkDuration_setActualGpuDurationNanos`, `AWorkDuration_setActualTotalDurationNanos`, `AWorkDuration_setWorkPeriodStartTimestampNanos`
- **level 36** (49): `ACpuHeadroomParams_create`, `ACpuHeadroomParams_destroy`, `ACpuHeadroomParams_getCalculationType`, `ACpuHeadroomParams_getCalculationWindowMillis`, `ACpuHeadroomParams_setCalculationType`, `ACpuHeadroomParams_setCalculationWindowMillis`, `ACpuHeadroomParams_setTids`, `ADisplayLutsEntry_createEntry`, `ADisplayLutsEntry_destroy`, `ADisplayLutsEntry_getBuffer`, `ADisplayLutsEntry_getDimension`, `ADisplayLutsEntry_getSamplingKey`, `ADisplayLutsEntry_getSize`, `ADisplayLuts_create`, `ADisplayLuts_destroy`, `ADisplayLuts_setEntries`, `AGpuHeadroomParams_create`, `AGpuHeadroomParams_destroy`, `AGpuHeadroomParams_getCalculationType`, `AGpuHeadroomParams_getCalculationWindowMillis`, `AGpuHeadroomParams_setCalculationType`, `AGpuHeadroomParams_setCalculationWindowMillis`, `APerformanceHint_borrowSessionFromJava`, `APerformanceHint_createSessionUsingConfig`, `APerformanceHint_getMaxGraphicsPipelineThreadsCount`, `APerformanceHint_isFeatureSupported`, `APerformanceHint_notifyWorkloadIncrease`, `APerformanceHint_notifyWorkloadReset`, `APerformanceHint_notifyWorkloadSpike`, `APerformanceHint_setNativeSurfaces`, `ASessionCreationConfig_create`, `ASessionCreationConfig_release`, `ASessionCreationConfig_setGraphicsPipeline`, `ASessionCreationConfig_setNativeSurfaces`, `ASessionCreationConfig_setPreferPowerEfficiency`, `ASessionCreationConfig_setTargetWorkDurationNanos`, `ASessionCreationConfig_setTids`, `ASessionCreationConfig_setUseAutoTiming`, `ASurfaceTransaction_setBufferWithRelease`, `ASurfaceTransaction_setLuts`, `ASystemHealth_getCpuHeadroom`, `ASystemHealth_getCpuHeadroomCalculationWindowRange`, `ASystemHealth_getCpuHeadroomMinIntervalMillis`, `ASystemHealth_getGpuHeadroom`, `ASystemHealth_getGpuHeadroomCalculationWindowRange`, `ASystemHealth_getGpuHeadroomMinIntervalMillis`, `ASystemHealth_getMaxCpuHeadroomTidsSize`, `AThermal_registerThermalHeadroomListener`, `AThermal_unregisterThermalHeadroomListener`
- stubbed: does nothing: `AInputEvent_release`, `ANativeWindow_acquire`, `ATrace_beginAsyncSection`, `ATrace_beginSection`, `ATrace_endAsyncSection`, `ATrace_endSection`, `ATrace_setCounter`; returns -22: `ASensorEventQueue_enableSensor`, `ASensorEventQueue_registerSensor`, `ASensorEventQueue_requestAdditionalInfoEvents`, `ASensorEventQueue_setEventRate`; returns 0: `AConfiguration_diff`, `AConfiguration_isBetterThan`, `AMotionEvent_getClassification`, `AMotionEvent_getHistoricalEventTime`, `AMotionEvent_getHistorySize`, `ASensorEventQueue_disableSensor`, `ASensorEventQueue_getEvents`, `ASensorEventQueue_hasEvents`, `ASensor_getFifoMaxEventCount`, `ASensor_getFifoReservedEventCount`, `ASensor_getHandle`, `ASensor_getHighestDirectReportRateLevel`, `ASensor_getMinDelay`, `ASensor_getReportingMode`, `ASensor_getType`, `ASensor_isDirectChannelTypeSupported`, `ASensor_isWakeUpSensor`, `ASharedMemory_setProt`; returns 0.0f: `AMotionEvent_getHistoricalAxisValue`, `ASensor_getResolution`; returns 1: `AAsset_isAllocated`, `AConfiguration_getScreenRound`, `AConfiguration_match`; returns false: `ATrace_isEnabled`; returns nullptr: `ASensorManager_getDefaultSensor`, `ASensorManager_getDefaultSensorEx`, `ASensor_getName`, `ASensor_getStringType`, `ASensor_getVendor`

#### libjnigraphics
- level 21 (1): `AndroidBitmap_getDataSpace`
- level 30 (21): `AImageDecoderHeaderInfo_*` (6), `AImageDecoder_*` (13), `AndroidBitmap_compress`, `AndroidBitmap_getHardwareBuffer`
- level 31 (14): `AImageDecoderFrameInfo_*` (7), `AImageDecoder_*` (7)

#### libamidi (nothing implemented: all 14 missing)
- level 21 (13): `AMidiDevice_*` (5), `AMidiInputPort_*` (5), `AMidiOutputPort_*` (3)
- level 33 (1): `AMidiDevice_getDefaultProtocol`

#### libnativewindow
- level 21 (8): `AHardwareBuffer_*` (8)
- level 29 (3): `AHardwareBuffer_*` (3)
- level 31 (1): `AHardwareBuffer_getId`
- **level 34** (4): `AHardwareBuffer_readFromParcel`, `AHardwareBuffer_writeToParcel`, `ANativeWindow_readFromParcel`, `ANativeWindow_writeToParcel`
- stubbed: does nothing: `ANativeWindow_acquire`, `ANativeWindow_tryAllocateBuffers`; returns 0: `ANativeWindow_getBuffersDataSpace`, `ANativeWindow_getBuffersDefaultDataSpace` (**34**), `ANativeWindow_setBuffersDataSpace`, `ANativeWindow_setBuffersTransform`, `ANativeWindow_setFrameRate`, `ANativeWindow_setFrameRateWithChangeStrategy`

#### libEGL
- level 21 (5): `eglGetSystemTimeFrequencyNV`, `eglGetSystemTimeNV`, `eglLockSurfaceKHR`, `eglSignalSyncKHR`, `eglUnlockSurfaceKHR`
- level 23 (12): `eglCreateStreamFromFileDescriptorKHR`, `eglCreateStreamKHR`, `eglCreateStreamProducerSurfaceKHR`, `eglDestroyStreamKHR`, `eglGetStreamFileDescriptorKHR`, `eglQueryStreamKHR`, `eglQueryStreamTimeKHR`, `eglQueryStreamu64KHR`, `eglStreamAttribKHR`, `eglStreamConsumerAcquireKHR`, `eglStreamConsumerGLTextureExternalKHR`, `eglStreamConsumerReleaseKHR`
- level 24 (1): `eglCreateNativeClientBufferANDROID`
- level 26 (1): `eglGetNativeClientBufferANDROID`
- stubbed: returns EGL_TRUE: `eglPresentationTimeANDROID`, `eglSetDamageRegionKHR`

#### libGLESv1_CM
- level 21 (94): `gl*Bounds` (7), `gl*IMG` (2), `gl*OES` (85)

#### libGLESv3
- stubbed: does nothing: `glDebugMessageCallback`, `glDebugMessageCallbackKHR`

#### libvulkan (nothing implemented: all 251 missing)
- level 21 (148): the Vulkan 1.0 + Android WSI core entry points
- level 28 (34): the Vulkan 1.1 core entry points
- level 31 (13): the Vulkan 1.2 core entry points
- level 33 (37): the Vulkan 1.3 core entry points
- **level 34** (1): `vkGetImageSubresourceLayout2EXT`
- **level 36** (18): `vkCmdBindDescriptorSets2`, `vkCmdBindIndexBuffer2`, `vkCmdPushConstants2`, `vkCmdPushDescriptorSet`, `vkCmdPushDescriptorSet2`, `vkCmdPushDescriptorSetWithTemplate`, `vkCmdPushDescriptorSetWithTemplate2`, `vkCmdSetLineStipple`, `vkCmdSetRenderingAttachmentLocations`, `vkCmdSetRenderingInputAttachmentIndices`, `vkCopyImageToMemory`, `vkCopyMemoryToImage`, `vkGetDeviceImageSubresourceLayout`, `vkGetImageSubresourceLayout2`, `vkGetRenderingAreaGranularity`, `vkMapMemory2`, `vkTransitionImageLayout`, `vkUnmapMemory2`

#### libbinder_ndk (nothing implemented: all 150 missing)
- level 29 (89): `AIBinder_*` (26), `AParcel_*` (51), `AStatus_*` (12)
- level 30 (5): `AIBinder_getExtension`, `AIBinder_setExtension`, `AParcel_fromJavaParcel`, `AStatus_deleteDescription`, `AStatus_getDescription`
- level 31 (8): `AIBinder_*` (4), `AParcel_*` (4)
- level 33 (5): `AIBinder_*` (3), `AParcel_marshal`, `AParcel_unmarshal`
- **level 35** (41): `APersistableBundle_delete`, `APersistableBundle_dup`, `APersistableBundle_erase`, `APersistableBundle_getBoolean`, `APersistableBundle_getBooleanKeys`, `APersistableBundle_getBooleanVector`, `APersistableBundle_getBooleanVectorKeys`, `APersistableBundle_getDouble`, `APersistableBundle_getDoubleKeys`, `APersistableBundle_getDoubleVector`, `APersistableBundle_getDoubleVectorKeys`, `APersistableBundle_getInt`, `APersistableBundle_getIntKeys`, `APersistableBundle_getIntVector`, `APersistableBundle_getIntVectorKeys`, `APersistableBundle_getLong`, `APersistableBundle_getLongKeys`, `APersistableBundle_getLongVector`, `APersistableBundle_getLongVectorKeys`, `APersistableBundle_getPersistableBundle`, `APersistableBundle_getPersistableBundleKeys`, `APersistableBundle_getString`, `APersistableBundle_getStringKeys`, `APersistableBundle_getStringVector`, `APersistableBundle_getStringVectorKeys`, `APersistableBundle_isEqual`, `APersistableBundle_new`, `APersistableBundle_putBoolean`, `APersistableBundle_putBooleanVector`, `APersistableBundle_putDouble`, `APersistableBundle_putDoubleVector`, `APersistableBundle_putInt`, `APersistableBundle_putIntVector`, `APersistableBundle_putLong`, `APersistableBundle_putLongVector`, `APersistableBundle_putPersistableBundle`, `APersistableBundle_putString`, `APersistableBundle_putStringVector`, `APersistableBundle_readFromParcel`, `APersistableBundle_size`, `APersistableBundle_writeToParcel`
- **level 36** (2): `AIBinder_Class_getFunctionName`, `AIBinder_Class_setTransactionCodeToFunctionNameMap`

#### libsync (nothing implemented: all 3 missing)
- level 26 (3): `sync_*` (3)

#### libmediandk
- level 21 (39): `AMediaCodecCryptoInfo_*` (8), `AMediaCrypto_*` (4), `AMediaDrm_*` (23), `AMediaExtractor_*` (3), `AMediaMuxer_setLocation`
- level 24 (21): `AImageReader_*` (10), `AImage_*` (10), `AMediaCodecCryptoInfo_setPattern`
- level 26 (7): `AImageReader_*` (4), `AImage_deleteAsync`, `AImage_getHardwareBuffer`, `AMediaCodec_createPersistentInputSurface`
- level 28 (13): `AMediaCodecActionCode_isRecoverable`, `AMediaCodecActionCode_isTransient`, `AMediaCodec_*` (3), `AMediaDataSource_*` (6), `AMediaExtractor_getCachedDuration`, `AMediaExtractor_getSampleFormat`
- level 29 (5): `AMediaDataSource_*` (3), `AMediaDrm_setOnExpirationUpdateListener`, `AMediaDrm_setOnKeysChangeListener`
- level 31 (10): `AMEDIACODEC_*` (7), `AMediaMuxer_*` (3)
- level 33 (2): `AMediaCodec_setOnFrameRenderedCallback`, `AMediaDrm_getKeyRequestWithDefaultUrlAndType`
- **level 34** (2): `AImageReader_newWithDataSpace`, `AImage_getDataSpace`
- **level 36** (60): `ACodecAudioCapabilities_getBitrateRange`, `ACodecAudioCapabilities_getInputChannelCountRanges`, `ACodecAudioCapabilities_getMaxInputChannelCount`, `ACodecAudioCapabilities_getMinInputChannelCount`, `ACodecAudioCapabilities_getSupportedSampleRateRanges`, `ACodecAudioCapabilities_getSupportedSampleRates`, `ACodecAudioCapabilities_isSampleRateSupported`, `ACodecEncoderCapabilities_getComplexityRange`, `ACodecEncoderCapabilities_getQualityRange`, `ACodecEncoderCapabilities_isBitrateModeSupported`, `ACodecPerformancePoint_covers`, `ACodecPerformancePoint_coversFormat`, `ACodecPerformancePoint_create`, `ACodecPerformancePoint_destroy`, `ACodecPerformancePoint_equals`, `ACodecVideoCapabilities_areSizeAndRateSupported`, `ACodecVideoCapabilities_getAchievableFrameRatesFor`, `ACodecVideoCapabilities_getBitrateRange`, `ACodecVideoCapabilities_getHeightAlignment`, `ACodecVideoCapabilities_getNextSupportedPerformancePoint`, `ACodecVideoCapabilities_getSupportedFrameRates`, `ACodecVideoCapabilities_getSupportedFrameRatesFor`, `ACodecVideoCapabilities_getSupportedHeights`, `ACodecVideoCapabilities_getSupportedHeightsFor`, `ACodecVideoCapabilities_getSupportedWidths`, `ACodecVideoCapabilities_getSupportedWidthsFor`, `ACodecVideoCapabilities_getWidthAlignment`, `ACodecVideoCapabilities_isSizeSupported`, `AMediaCodecInfo_FEATURE_AdaptivePlayback`, `AMediaCodecInfo_FEATURE_DetachedSurface`, `AMediaCodecInfo_FEATURE_DynamicColorAspects`, `AMediaCodecInfo_FEATURE_DynamicTimestamp`, `AMediaCodecInfo_FEATURE_EncodingStatistics`, `AMediaCodecInfo_FEATURE_FrameParsing`, `AMediaCodecInfo_FEATURE_HdrEditing`, `AMediaCodecInfo_FEATURE_HlgEditing`, `AMediaCodecInfo_FEATURE_IntraRefresh`, `AMediaCodecInfo_FEATURE_LowLatency`, `AMediaCodecInfo_FEATURE_MultipleFrames`, `AMediaCodecInfo_FEATURE_PartialFrame`, `AMediaCodecInfo_FEATURE_QpBounds`, `AMediaCodecInfo_FEATURE_Roi`, `AMediaCodecInfo_FEATURE_SecurePlayback`, `AMediaCodecInfo_FEATURE_TunneledPlayback`, `AMediaCodecInfo_getAudioCapabilities`, `AMediaCodecInfo_getCanonicalName`, `AMediaCodecInfo_getEncoderCapabilities`, `AMediaCodecInfo_getKind`, `AMediaCodecInfo_getMaxSupportedInstances`, `AMediaCodecInfo_getMediaCodecInfoType`, `AMediaCodecInfo_getMediaType`, `AMediaCodecInfo_getVideoCapabilities`, `AMediaCodecInfo_isFeatureRequired`, `AMediaCodecInfo_isFeatureSupported`, `AMediaCodecInfo_isFormatSupported`, `AMediaCodecInfo_isVendor`, `AMediaCodecStore_findNextDecoderForFormat`, `AMediaCodecStore_findNextEncoderForFormat`, `AMediaCodecStore_getCodecInfo`, `AMediaCodecStore_getSupportedMediaTypes`
- stubbed: returns -1: `AMediaExtractor_getSampleSize`, `AMediaExtractor_getSampleTrackIndex`; returns kErrInvalidObject: `AMediaCodec_createInputSurface`, `AMediaCodec_flush`, `AMediaCodec_queueSecureInputBuffer`, `AMediaCodec_releaseOutputBufferAtTime`, `AMediaCodec_setAsyncNotifyCallback`, `AMediaCodec_setInputSurface`, `AMediaCodec_setOutputSurface`, `AMediaCodec_setParameters`, `AMediaCodec_signalEndOfInputStream`; returns kErrUnsupported: `AMediaExtractor_seekTo`, `AMediaExtractor_setDataSource`, `AMediaExtractor_setDataSourceCustom`, `AMediaExtractor_unselectTrack`; returns nullptr: `AMediaCodec_getBufferFormat`, `AMediaCodec_getInputFormat`, `AMediaExtractor_getFileFormat`

#### libcamera2ndk (nothing implemented: all 72 missing)
- level 21 (41): `ACameraCaptureSession_*` (6), `ACameraDevice_*` (4), `ACameraManager_*` (8), `ACameraMetadata_*` (4), `ACameraOutputTarget_create`, `ACameraOutputTarget_free`, `ACaptureRequest_*` (11), `ACaptureSessionOutputContainer_*` (4), `ACaptureSessionOutput_create`, `ACaptureSessionOutput_free`
- level 28 (8): `ACameraCaptureSession_updateSharedOutput`, `ACameraDevice_createCaptureSessionWithSessionParameters`, `ACaptureRequest_*` (3), `ACaptureSessionSharedOutput_*` (3)
- level 29 (15): `ACameraCaptureSession_logicalCamera_capture`, `ACameraCaptureSession_logicalCamera_setRepeatingRequest`, `ACameraDevice_createCaptureRequest_withPhysicalIds`, `ACameraDevice_isSessionConfigurationSupported`, `ACameraManager_registerExtendedAvailabilityCallback`, `ACameraManager_unregisterExtendedAvailabilityCallback`, `ACameraMetadata_isLogicalMultiCamera`, `ACaptureRequest_*` (7), `ACaptureSessionPhysicalOutput_create`
- level 30 (1): `ACameraMetadata_fromCameraMetadata`
- level 33 (4): `ACameraCaptureSession_*` (4)
- **level 34** (2): `ACameraCaptureSession_prepareWindow`, `ACameraCaptureSession_setWindowPreparedCallback`
- **level 35** (1): `ACameraMetadata_getTagFromName`

#### libaaudio
- level 21 (2): `AAudio_getMMapPolicy`, `AAudio_setMMapPolicy`
- level 29 (1): `AAudioStream_getAllowedCapturePolicy`
- level 30 (1): `AAudioStream_isPrivacySensitive`
- level 31 (2): `AAudioStreamBuilder_setAttributionTag`, `AAudioStreamBuilder_setPackageName`
- level 32 (5): `AAudioStreamBuilder_setIsContentSpatialized`, `AAudioStreamBuilder_setSpatializationBehavior`, `AAudioStream_*` (3)
- **level 34** (3): `AAudioStream_getHardwareChannelCount`, `AAudioStream_getHardwareFormat`, `AAudioStream_getHardwareSampleRate`
- **level 36** (8): `AAudioStreamBuilder_setPresentationEndCallback`, `AAudioStream_getDeviceIds`, `AAudioStream_getOffloadDelay`, `AAudioStream_getOffloadPadding`, `AAudioStream_setOffloadDelayPadding`, `AAudioStream_setOffloadEndOfStream`, `AAudio_getPlatformMMapExclusivePolicy`, `AAudio_getPlatformMMapPolicy`
- stubbed: does nothing: `AAudioStreamBuilder_setAllowedCapturePolicy`, `AAudioStreamBuilder_setContentType`, `AAudioStreamBuilder_setDeviceId`, `AAudioStreamBuilder_setErrorCallback`, `AAudioStreamBuilder_setPerformanceMode`, `AAudioStreamBuilder_setPrivacySensitive`, `AAudioStreamBuilder_setSessionId`, `AAudioStreamBuilder_setSharingMode`, `AAudioStreamBuilder_setUsage`; returns -1: `AAudioStream_getSessionId`; returns 0: `AAudioStream_getDeviceId`, `AAudioStream_getDirection`, `AAudioStream_getFramesRead`, `AAudioStream_getSharingMode`, `AAudioStream_getXRunCount`, `AAudioStream_read`; returns 1: `AAudioStream_getUsage`; returns 10: `AAudioStream_getPerformanceMode`; returns 2: `AAudioStream_getContentType`; returns false: `AAudioStream_isMMapUsed`

#### libOpenMAXAL (nothing implemented: all 44 missing)
- level 21 (44): `XA_*` (41), `xaCreateEngine`, `xaQueryNumSupportedEngineInterfaces`, `xaQuerySupportedEngineInterfaces`

#### libneuralnetworks (nothing implemented: all 69 missing)
- level 21 (24): `ANeuralNetworksCompilation_*` (4), `ANeuralNetworksEvent_free`, `ANeuralNetworksEvent_wait`, `ANeuralNetworksExecution_*` (7), `ANeuralNetworksMemory_createFromFd`, `ANeuralNetworksMemory_free`, `ANeuralNetworksModel_*` (9)
- level 29 (19): `ANeuralNetworksBurst_create`, `ANeuralNetworksBurst_free`, `ANeuralNetworksCompilation_createForDevices`, `ANeuralNetworksCompilation_setCaching`, `ANeuralNetworksDevice_*` (4), `ANeuralNetworksExecution_*` (6), `ANeuralNetworksMemory_createFromAHardwareBuffer`, `ANeuralNetworksModel_getSupportedOperationsForDevices`, `ANeuralNetworksModel_setOperandSymmPerChannelQuantParams`, `ANeuralNetworks_getDevice`, `ANeuralNetworks_getDeviceCount`
- level 30 (19): `ANeuralNetworksCompilation_setPriority`, `ANeuralNetworksCompilation_setTimeout`, `ANeuralNetworksDevice_wait`, `ANeuralNetworksEvent_createFromSyncFenceFd`, `ANeuralNetworksEvent_getSyncFenceFd`, `ANeuralNetworksExecution_*` (3), `ANeuralNetworksMemoryDesc_*` (6), `ANeuralNetworksMemory_copy`, `ANeuralNetworksMemory_createFromDesc`, `ANeuralNetworksModel_setOperandValueFromModel`, `ANeuralNetworks_getDefaultLoopTimeout`, `ANeuralNetworks_getMaximumLoopTimeout`
- level 31 (7): `ANeuralNetworksCompilation_*` (4), `ANeuralNetworksExecution_enableInputAndOutputPadding`, `ANeuralNetworksExecution_setReusable`, `ANeuralNetworks_getRuntimeFeatureLevel`

#### libicu (nothing implemented: all 306 missing)
- level 31 (270): `u_*` (108), `ubidi_*` (41), `ubrk_*` (17), `uenum_*` (7), `uldn_*` (14), `uloc_*` (36), `ulocdata_getCLDRVersion`, `unorm2_*` (19), `uscript_*` (11), `utext_*` (16)
- level 33 (36): `ucol_*` (25), `utrans_*` (11)

