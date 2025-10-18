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

IF NOT EXIST "%DST%"\ThirdParty MKDIR "%DST%"\ThirdParty
IF NOT EXIST "%DST%"\ThirdParty\include MKDIR "%DST%"\ThirdParty\include
IF NOT EXIST "%DST%"\ThirdParty\include\o3ds MKDIR "%DST%"\ThirdParty\include\o3ds
IF NOT EXIST "%DST%"\ThirdParty\include\nng MKDIR "%DST%"\ThirdParty\include\nng
IF NOT EXIST "%DST%"\ThirdParty\include\nng MKDIR "%DST%"\ThirdParty\include\flatbuffers

COPY "%BLD%\src\RelWithDebInfo\open3dstreamstatic.lib"  "%DST%"\ThirdParty
COPY "%BLD%\src\RelWithDebInfo\open3dstreamstatic.pdb"  "%DST%"\ThirdParty

COPY "%~DP0..\..\..\thirdparty\build\nng\RelWithDebInfo\nng.lib" "%DST%"\ThirdParty
COPY "%~DP0..\..\..\thirdparty\build\flatbuffers\RelWithDebInfo\flatbuffers.lib" "%DST%"\ThirdParty

REM WebRTC Support - libdatachannel
IF EXIST "%~DP0..\..\..\thirdparty\build\libdatachannel\RelWithDebInfo\datachannel-static.lib" (
    ECHO Copying WebRTC libraries...
    COPY "%~DP0..\..\..\thirdparty\build\libdatachannel\RelWithDebInfo\datachannel-static.lib" "%DST%"\ThirdParty\datachannel.lib
    IF EXIST "%~DP0..\..\..\thirdparty\build\libdatachannel\RelWithDebInfo\datachannel-static.pdb" (
        COPY "%~DP0..\..\..\thirdparty\build\libdatachannel\RelWithDebInfo\datachannel-static.pdb" "%DST%"\ThirdParty\datachannel.pdb
    )
) ELSE (
    ECHO WebRTC libraries not found - skipping
)

COPY "%~DP0..\..\..\src\o3ds\*.h" "%DST%\ThirdParty\include\o3ds\"
XCOPY /S /I /Y "%~DP0..\..\..\usr\include\nng" "%DST%\ThirdParty\include\nng\"
XCOPY /S /I /Y "%~DP0..\..\..\usr\include\flatbuffers" "%DST%\ThirdParty\include\flatbuffers\"
COPY "%~DP0..\..\..\usr\include\*.h" "%DST%\ThirdParty\include\"

REM WebRTC headers
IF EXIST "%~DP0..\..\..\thirdparty\libdatachannel\include\rtc" (
    ECHO Copying WebRTC headers...
    XCOPY /S /I /Y "%~DP0..\..\..\thirdparty\libdatachannel\include\rtc" "%DST%\ThirdParty\include\rtc\"
) ELSE (
    ECHO WebRTC headers not found - skipping
)

pause