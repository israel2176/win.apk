// ISafetyCenterManager in this process ("safety_center"), what AOSP's SafetyCenterManager asks
// system_server's SafetyCenterService (packages/modules/Permission/service/.../safetycenter). Safety Center
// is off here, as on a phone whose config disables it: AOSP's service then answers every call as this does
// -- isSafetyCenterEnabled false, a safety source's data dropped (setSafetySourceData, reportSafetySourceError
// return without effect), nothing to read back (getSafetySourceData, getSafetyCenterConfig and
// getSafetyCenterData null), no listeners kept. The methods not declared here get their type's empty answer
// from the generated stub (AospAppRewrite.java), which is that same answer. Google Play reports Play Protect's
// state through it (issue #208: without the class Play died at every start).
package com.winapk.aospapp;

public final class SafetyCenterService {
    /** AOSP SafetyCenterService.isSafetyCenterEnabled: the device's config and flag; off here. */
    public boolean isSafetyCenterEnabled() { return false; }
}
