python3 first_setting.py
./windows/scripts/build-windows-clang.sh
python3 second_setting.py
./windows/scripts/stage-ucrt-runtime.sh >/dev/null
cp -a build-windows-clang/Win.apk.exe Win.apk-UCRT/Win.apk.exe