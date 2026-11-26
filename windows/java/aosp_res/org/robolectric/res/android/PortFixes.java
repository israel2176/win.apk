// Bodies for Robolectric's libandroidfw port where it departs from AssetManager2's C++ and the
// departure breaks AOSP's own callers in Win.apk (AospResRewrite.java points the port's method here).
// In the port's own package, to read its package-private state as the C++ reads its members.
package org.robolectric.res.android;

import java.nio.ByteBuffer;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;
import java.util.WeakHashMap;

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

    /** A package's names as LoadedPackage::FindEntryByName reads them, each pool decoded once: the
     *  index of every type and key string, and per type the entry index of every key it holds. The
     *  port's FindEntryByName found the type and key with ResStringPool.indexOfString, which on an
     *  unsorted pool (aapt2 writes them unsorted) decodes every string from the back, then walked
     *  every entry of every configuration of the type: a getIdentifier cost the pool's size, and an
     *  app that looks up each of its R fields by name (R-init, ~22k for a bank app with two pools of
     *  ~10k keys) waited minutes. The C++ does the same walk over memory it only compares; here a
     *  lookup is two hash hits, built at the package's first. */
    private static final class Names {
        final Map<String, Integer> types = new HashMap<>();
        final Map<String, Integer> keys = new HashMap<>();
        /** type index -> key index -> entry index (the first configuration that has the key, as the
         *  C++'s walk returns it). */
        final Map<Integer, Map<Integer, Integer>> entries = new HashMap<>();
    }

    private static final Map<LoadedArsc.LoadedPackage, Names> NAMES = new WeakHashMap<>();

    /** ResStringPool::indexOfString for every string at once: an unsorted pool is searched from the
     *  back, so of two equal strings the last index wins; a sorted pool's are not repeated. */
    private static void indexPool(ResStringPool pool, Map<String, Integer> out) {
        int n = pool.size();
        for (int i = 0; i < n; i++) {
            String s = pool.stringAt(i);
            if (s != null) out.put(s, i);
        }
    }

    private static Map<Integer, Integer> indexType(LoadedArsc.LoadedPackage pkg, int typeIdx) {
        Map<Integer, Integer> keys = new HashMap<>();
        LoadedArsc.TypeSpec spec = pkg.type_specs_.get(typeIdx);
        if (spec == null) return keys;
        for (ResourceTypes.ResTable_type type : spec.types) {
            ByteBuffer buf = type.myBuf();
            int base = type.myOffset();
            int offsets = base + Short.toUnsignedInt(type.header.headerSize);
            boolean sparse = (type.flags & ResourceTypes.ResTable_type.FLAG_SPARSE) != 0;
            for (int i = 0; i < type.entryCount; i++) {
                int offset;
                int resIdx;
                if (sparse) {
                    // ResTable_sparseTypeEntry {uint16_t idx; uint16_t offset;}, after the header.
                    int at = offsets + i * ResourceTypes.ResTable_sparseTypeEntry.SIZEOF;
                    resIdx = Short.toUnsignedInt(buf.getShort(at));
                    offset = Short.toUnsignedInt(buf.getShort(at + 2)) * 4;
                } else {
                    offset = type.entryOffset(i);
                    resIdx = i;
                }
                if (offset == ResourceTypes.ResTable_type.NO_ENTRY) continue;
                // ResTable_entry::key(): a compact entry's uint16_t key, else its ResStringPool_ref.
                int at = base + type.entriesStart + offset;
                int flags = Short.toUnsignedInt(buf.getShort(at + 2));
                int key = (flags & ResourceTypes.ResTable_entry.FLAG_COMPACT) != 0
                        ? Short.toUnsignedInt(buf.getShort(at)) : buf.getInt(at + 4);
                keys.putIfAbsent(key, resIdx);
            }
        }
        return keys;
    }

    /** LoadedPackage::FindEntryByName (LoadedArsc.cpp, Android 14): the partial id (package 0x00) of
     *  the entry of that type and name, 0 when there is none. */
    public static int findEntryByName(LoadedArsc.LoadedPackage pkg, String typeName, String entryName) {
        if (typeName == null || entryName == null) return 0;
        Map<Integer, Integer> byKey;
        Integer keyIdx;
        synchronized (NAMES) {
            Names names = NAMES.get(pkg);
            if (names == null) {
                names = new Names();
                indexPool(pkg.type_string_pool_, names.types);
                indexPool(pkg.key_string_pool_, names.keys);
                NAMES.put(pkg, names);
            }
            Integer typeIdx = names.types.get(typeName);
            if (typeIdx == null) return 0;
            keyIdx = names.keys.get(entryName);
            if (keyIdx == null) return 0;
            byKey = names.entries.get(typeIdx);
            if (byKey == null) {
                byKey = indexType(pkg, typeIdx);
                names.entries.put(typeIdx, byKey);
            }
            Integer resIdx = byKey.get(keyIdx);
            if (resIdx == null) return 0;
            int typeId = (typeIdx + pkg.type_id_offset_ + 1) & 0xff;
            return (typeId << 16) | (resIdx & 0xffff);
        }
    }
}
