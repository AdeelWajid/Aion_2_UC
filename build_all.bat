@echo off
REM Historical name for "build everything". Everything is now one artifact: the release overlay.
REM This forwards to build.bat so there is a single copy of the release recipe; the verification
REM targets (recon, overlay_test) live in build_test.bat and are not part of a release build.
call "%~dp0build.bat" %*
