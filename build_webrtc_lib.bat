@ECHO OFF
REM ============================================================================
REM Build datachannel.lib for WebRTC support
REM This script builds libdatachannel with OpenSSL and copies the result
REM to the Unreal plugin directory for pre-built library usage.
REM ============================================================================

SETLOCAL EnableDelayedExpansion

ECHO.
ECHO ========================================================================
ECHO Building datachannel.lib for WebRTC Support
ECHO ========================================================================
ECHO.

REM Set paths
SET SCRIPT_DIR=%~dp0
SET THIRDPARTY_DIR=%SCRIPT_DIR%thirdparty
SET BUILD_DIR=%THIRDPARTY_DIR%\build_webrtc
SET PLUGIN_LIB_DIR=%SCRIPT_DIR%plugins\unreal\Open3DStream\ThirdParty\webrtc
SET INSTALL_DIR=%SCRIPT_DIR%usr_webrtc

REM Check for Visual Studio 2022
SET VS_VERSION="Visual Studio 17 2022"
WHERE cmake >nul 2>nul
IF %ERRORLEVEL% NEQ 0 (
    ECHO [ERROR] CMake not found in PATH!
    ECHO Please install CMake or ensure it's in your PATH.
    EXIT /B 1
)

ECHO [1/7] Checking for OpenSSL...
ECHO.

REM Check for OpenSSL using CMake's find_package
cmake -P check_openssl.cmake >nul 2>nul
IF %ERRORLEVEL% NEQ 0 (
    ECHO [WARNING] OpenSSL not found automatically.
    ECHO.
    ECHO Please install OpenSSL using one of these methods:
    ECHO.
    ECHO   Option 1 - vcpkg ^(Recommended^):
    ECHO     vcpkg install openssl:x64-windows
    ECHO     SET CMAKE_TOOLCHAIN_FILE=^<vcpkg-root^>/scripts/buildsystems/vcpkg.cmake
    ECHO.
    ECHO   Option 2 - Chocolatey:
    ECHO     choco install openssl
    ECHO.
    ECHO   Option 3 - Manual download:
    ECHO     Download from: https://slproweb.com/products/Win32OpenSSL.html
    ECHO     Install Win64 OpenSSL v3.x
    ECHO.
    ECHO Then set OPENSSL_ROOT_DIR to point to the installation directory.
    ECHO.
    
    REM Try common OpenSSL locations
    SET OPENSSL_CANDIDATES=C:\Program Files\OpenSSL-Win64;C:\Program Files\OpenSSL;C:\OpenSSL-Win64
    
    FOR %%P IN (%OPENSSL_CANDIDATES%) DO (
        IF EXIST "%%P\include\openssl\ssl.h" (
            ECHO Found OpenSSL at: %%P
            SET OPENSSL_ROOT_DIR=%%P
            GOTO :openssl_found
        )
    )
    
    ECHO.
    ECHO You can also provide OpenSSL path as argument:
    ECHO   %~nx0 "C:\path\to\openssl"
    ECHO.
    
    IF "%~1"=="" (
        ECHO [ERROR] OpenSSL not found. Please install or specify path.
        EXIT /B 1
    ) ELSE (
        IF NOT EXIST "%~1\include\openssl\ssl.h" (
            ECHO [ERROR] Invalid OpenSSL path: %~1
            EXIT /B 1
        )
        SET OPENSSL_ROOT_DIR=%~1
        ECHO Using provided OpenSSL path: !OPENSSL_ROOT_DIR!
    )
)

:openssl_found
ECHO [✓] OpenSSL ready
IF DEFINED OPENSSL_ROOT_DIR (
    ECHO     Location: !OPENSSL_ROOT_DIR!
)
ECHO.

ECHO [2/7] Initializing libdatachannel submodules...
CD "%THIRDPARTY_DIR%\libdatachannel"
IF NOT EXIST "deps\plog\CMakeLists.txt" (
    ECHO     Initializing dependencies...
    git submodule update --init --recursive
    IF %ERRORLEVEL% NEQ 0 (
        ECHO [ERROR] Failed to initialize submodules!
        CD "%SCRIPT_DIR%"
        EXIT /B 1
    )
    ECHO     [✓] Submodules initialized
) ELSE (
    ECHO     [✓] Submodules already initialized
)
CD "%SCRIPT_DIR%"
ECHO.

ECHO [3/7] Creating build directory...
IF EXIST "%BUILD_DIR%" (
    ECHO     Cleaning previous build...
    RMDIR /S /Q "%BUILD_DIR%"
)
MKDIR "%BUILD_DIR%"
ECHO [✓] Build directory ready
ECHO.

ECHO [4/7] Configuring CMake for libdatachannel...
ECHO.
CD "%BUILD_DIR%"

SET CMAKE_ARGS=-G %VS_VERSION% -A x64
SET CMAKE_ARGS=%CMAKE_ARGS% -DCMAKE_INSTALL_PREFIX="%INSTALL_DIR%"
SET CMAKE_ARGS=%CMAKE_ARGS% -DNO_EXAMPLES=ON
SET CMAKE_ARGS=%CMAKE_ARGS% -DNO_TESTS=ON
SET CMAKE_ARGS=%CMAKE_ARGS% -DUSE_NICE=OFF
SET CMAKE_ARGS=%CMAKE_ARGS% -DNO_WEBSOCKET=OFF
SET CMAKE_ARGS=%CMAKE_ARGS% -DNO_MEDIA=ON
SET CMAKE_ARGS=%CMAKE_ARGS% -DUSE_GNUTLS=OFF
SET CMAKE_ARGS=%CMAKE_ARGS% -DUSE_MBEDTLS=OFF

IF DEFINED OPENSSL_ROOT_DIR (
    SET CMAKE_ARGS=%CMAKE_ARGS% -DOPENSSL_ROOT_DIR="%OPENSSL_ROOT_DIR%"
)

IF DEFINED CMAKE_TOOLCHAIN_FILE (
    SET CMAKE_ARGS=%CMAKE_ARGS% -DCMAKE_TOOLCHAIN_FILE=%CMAKE_TOOLCHAIN_FILE%
)

cmake %CMAKE_ARGS% ..\libdatachannel
IF %ERRORLEVEL% NEQ 0 (
    ECHO [ERROR] CMake configuration failed!
    CD "%SCRIPT_DIR%"
    EXIT /B 1
)
ECHO [✓] CMake configuration complete
ECHO.

ECHO [5/7] Building libdatachannel ^(Debug^)...
cmake --build . --config Debug --target datachannel-static
IF %ERRORLEVEL% NEQ 0 (
    ECHO [ERROR] Debug build failed!
    CD "%SCRIPT_DIR%"
    EXIT /B 1
)
ECHO [✓] Debug build complete
ECHO.

ECHO [6/7] Building libdatachannel ^(RelWithDebInfo^)...
cmake --build . --config RelWithDebInfo --target datachannel-static
IF %ERRORLEVEL% NEQ 0 (
    ECHO [ERROR] RelWithDebInfo build failed!
    CD "%SCRIPT_DIR%"
    EXIT /B 1
)
ECHO [✓] RelWithDebInfo build complete
ECHO.

ECHO [7/7] Copying libraries to plugin directory...
IF NOT EXIST "%PLUGIN_LIB_DIR%" MKDIR "%PLUGIN_LIB_DIR%"
IF NOT EXIST "%PLUGIN_LIB_DIR%\include" MKDIR "%PLUGIN_LIB_DIR%\include"

REM Copy the library files
IF EXIST "RelWithDebInfo\datachannel-static.lib" (
    COPY /Y "RelWithDebInfo\datachannel-static.lib" "%PLUGIN_LIB_DIR%\datachannel.lib"
    ECHO     [✓] Copied datachannel.lib
) ELSE (
    ECHO     [ERROR] RelWithDebInfo\datachannel-static.lib not found!
    CD "%SCRIPT_DIR%"
    EXIT /B 1
)

IF EXIST "Debug\datachannel-static.lib" (
    COPY /Y "Debug\datachannel-static.lib" "%PLUGIN_LIB_DIR%\datachanneld.lib"
    ECHO     [✓] Copied datachanneld.lib ^(debug^)
)

REM Copy headers
ECHO     Copying headers...
XCOPY /Y /E /I "..\libdatachannel\include\rtc" "%PLUGIN_LIB_DIR%\include\rtc\" >nul
IF %ERRORLEVEL% EQU 0 (
    ECHO     [✓] Copied headers
) ELSE (
    ECHO     [WARNING] Failed to copy some headers
)

CD "%SCRIPT_DIR%"
ECHO [✓] Files copied to plugin directory
ECHO.

ECHO [8/8] Creating build info file...
ECHO datachannel.lib Build Information > "%PLUGIN_LIB_DIR%\BUILD_INFO.txt"
ECHO ======================================= >> "%PLUGIN_LIB_DIR%\BUILD_INFO.txt"
ECHO. >> "%PLUGIN_LIB_DIR%\BUILD_INFO.txt"
ECHO Build Date: %DATE% %TIME% >> "%PLUGIN_LIB_DIR%\BUILD_INFO.txt"
ECHO Built by: %USERNAME% >> "%PLUGIN_LIB_DIR%\BUILD_INFO.txt"

REM Get libdatachannel version
CD "%THIRDPARTY_DIR%\libdatachannel"
FOR /F "tokens=*" %%i IN ('git describe --tags --always') DO SET DC_VERSION=%%i
CD "%SCRIPT_DIR%"
ECHO libdatachannel Version: %DC_VERSION% >> "%PLUGIN_LIB_DIR%\BUILD_INFO.txt"

IF DEFINED OPENSSL_ROOT_DIR (
    ECHO OpenSSL Location: !OPENSSL_ROOT_DIR! >> "%PLUGIN_LIB_DIR%\BUILD_INFO.txt"
) ELSE (
    ECHO OpenSSL: System/vcpkg >> "%PLUGIN_LIB_DIR%\BUILD_INFO.txt"
)

ECHO Configuration: RelWithDebInfo >> "%PLUGIN_LIB_DIR%\BUILD_INFO.txt"
ECHO Platform: x64 >> "%PLUGIN_LIB_DIR%\BUILD_INFO.txt"
ECHO. >> "%PLUGIN_LIB_DIR%\BUILD_INFO.txt"
ECHO Files: >> "%PLUGIN_LIB_DIR%\BUILD_INFO.txt"

FOR %%F IN ("%PLUGIN_LIB_DIR%\*.lib") DO (
    ECHO   - %%~nxF ^(%%~zF bytes^) >> "%PLUGIN_LIB_DIR%\BUILD_INFO.txt"
)

ECHO [✓] Build info created
ECHO.

ECHO ========================================================================
ECHO SUCCESS! WebRTC library built and deployed
ECHO ========================================================================
ECHO.
ECHO Library location: %PLUGIN_LIB_DIR%\datachannel.lib
ECHO Headers location: %PLUGIN_LIB_DIR%\include\rtc\
ECHO.
ECHO Next steps:
ECHO   1. Commit the libraries to Git: git add plugins/unreal/Open3DStream/lib/webrtc/
ECHO   2. Build the Unreal plugin: cd plugins\unreal\Open3DStream ^&^& build.bat
ECHO   3. Test WebRTC in Unreal Editor
ECHO.
ECHO For Git LFS support ^(recommended for large binaries^):
ECHO   git lfs track "*.lib"
ECHO   git add .gitattributes
ECHO.

ENDLOCAL
