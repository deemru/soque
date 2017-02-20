if not "%1"=="" (
    set OUT=%1
) else (
    set OUT=soque
)

set DATETIMEVERSION=%DATE:~3,1%
if "%DATETIMEVERSION%" == " " (
:: en-us
    set DATETIMEVERSION=%DATE:~10,4%,%DATE:~4,2%,%DATE:~7,2%,%TIME:~0,2%%TIME:~3,2%
) else (
:: ru-ru
    set DATETIMEVERSION=%DATE:~6,4%,%DATE:~3,2%,%DATE:~0,2%,%TIME:~0,2%%TIME:~3,2%
)

( echo #define DATETIMEVERSION %DATETIMEVERSION%) > soque_ver.rc

cl /c /O2 /GL /GS- /W4 /EHsc ../src/soque.cpp
cl /c /O2 /GL /GS- /W4 /EHsc /I../src ../examples/soque_test.c
rc -r soque.rc

link /DLL /LTCG soque.obj soque.res /OUT:%OUT%.dll
link /LTCG soque_test.obj soque.res %OUT%.lib /subsystem:console /OUT:%OUT%_test.exe
