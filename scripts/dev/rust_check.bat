@echo off
rem Dev helper: cargo build/check for the Rust bindings on this machine.
rem - vcvars from VS Community (compiler), cmake from VS BuildTools
rem - short CARGO_TARGET_DIR to dodge the MSVC 261-char TryCompile limit
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "CARGO_TARGET_DIR=C:\t\rt"
cd /d "%~dp0..\..\bindings\rust"
cargo %*
