@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" > nul
cd /d Z:\cd_mods\build_it\CDFlight
msbuild CDFlight.sln /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
