if not "%1"=="" (
    set out=%1
) else (
    set out=soque.exe
)
cl /c /Ox /Os /GL /GF /GS- /W4 /EHsc ../src/soque.cpp
rc -r soque.rc
link /LTCG soque.obj soque.res /subsystem:console /OUT:%out%
