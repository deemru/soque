if not "%1"=="" (
    set OUT=%1
) else (
    set OUT=soque
)

cl /c /Ox /Os /GL /GF /GS- /W4 /EHsc ../src/soque.cpp
cl /c /Ox /Os /GL /GF /GS- /W4 /EHsc /I../src ../examples/soque_test.c
rc -r soque.rc

link /DLL /LTCG soque.obj soque.res /OUT:%OUT%.dll
link /LTCG soque_test.obj soque.res %OUT%.lib /subsystem:console /OUT:%OUT%_test.exe
