@echo off
if "%1" == "clean" goto clean
wcl -wx -we -e=1 -ms -0 -q -d0 -s -otxh -fi=malloc.h -fe=bluenano.exe bluems*.c arch_dos.c z80.c
goto end
:clean
if exist *.obj del *.obj
if exist bluenano.exe del bluenano.exe
:end

