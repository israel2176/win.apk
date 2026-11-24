// Bodies for Robolectric's libandroidfw port where it departs from AssetManager2's C++ and the
// departure breaks AOSP's own callers in Win.apk (AospResRewrite.java points the port's method here).
// In the port's own package, to read its package-private state as the C++ reads its members.
package org.robolectric.res.android;

import java.util.Set;

public final class PortFixes {
    private PortFixes() {}

    /** LoadedPackage::CollectConfigurations (LoadedArsc.cpp, Android 14): every configuration of every
     *  type the package has, mipmaps left out when asked. The port walked the type map by the indexes
     *  0..size-1 rather than by its keys (type ids, from 1, with gaps) and dereferenced the missing
     *  ones, so every call threw (AssetManager.getSizeConfigurations, which ActivityThread reports at
     *  each activity launch). */
    public static void collectConfigurations(LoadedArsc.LoadedPackage pkg, boolean excludeMipmap,
                                             Set<ResTable_config> out) {
        for (LoadedArsc.TypeSpec typeSpec : pkg.type_specs_.values()) {
            if (typeSpec == null) continue;
            if (excludeMipmap) {
                int typeIndex = typeSpec.type_spec.id - 1;
                Ref<Integer> len = new Ref<>(0);
                String name = pkg.type_string_pool_.stringAt(typeIndex, len);
                if (name == null) name = pkg.type_string_pool_.string8At(typeIndex, len);
                if ("mipmap".equals(name)) continue;
            }
            for (ResourceTypes.ResTable_type type : typeSpec.types) {
                out.add(ResTable_config.fromDtoH(type.config));
            }
        }
    }
}
