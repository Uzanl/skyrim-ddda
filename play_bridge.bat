@echo off
rem Starts DDDA in a bridge session: the DDDA bridge reads ddda_session.txt (the tile
rem overlay's folder), deletes it, and only then starts the streamer and reads the
rem generated tiles from tools\terrain\overlay. DDDA started from Steam is the plain game.
rem Then start Skyrim through skse64_loader.exe as usual.
setlocal
set DDDA=E:\SteamLibrary\steamapps\common\DDDA
> "%DDDA%\ddda_session.txt" echo %~dp0tools\terrain\overlay
start "" steam://rungameid/367500
