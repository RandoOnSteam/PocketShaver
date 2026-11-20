param([string]$output = "out/soundbase-check")
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$directory = Join-Path $root $output
New-Item -ItemType Directory -Force $directory | Out-Null
$source = [IO.File]::ReadAllText((Join-Path $root "BasiliskII/src/audio.cpp"))
$start = $source.IndexOf("static uint32 AudioSoundBasePort(void)")
$finish = $source.IndexOf("#endif", $start)
if ($start -lt 0 -or $finish -lt 0) { throw "SoundBase implementation not found" }
$section = $source.Substring($start, $finish - $start).Replace("static uint32 AudioSoundBasePort", "uint32 AudioSoundBasePort")
[IO.File]::WriteAllText((Join-Path $directory "soundbase-source.inc"), $section)
$commands = @"
@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /EHsc /W4 /I"$root/BasiliskII/src/include" /I"$directory" "$root/tools/tests/soundbase.cpp" /Fo"$directory/soundbase.obj" /Fe"$directory/SoundBaseTest.exe"
if errorlevel 1 exit /b 1
"$directory/SoundBaseTest.exe"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /W4 /DSHEEPSHAVER=1 /I"$root/BasiliskII/src/include" /I"$directory" "$root/tools/tests/soundbase.cpp" /Fo"$directory/soundbase-ss.obj" /Fe"$directory/SoundBaseSheepShaverTest.exe"
if errorlevel 1 exit /b 1
"$directory/SoundBaseSheepShaverTest.exe"
"@
$script = Join-Path $directory "buildtest.cmd"
[IO.File]::WriteAllText($script, $commands)
& cmd /c $script
if ($LASTEXITCODE -ne 0) { throw "SoundBase tests failed" }
