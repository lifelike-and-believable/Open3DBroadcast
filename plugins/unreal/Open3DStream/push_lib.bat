@ECHO OFF

REM This script copies the most recent built .lib and .h files to a ue plugin's lib dir

REM The top level project dir:
SET PROJECT=D:\P4_PD\o3ds

SET DST=%PROJECT%\Plugins\Open3DStream

SET BLD=%~DP0..\..\..\vsbuild

IF EXIST "%DST%\Open3DStream.uplugin" GOTO POK
ECHO Invalid destination dir: %DST%
PAUSE
EXIT /B

:POK

IF EXIST "%DST%" GOTO DSTOK

ECHO could not find unreal project
PAUSE
EXIT /B

:DSTOK

IF NOT EXIST "%DST%"\lib MKDIR "%DST%"\lib
IF NOT EXIST "%DST%"\lib\include MKDIR "%DST%"\lib\include
IF NOT EXIST "%DST%"\lib\include\o3ds MKDIR "%DST%"\lib\include\o3ds
IF NOT EXIST "%DST%"\lib\include\nng MKDIR "%DST%"\lib\include\nng
IF NOT EXIST "%DST%"\lib\include\nng MKDIR "%DST%"\lib\include\flatbuffers

COPY "%BLD%\src\RelWithDebInfo\open3dstreamstatic.lib"  "%DST%"\lib
COPY "%BLD%\src\RelWithDebInfo\open3dstreamstatic.pdb"  "%DST%"\lib

COPY "%~DP0..\..\..\thirdparty\build\nng\RelWithDebInfo\nng.lib" "%DST%"\lib
COPY "%~DP0..\..\..\thirdparty\build\flatbuffers\RelWithDebInfo\flatbuffers.lib" "%DST%"\lib

REM WebRTC Support - libdatachannel
IF EXIST "%~DP0..\..\..\thirdparty\build\libdatachannel\RelWithDebInfo\datachannel-static.lib" (
    ECHO Copying WebRTC libraries...
    COPY "%~DP0..\..\..\thirdparty\build\libdatachannel\RelWithDebInfo\datachannel-static.lib" "%DST%"\lib\datachannel.lib
    IF EXIST "%~DP0..\..\..\thirdparty\build\libdatachannel\RelWithDebInfo\datachannel-static.pdb" (
        COPY "%~DP0..\..\..\thirdparty\build\libdatachannel\RelWithDebInfo\datachannel-static.pdb" "%DST%"\lib\datachannel.pdb
    )
) ELSE (
    ECHO WebRTC libraries not found - skipping
)

COPY "%~DP0..\..\..\src\o3ds\*.h" "%DST%\lib\include\o3ds\"
XCOPY /S /I /Y "%~DP0..\..\..\usr\include\nng" "%DST%\lib\include\nng\"
XCOPY /S /I /Y "%~DP0..\..\..\usr\include\flatbuffers" "%DST%\lib\include\flatbuffers\"
COPY "%~DP0..\..\..\usr\include\*.h" "%DST%\lib\include\"

REM WebRTC headers
IF EXIST "%~DP0..\..\..\thirdparty\libdatachannel\include\rtc" (
    ECHO Copying WebRTC headers...
    XCOPY /S /I /Y "%~DP0..\..\..\thirdparty\libdatachannel\include\rtc" "%DST%\lib\include\rtc\"
) ELSE (
    ECHO WebRTC headers not found - skipping
)

pause