./windows/scripts/build-windows-ucrt64-gstreamer.sh
python3 second_setting.py
./windows/scripts/stage-ucrt-runtime.sh >/dev/null
cp -a  build-windows-ucrt64/Win.apk.exe Win.apk-UCRT/Win.apk.exe
cp -a build-windows-ucrt64/winapk-runtime-dist/* Win.apk-UCRT/