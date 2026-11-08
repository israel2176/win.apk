// A stand-in for a translation (<lib>.so.aot.dll) in aot_cache_test.cpp: a
// function to call once it is loaded, and 8 MB of data so a copy of it takes
// long enough to be killed half way.

extern "C" __declspec(dllexport) int winapk_aot_test_value() { return 42; }

extern "C" __declspec(dllexport) const unsigned char winapk_aot_test_blob[8 << 20] = {1, 2, 3, 4};
