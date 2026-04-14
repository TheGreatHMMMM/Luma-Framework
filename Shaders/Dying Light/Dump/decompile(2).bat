@echo off
for %%f in (*.cso) do (
    echo Decompiling %%f...
    cmd_Decompiler.exe -D "%%f"
)
echo Done!
pause
