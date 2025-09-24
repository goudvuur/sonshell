```bash
SDK="$HOME/Projects/a6700/sdk/CrSDK_v2.00.00_20250805a_Linux64PC"
mkdir build
cd build
cmake .. -DSONY_SDK_DIR="$SDK"
python3 ../tools/gen_prop_names.py --header "${SDK}/app/CRSDK/CrDeviceProperty.h" -o gen/prop_names_generated.h
python3 ../tools/gen_error_names.py --header "${SDK}/app/CRSDK/CrError.h" -o gen/error_names_generated.h
cmake --build .
```
