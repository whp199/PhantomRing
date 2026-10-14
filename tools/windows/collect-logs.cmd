<# : collect-logs.cmd - PhantomRing for Windows: one zip on the Desktop to send back after a test run
@echo off
rem Double-click it (it sits next to PhantomRingHost.dll). It gathers both PhantomRing logs, both inis, BUILD.txt,
rem dxdiag's report and a short system summary into Desktop\PhantomRing-logs-DATE.zip, with your user name,
rem computer name and Steam id blanked out. No saves, screenshots or game files.
rem The rest of this file is a PowerShell script (Windows 10 and 11 have PowerShell 5.1 built in).
setlocal
set "PR_SELF=%~f0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$s = [IO.File]::ReadAllText($env:PR_SELF); & ([ScriptBlock]::Create($s))"
set "PR_RC=%ERRORLEVEL%"
echo.
pause
exit /b %PR_RC%
#>

# ---- PowerShell part (docs/WINDOWS.md "For testers") ----
$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $env:PR_SELF
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$desktop = [Environment]::GetFolderPath('Desktop')
if (-not $desktop -or -not (Test-Path -LiteralPath $desktop)) { $desktop = $here }
$work = Join-Path ([IO.Path]::GetTempPath()) "PhantomRing-logs-$stamp"
$zip = Join-Path $desktop "PhantomRing-logs-$stamp.zip"
New-Item -ItemType Directory -Force -Path $work | Out-Null
$notes = New-Object System.Collections.Generic.List[string]
Write-Host "PhantomRing: collecting logs from $here ..."

# Personal names out: the profile folder first (it holds the user name), then the user name where it is a path part
# or a 'user = name' value, the computer name, and 17-digit Steam ids (EldenRing\<steamid>).
$user = $env:USERNAME
$profileDir = $env:USERPROFILE
$pc = $env:COMPUTERNAME
function Redact([string]$t) {
	if (-not $t) { return $t }
	if ($profileDir -and $profileDir.Length -ge 4) { $t = $t -ireplace [regex]::Escape($profileDir), '%USERPROFILE%' }
	if ($user -and $user.Length -ge 2) {
		$u = [regex]::Escape($user)
		$t = $t -ireplace ('(?<=[\\/])' + $u + '(?=[\\/]|$|\s|["'',;:)])'), '<user>'
		$t = $t -ireplace ('\b(user(?:\s*name)?\s*[=:]\s*)' + $u + '\b'), '$1<user>'
	}
	if ($pc -and $pc.Length -ge 3) { $t = $t -ireplace ('\b' + [regex]::Escape($pc) + '\b'), '<pc>' }
	$t = $t -replace '(?<![0-9])7656119[0-9]{10}(?![0-9])', '<steamid>'
	return $t
}

# Reads a text file even while a game still has it open; a big one keeps its first 1 MB and last 7 MB.
function Read-Shared([string]$path) {
	$fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite, Delete')
	try {
		$len = $fs.Length
		$limit = 8MB
		if ($len -le $limit) {
			$buf = New-Object byte[] $len
			[void]$fs.Read($buf, 0, $len)
			return [Text.Encoding]::UTF8.GetString($buf)
		}
		$head = New-Object byte[] 1MB
		[void]$fs.Read($head, 0, $head.Length)
		$tail = New-Object byte[] ($limit - 1MB)
		[void]$fs.Seek(-$tail.Length, 'End')
		[void]$fs.Read($tail, 0, $tail.Length)
		return [Text.Encoding]::UTF8.GetString($head) + "`r`n[... $($len - $limit) bytes left out ...]`r`n" + [Text.Encoding]::UTF8.GetString($tail)
	} finally { $fs.Dispose() }
}

function Add-Text([string]$name, [string]$text) {
	$dst = Join-Path $work $name
	New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
	[IO.File]::WriteAllText($dst, (Redact $text), (New-Object Text.UTF8Encoding $false))
}

function Add-File([string]$src, [string]$name) {
	if ($src -and (Test-Path -LiteralPath $src -PathType Leaf)) {
		try { Add-Text $name (Read-Shared $src); $notes.Add("$name <- $(Redact $src)") }
		catch { $notes.Add("$name : could not read $(Redact $src): $($_.Exception.Message)") }
	} else {
		$notes.Add("$name : not found ($(Redact $src))")
	}
}

function Hash-List([string]$dir, [string[]]$patterns) {
	$out = @()
	foreach ($p in $patterns) {
		foreach ($f in (Get-ChildItem -LiteralPath $dir -Filter $p -File -ErrorAction SilentlyContinue)) {
			$h = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash
			$out += ('{0}  {1,12}  {2:yyyy-MM-dd HH:mm}  {3}' -f ("$h".ToLower()), $f.Length, $f.LastWriteTime, $f.Name)
		}
	}
	return ($out -join "`r`n")
}

# ---- Elden Ring side: next to this file ----
$hostLog = Join-Path $here 'PhantomRingHost.log'
$hostIni = Join-Path $here 'PhantomRingHost.ini'
Add-File $hostLog 'host/PhantomRingHost.log'
Add-File $hostIni 'host/PhantomRingHost.ini'
Add-File (Join-Path $here 'BUILD.txt') 'BUILD.txt'
Add-Text 'host/files.txt' ("sha256 of the PhantomRing folder's files (compare with BUILD.txt):`r`n" +
	(Hash-List $here @('*.dll', '*.ini', '*.me3', 'BUILD.txt')) + "`r`n" + (Hash-List (Join-Path $here 'mgsv') @('*.dll', '*.ini')))

# ---- MGSV side: the folder of mgsvtpp.exe ([Launch] MgsvExe, the host log, or Steam's library list) ----
$mgsvDir = $null
$how = ''
try {
	if (Test-Path -LiteralPath $hostIni) {
		$m = Select-String -LiteralPath $hostIni -Pattern '^\s*MgsvExe\s*=\s*(\S.*?)\s*$' | Select-Object -First 1
		if ($m) { $exe = $m.Matches[0].Groups[1].Value.Trim('"'); if (Test-Path -LiteralPath $exe) { $mgsvDir = Split-Path -Parent $exe; $how = '[Launch] MgsvExe' } }
	}
	if (-not $mgsvDir -and (Test-Path -LiteralPath $hostLog)) {
		$m = Select-String -LiteralPath $hostLog -Pattern 'launcher: MGSV is (.+mgsvtpp\.exe)' | Select-Object -Last 1
		if ($m) { $exe = $m.Matches[0].Groups[1].Value.Trim(); if (Test-Path -LiteralPath $exe) { $mgsvDir = Split-Path -Parent $exe; $how = 'the host log' } }
	}
	if (-not $mgsvDir) {
		$steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
		if ($steam) {
			$libs = @($steam)
			$vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
			if (Test-Path -LiteralPath $vdf) {
				foreach ($m in (Select-String -LiteralPath $vdf -Pattern '"path"\s+"([^"]+)"')) { $libs += $m.Matches[0].Groups[1].Value.Replace('\\', '\') }
			}
			foreach ($lib in $libs) {
				$d = Join-Path $lib 'steamapps\common\MGS_TPP'
				if (Test-Path -LiteralPath (Join-Path $d 'mgsvtpp.exe')) { $mgsvDir = $d; $how = "Steam's library list"; break }
			}
		}
	}
} catch { $notes.Add("finding MGSV: $($_.Exception.Message)") }

if ($mgsvDir) {
	$notes.Add("MGSV folder: $(Redact $mgsvDir) (from $how)")
	Add-File (Join-Path $mgsvDir 'PhantomRingGuest.log') 'mgsv/PhantomRingGuest.log'
	Add-File (Join-Path $mgsvDir 'PhantomRingGuest.ini') 'mgsv/PhantomRingGuest.ini'
	Add-File (Join-Path $mgsvDir 'PhantomRingGuest.installed') 'mgsv/PhantomRingGuest.installed'
	Add-Text 'mgsv/files.txt' ("sha256 of PhantomRing's files in MGSV's folder (dinput8.dll must match mgsv/dinput8.dll in BUILD.txt):`r`n" +
		(Hash-List $mgsvDir @('dinput8.dll', 'PhantomRing*', 'mgsvtpp.exe')))
} else {
	$notes.Add('MGSV folder: not found (set [Launch] MgsvExe, or copy PhantomRingGuest.log from MGSV''s folder by hand)')
}

# ---- Mod Engine 3's own logs, if it keeps any where we look (best effort) ----
$me3 = Join-Path $env:LOCALAPPDATA 'garyttierney\me3'
if (Test-Path -LiteralPath $me3) {
	$i = 0
	foreach ($f in (Get-ChildItem -LiteralPath $me3 -Recurse -File -Include '*.log', '*.txt' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 3)) {
		$i++
		Add-File $f.FullName ("me3/$i-" + $f.Name)
	}
}

# ---- the PC: Windows, GPU and driver, the Graphics Tools, what runs now, Defender's findings about PhantomRing ----
$sys = New-Object System.Collections.Generic.List[string]
$sys.Add("collected: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')")
try { $os = Get-CimInstance Win32_OperatingSystem; $sys.Add("Windows: $($os.Caption) $($os.Version) (build $($os.BuildNumber)), $([math]::Round($os.TotalVisibleMemorySize / 1MB, 1)) GB RAM") } catch { $sys.Add("Windows: $([Environment]::OSVersion.VersionString)") }
try { foreach ($g in (Get-CimInstance Win32_VideoController)) { $sys.Add("GPU: $($g.Name), driver $($g.DriverVersion) ($($g.DriverDate)), $($g.CurrentHorizontalResolution)x$($g.CurrentVerticalResolution) @ $($g.CurrentRefreshRate) Hz") } } catch {}
try { foreach ($c in (Get-CimInstance Win32_Processor)) { $sys.Add("CPU: $($c.Name.Trim()), $($c.NumberOfCores) cores / $($c.NumberOfLogicalProcessors) threads") } } catch {}
$sys.Add("PowerShell: $($PSVersionTable.PSVersion)")
$sys.Add("D3D12 debug layer (Graphics Tools) installed: $(Test-Path -LiteralPath (Join-Path $env:WINDIR 'System32\d3d12SDKLayers.dll'))")
$sys.Add('Running now:')
foreach ($p in (Get-Process -Name eldenring, mgsvtpp, me3, me3-launcher, steam -ErrorAction SilentlyContinue)) { $sys.Add("  $($p.ProcessName) pid $($p.Id), started $($p.StartTime)") }
try {
	$threats = Get-MpThreatDetection -ErrorAction Stop | Where-Object { "$($_.Resources)" -match 'PhantomRing|dinput8|MGS_TPP|ELDEN RING' }
	if ($threats) { foreach ($t in $threats) { $sys.Add("Defender: $($t.InitialDetectionTime) threat $($t.ThreatID) action $($t.ActionSuccess): $($t.Resources -join '; ')") } }
	else { $sys.Add('Defender: no detections that name PhantomRing, dinput8 or the games') }
} catch { $sys.Add("Defender: could not ask ($($_.Exception.Message.Split([Environment]::NewLine)[0]))") }
Add-Text 'system.txt' ($sys -join "`r`n")

# ---- dxdiag (takes up to a minute) ----
# Its raw report (PC name, user paths) is written OUTSIDE $work: only the redacted copy goes into the zip. (It used to be
# written into $work: a dxdiag still writing after the wait, a sharing violation on the read or a locked delete left the raw
# file there, and Compress-Archive packed it.)
Write-Host 'Running dxdiag (up to a minute) ...'
$dx = Join-Path ([IO.Path]::GetTempPath()) "PhantomRing-dxdiag-$stamp.txt"
try {
	$p = Start-Process -FilePath (Join-Path $env:WINDIR 'System32\dxdiag.exe') -ArgumentList '/t', "`"$dx`"" -WindowStyle Hidden -PassThru
	[void]$p.WaitForExit(120000)
	for ($i = 0; $i -lt 60 -and -not (Test-Path -LiteralPath $dx); $i++) { Start-Sleep -Seconds 1 }
	if (Test-Path -LiteralPath $dx) {
		Start-Sleep -Seconds 1
		Add-Text 'dxdiag.txt' (Read-Shared $dx)
		$notes.Add('dxdiag.txt <- dxdiag /t')
	} else { $notes.Add('dxdiag.txt: dxdiag wrote nothing within 3 minutes') }
} catch { $notes.Add("dxdiag: $($_.Exception.Message)") }
finally { Remove-Item -LiteralPath $dx -Force -ErrorAction SilentlyContinue }

Add-Text 'README.txt' (("PhantomRing logs, collected by collect-logs.cmd on $(Get-Date -Format 'yyyy-MM-dd HH:mm').`r`n" +
	"Your user name, computer name and Steam id are replaced by <user>, <pc> and <steamid>; look the files over before you send them.`r`n" +
	"No saves, screenshots or game files are included.`r`n`r`n") + ($notes -join "`r`n"))

if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Remove-Item -LiteralPath (Join-Path $work 'dxdiag-raw.txt') -Force -ErrorAction SilentlyContinue  # never an unredacted report
try {
	Compress-Archive -Path (Join-Path $work '*') -DestinationPath $zip -ErrorAction Stop
} catch {
	Write-Host "Could not write the zip: $($_.Exception.Message)"
	Write-Host "The files are in $work"
	exit 1
}
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ''
Write-Host "Saved: $zip"
foreach ($n in $notes) { Write-Host "  $n" }
Write-Host 'Send that zip (and a short screen recording if you have one) back. Look it over first if you like: it is plain text.'
try { Start-Process -FilePath explorer.exe -ArgumentList "/select,`"$zip`"" } catch {}
exit 0
