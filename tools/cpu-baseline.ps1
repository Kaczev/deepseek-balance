<#
tools\cpu-baseline.ps1 — baseline CPU measurement for build\dshb.exe

WHAT IT MEASURES (per window of $Seconds x 1 s, one window per window-state phase):
  1. process CPU as % of ONE logical core, mean/min/max, from a DELTA of
     Process.TotalProcessorTime (Get-Process .CPU), NOT from a 16-core-normalised reading
  2. how many sampled seconds were idle (< $IdlePct % of one core) and the threshold used
  3. the hottest thread's share of process CPU, plus its ProcessThread.ThreadState samples
  4. \Process(dshb)\IO Read / Write Operations/sec means
  5. \GPU Engine(pid_<pid>_*)\Utilization Percentage means, split by engtype (3D / Copy)
  6. intervals between consecutive "[api t=" lines in dshb.log that landed inside the window

WHY EACH PHASE IS A FRESH PROCESS: once a swapchain returns DXGI_STATUS_OCCLUDED, DXGI keeps
returning it immediately until the app resizes/re-creates it, so a minimized phase could bleed
into a following occluded phase inside one process. A fresh process per phase makes window state
the only variable.

BEFORE THE STATE IS APPLIED the harness waits for the app's own start-up to go quiet
(-StartupSeconds, default 15), because a minimize or an occluder applied earlier is undone by the
app: it re-shows and re-raises its window while starting. Every measured tick then verifies that
the requested state still holds and re-asserts it if it does not, and the count of re-assertions is
part of the report -- so a phase that never actually held its state cannot be mistaken for one that
did.

USAGE
  pwsh -NoProfile -File tools\cpu-baseline.ps1                 # visible, minimized, occluded
  pwsh -NoProfile -File tools\cpu-baseline.ps1 -Mode visible   # one phase
  pwsh -NoProfile -File tools\cpu-baseline.ps1 -Seconds 5      # short smoke run

The live dshb process belongs to this harness: it refuses to start if one is already running and
force-stops what it started before returning.
#>
[CmdletBinding()]
param(
    [ValidateSet('visible', 'minimized', 'occluded', 'all')][string]$Mode = 'all',
    [int]$Seconds = 30,
    [int]$SettleSeconds = 8,
    [int]$StartupSeconds = 15,
    [double]$IdlePct = 1.0,
    [string]$ExePath,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'

if (-not $ExePath) { $ExePath = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\dshb.exe' }
if (-not $OutDir) { $OutDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'out\cpu-baseline' }
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
$LogPath = Join-Path $env:LOCALAPPDATA 'deepseek-balance\dshb.log'
$ConfigPath = Join-Path $env:LOCALAPPDATA 'deepseek-balance\config.json'
$ProcName = [System.IO.Path]::GetFileNameWithoutExtension($ExePath)
$RunStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
# config.json exactly as found: restored before every phase and again before returning
$ConfigSnapshot = [System.IO.File]::ReadAllBytes($ConfigPath)

$script:Transcript = New-Object System.Collections.Generic.List[string]
function Say {
    param([string]$Line = '')
    $script:Transcript.Add($Line)
    Write-Output $Line
}
function Fmt {
    param($Value, [int]$Digits = 2)
    if ($null -eq $Value) { return 'n/a' }
    if ($Value -is [double] -and [double]::IsNaN($Value)) { return 'nan' }
    return ([double]$Value).ToString("F$Digits", [System.Globalization.CultureInfo]::InvariantCulture)
}
function Series {
    param($Values, [int]$Digits = 1)
    return (($Values | ForEach-Object { Fmt $_ $Digits }) -join ' ')
}

# ---------------------------------------------------------------------------
# Win32 / DWM interop
# ---------------------------------------------------------------------------
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public class DshbWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int idx);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO li);
    [DllImport("kernel32.dll")] public static extern uint GetTickCount();
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int val, int size);

    public static List<IntPtr> WindowsOf(uint want) {
        List<IntPtr> outl = new List<IntPtr>();
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == want) outl.Add(h);
            return true;
        }, IntPtr.Zero);
        return outl;
    }
    public static string ClassOf(IntPtr h) {
        StringBuilder sb = new StringBuilder(256);
        GetClassName(h, sb, 256);
        return sb.ToString();
    }
    public static IntPtr RootAt(int x, int y) {
        POINT p; p.X = x; p.Y = y;
        return GetAncestor(WindowFromPoint(p), 2); // GA_ROOT
    }
    public static int Cloaked(IntPtr h) {
        int v = -1;
        DwmGetWindowAttribute(h, 14, out v, 4); // DWMWA_CLOAKED
        return v;
    }
    public static int[] Rect(IntPtr h) {
        RECT r; GetWindowRect(h, out r);
        return new int[] { r.Left, r.Top, r.Right, r.Bottom };
    }
    public static int[] Cursor() {
        POINT p; GetCursorPos(out p);
        return new int[] { p.X, p.Y };
    }
    // seconds since the last input the session saw; -1 if the API failed
    public static double IdleSeconds() {
        LASTINPUTINFO li = new LASTINPUTINFO();
        li.cbSize = (uint)Marshal.SizeOf(li);
        if (!GetLastInputInfo(ref li)) return -1.0;
        return unchecked((uint)(GetTickCount() - li.dwTime)) / 1000.0;
    }
}
'@

$SW_RESTORE = 9
$SW_MINIMIZE = 6
$SWP_NOSIZE = 0x1
$SWP_NOZORDER = 0x4
    $SWP_NOACTIVATE = 0x10
$HWND_TOPMOST = [IntPtr](-1)

function Get-PixelStats {
    param([int]$X, [int]$Y, [int]$W, [int]$H)
    if ($W -le 0 -or $H -le 0) { return 'empty-rect' }
    $bmp = New-Object System.Drawing.Bitmap($W, $H)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size($W, $H)))
    $hist = @{}
    $total = 0
    for ($yy = 0; $yy -lt $H; $yy += 3) {
        for ($xx = 0; $xx -lt $W; $xx += 3) {
            $c = $bmp.GetPixel($xx, $yy).ToArgb() -band 0xFFFFFF
            if ($hist.ContainsKey($c)) { $hist[$c]++ } else { $hist[$c] = 1 }
            $total++
        }
    }
    $g.Dispose(); $bmp.Dispose()
    $top = $hist.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1
    return ("sampled={0} distinct={1} dominant=#{2:X6} frac={3}" -f $total, $hist.Count, $top.Key, (Fmt ($top.Value / $total) 3))
}

# Where is the widget actually visible? The window the desktop shows at the widget's centre.
function Get-OcclusionState {
    param([IntPtr]$Hwnd, [int[]]$Rect)
    $cx = [int](($Rect[0] + $Rect[2]) / 2)
    $cy = [int](($Rect[1] + $Rect[3]) / 2)
    $at = [DshbWin]::RootAt($cx, $cy)
    $pidAt = 0
    [void][DshbWin]::GetWindowThreadProcessId($at, [ref]$pidAt)
    return [pscustomobject]@{
        CenterX       = $cx
        CenterY       = $cy
        TopHwnd       = ('0x{0:X}' -f $at.ToInt64())
        TopPid        = $pidAt
        TopClass      = [DshbWin]::ClassOf($at)
        TopIsWidget   = ($at -eq $Hwnd)
        Pixels        = (Get-PixelStats -X $Rect[0] -Y $Rect[1] -W ($Rect[2] - $Rect[0]) -H ($Rect[3] - $Rect[1]))
        Iconic        = [DshbWin]::IsIconic($Hwnd)
        Visible       = [DshbWin]::IsWindowVisible($Hwnd)
        Cloaked       = [DshbWin]::Cloaked($Hwnd)
        Cursor        = ([DshbWin]::Cursor() -join ',')
        IdleSec       = [math]::Round([DshbWin]::IdleSeconds(), 2)
    }
}

function Read-LogRange {
    param([string]$Path, [long]$From, [long]$To)
    if ($To -le $From) { return '' }
    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        [void]$fs.Seek($From, [System.IO.SeekOrigin]::Begin)
        $buf = New-Object byte[] ($To - $From)
        $read = 0
        while ($read -lt $buf.Length) {
            $n = $fs.Read($buf, $read, $buf.Length - $read)
            if ($n -le 0) { break }
            $read += $n
        }
        return [System.Text.Encoding]::UTF8.GetString($buf, 0, $read)
    } finally { $fs.Dispose() }
}

function Test-BytesEqual {
    param([byte[]]$A, [byte[]]$B)
    if ($A.Length -ne $B.Length) { return $false }
    for ($i = 0; $i -lt $A.Length; $i++) { if ($A[$i] -ne $B[$i]) { return $false } }
    return $true
}

# The app rewrites config.json when a drag ends (src\main.cpp: EndDrag -> SavePanelPos). Restoring
# the run-start bytes keeps every phase launching from identical settings, and leaves the file
# exactly as this run found it.
function Restore-ConfigSnapshot {
    if (-not (Test-Path -LiteralPath $ConfigPath)) { return $false }
    $now = [System.IO.File]::ReadAllBytes($ConfigPath)
    if (Test-BytesEqual $now $script:ConfigSnapshot) { return $false }
    [System.IO.File]::WriteAllBytes($ConfigPath, $script:ConfigSnapshot)
    return $true
}

# The counter collector runs out of process so the 1 s CPU loop above stays tight.
$CounterJob = {
    param([int]$ProcId, [string]$ProcName, [int]$Samples)
    $paths = @(
        "\Process($ProcName)\IO Read Operations/sec",
        "\Process($ProcName)\IO Write Operations/sec",
        "\GPU Engine(pid_${ProcId}_*)\Utilization Percentage"
    )
    $out = New-Object System.Collections.Generic.List[string]
    try {
        $res = Get-Counter -Counter $paths -MaxSamples $Samples -SampleInterval 1 -ErrorAction Stop
        foreach ($s in $res.CounterSamples) {
            $out.Add(('{0}|{1}|{2}' -f $s.Timestamp.Ticks, $s.Path, ([double]$s.CookedValue).ToString('F4', [System.Globalization.CultureInfo]::InvariantCulture)))
        }
    } catch {
        $out.Add('ERROR|' + $_.Exception.Message)
    }
    return $out
}

# ---------------------------------------------------------------------------
# One measurement phase: fresh process, one window state, one sampling window
# ---------------------------------------------------------------------------
function Measure-Phase {
    param([string]$Phase)

    $result = [ordered]@{ Phase = $Phase; Ok = $false; Error = '' }

    if (@(Get-Process -Name $ProcName -ErrorAction SilentlyContinue).Count -gt 0) {
        throw "a $ProcName process is already running; this harness owns the live widget and refuses to race another owner"
    }

    $configHashBefore = (Get-FileHash -LiteralPath $ConfigPath -Algorithm SHA256).Hash
    $logLenBefore = (Get-Item -LiteralPath $LogPath).Length

    $proc = $null
    $form = $null
    $job = $null
    try {
        $proc = Start-Process -FilePath $ExePath -PassThru
        $result.Pid = $proc.Id

        # --- wait for the widget window, then park it fully inside one monitor ---
        $hwnd = [IntPtr]::Zero
        $deadline = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $deadline -and $hwnd -eq [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 200
            $proc.Refresh()
            if ($proc.HasExited) { throw "process exited during startup (exit code $($proc.ExitCode))" }
            foreach ($h in [DshbWin]::WindowsOf([uint32]$proc.Id)) {
                if ([DshbWin]::ClassOf($h) -eq 'DshbWnd') { $hwnd = $h; break }
            }
        }
        if ($hwnd -eq [IntPtr]::Zero) { throw 'no DshbWnd window appeared within 20 s' }

        # Measured exactly where the app puts it: no SetWindowPos, so the app's drag/snap
        # bookkeeping is untouched and this harness can never make the app save a new position.
        $widgetRect = [DshbWin]::Rect($hwnd)
        $screen = [System.Windows.Forms.Screen]::FromHandle($hwnd).Bounds
        $result.WidgetRect = ($widgetRect -join ',')
        $result.WindowSize = "$($widgetRect[2] - $widgetRect[0])x$($widgetRect[3] - $widgetRect[1])"
        $visL = [Math]::Max($widgetRect[0], $screen.Left)
        $visT = [Math]::Max($widgetRect[1], $screen.Top)
        $visR = [Math]::Min($widgetRect[2], $screen.Right)
        $visB = [Math]::Min($widgetRect[3], $screen.Bottom)
        $winArea = ($widgetRect[2] - $widgetRect[0]) * ($widgetRect[3] - $widgetRect[1])
        $visArea = [Math]::Max(0, $visR - $visL) * [Math]::Max(0, $visB - $visT)
        $result.OnScreenFraction = if ($winArea -gt 0) { [Math]::Round($visArea / $winArea, 3) } else { 0 }
        $result.OnScreenRect = (@($visL, $visT, $visR, $visB) -join ',')
        $result.ScreenBounds = "$($screen.X),$($screen.Y),$($screen.Width),$($screen.Height)"
        # probes and pixels use the on-screen part only: the off-screen sliver is not seeable
        $probeRect = @($visL, $visT, $visR, $visB)

        # The app keeps claiming its own window state for the first seconds after the window
        # appears. A state applied during that window is silently undone: measured on this box, a
        # SW_MINIMIZE sent 3 s after the window appeared was gone by +11 s (IsIconic false again),
        # while the same call at +15 s held for the remaining 12 s, and a topmost occluder raised
        # at +15 s stayed on top for all 16 s. So wait for start-up to go quiet first, then apply
        # the state, then verify it on every tick.
        $result.StartupSeconds = $StartupSeconds
        $sw0 = [System.Diagnostics.Stopwatch]::StartNew()
        while ($sw0.Elapsed.TotalSeconds -lt $StartupSeconds) { Start-Sleep -Milliseconds 100 }

        $fh = [IntPtr]::Zero

        # --- apply the window state ---
        if ($Phase -eq 'visible') {
            [void][DshbWin]::ShowWindow($hwnd, $SW_RESTORE)
        } elseif ($Phase -eq 'minimized') {
            [void][DshbWin]::ShowWindow($hwnd, $SW_MINIMIZE)
        } elseif ($Phase -eq 'occluded') {
            [void][DshbWin]::ShowWindow($hwnd, $SW_RESTORE)
            $form = New-Object System.Windows.Forms.Form
            $form.FormBorderStyle = 'None'
            $form.StartPosition = 'Manual'
            $bx = [int]$screen.X; $by = [int]$screen.Y; $bw = [int]$screen.Width; $bh = [int]$screen.Height
            Say "  occluder bounds: $bx,$by ${bw}x${bh}"
            $form.Bounds = New-Object System.Drawing.Rectangle($bx, $by, $bw, $bh)
            $form.BackColor = [System.Drawing.Color]::FromArgb(255, 10, 200, 30)
            $form.TopMost = $true
            $form.ShowInTaskbar = $false
            $form.Show()
            [System.Windows.Forms.Application]::DoEvents()
            $fh = $form.Handle
        }

        # --- settle, then state evidence ---
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $proc.Refresh()
        $settleCpu0 = $proc.TotalProcessorTime.TotalSeconds
        while ($sw.Elapsed.TotalSeconds -lt $SettleSeconds) {
            if ($form) { [System.Windows.Forms.Application]::DoEvents() }
            Start-Sleep -Milliseconds 100
        }
        $proc.Refresh()
        $settleCpu1 = $proc.TotalProcessorTime.TotalSeconds
        $result.SettleSeconds = [math]::Round($sw.Elapsed.TotalSeconds, 2)
        $result.SettleCpuPct = [math]::Round(($settleCpu1 - $settleCpu0) / $sw.Elapsed.TotalSeconds * 100, 2)

        $result.PreState = Get-OcclusionState -Hwnd $hwnd -Rect $probeRect

        # --- counter collector: one native PDH query, 1 s cadence, spans the window ---
        $job = Start-Job -ScriptBlock $CounterJob -ArgumentList $proc.Id, $ProcName, ($Seconds + 8)

        # let the collector's pwsh child start and take its first sample
        $sw2 = [System.Diagnostics.Stopwatch]::StartNew()
        while ($sw2.Elapsed.TotalSeconds -lt 3) {
            if ($form) { [System.Windows.Forms.Application]::DoEvents() }
            Start-Sleep -Milliseconds 100
        }

        # --- the sampling window: $Seconds ticks of ~1 s, absolute schedule, no drift ---
        $result.WindowStartTicks = [DateTime]::Now.Ticks
        $logLenWindowStart = (Get-Item -LiteralPath $LogPath).Length
        $proc.Refresh()
        $prevCpu = $proc.TotalProcessorTime.TotalSeconds
        $prevThreadCpu = @{}
        foreach ($t in $proc.Threads) { $prevThreadCpu[[int]$t.Id] = $t.TotalProcessorTime.TotalSeconds }

        $sw3 = [System.Diagnostics.Stopwatch]::StartNew()
        $prevWall = $sw3.Elapsed.TotalSeconds
        $ticks = New-Object System.Collections.Generic.List[object]
        $threadBusy = @{}
        $threadStates = @{}
        $reasserts = 0

        for ($i = 1; $i -le $Seconds; $i++) {
            $target = $i * 1000
            while ($sw3.ElapsedMilliseconds -lt ($target - 5)) {
                if ($form) { [System.Windows.Forms.Application]::DoEvents() }
                Start-Sleep -Milliseconds 10
            }
            while ($sw3.ElapsedMilliseconds -lt $target) { }
            $wall = $sw3.Elapsed.TotalSeconds
            $proc.Refresh()
            $cpu = $proc.TotalProcessorTime.TotalSeconds
            $perThread = @{}
            foreach ($t in $proc.Threads) {
                $id = [int]$t.Id
                $cur = $t.TotalProcessorTime.TotalSeconds
                $perThread[$id] = $cur
                if ($prevThreadCpu.ContainsKey($id)) {
                    $d = $cur - $prevThreadCpu[$id]
                    if ($d -gt 0) {
                        if ($threadBusy.ContainsKey($id)) { $threadBusy[$id] += $d } else { $threadBusy[$id] = $d }
                    }
                }
                if (-not $threadStates.ContainsKey($id)) { $threadStates[$id] = New-Object System.Collections.Generic.List[string] }
                $threadStates[$id].Add($t.ThreadState.ToString())
            }
            $prevThreadCpu = $perThread
            $dWall = $wall - $prevWall
            $dCpu = $cpu - $prevCpu
            $cur = [DshbWin]::Cursor()
            # hold the requested state and record whether it really held for this tick
            $held = $true
            if ($Phase -eq 'minimized') {
                $held = [DshbWin]::IsIconic($hwnd)
                if (-not $held) { [void][DshbWin]::ShowWindow($hwnd, $SW_MINIMIZE); $reasserts++ }
            } elseif ($Phase -eq 'occluded') {
                $midX = $probeRect[0] + [int](($probeRect[2] - $probeRect[0]) * 0.5)
                $midY = $probeRect[1] + [int](($probeRect[3] - $probeRect[1]) * 0.5)
                $held = ([DshbWin]::RootAt($midX, $midY) -eq $fh)
                if (-not $held) { [void][DshbWin]::SetWindowPos($fh, $HWND_TOPMOST, $bx, $by, $bw, $bh, $SWP_NOACTIVATE); $reasserts++ }
            } else {
                $held = -not [DshbWin]::IsIconic($hwnd)
                if (-not $held) { [void][DshbWin]::ShowWindow($hwnd, $SW_RESTORE); $reasserts++ }
            }
            $ticks.Add([pscustomobject]@{
                    Tick         = $i
                    Wall         = $wall
                    DCpu         = $dCpu
                    Pct          = ($dCpu / $dWall * 100)
                    Cursor       = ($cur -join ',')
                    CurInWidget  = ($cur[0] -ge $widgetRect[0] -and $cur[0] -le $widgetRect[2] -and $cur[1] -ge $widgetRect[1] -and $cur[1] -le $widgetRect[3])
                    InputIdleSec = [math]::Round([DshbWin]::IdleSeconds(), 2)
                    StateHeld    = $held
                })
            $prevWall = $wall
            $prevCpu = $cpu
        }
        $windowSeconds = $sw3.Elapsed.TotalSeconds
        $logLenWindowEnd = (Get-Item -LiteralPath $LogPath).Length
        $result.LogBytesInWindow = $logLenWindowEnd - $logLenWindowStart
        $proc.Refresh()
        $result.WindowTicksEnd = [DateTime]::Now.Ticks

        $result.PostState = Get-OcclusionState -Hwnd $hwnd -Rect $probeRect

        # --- CPU summary ---
        $pcts = @($ticks | ForEach-Object { $_.Pct })
        $cpuTotal = ($ticks | Measure-Object -Property DCpu -Sum).Sum
        $result.WindowSeconds = [math]::Round($windowSeconds, 3)
        $result.CpuMeanPct = [math]::Round(($cpuTotal / $windowSeconds * 100), 2)
        $result.CpuMinPct = [math]::Round(($pcts | Measure-Object -Minimum).Minimum, 2)
        $result.CpuMaxPct = [math]::Round(($pcts | Measure-Object -Maximum).Maximum, 2)
        $result.CpuPerSecondPct = $pcts
        $result.CpuSeconds = [math]::Round($cpuTotal, 3)
        $result.IdlePctThreshold = $IdlePct
        $result.IdleSeconds = @($pcts | Where-Object { $_ -lt $IdlePct }).Count
        $result.Below5PctSeconds = @($pcts | Where-Object { $_ -lt 5 }).Count
        $result.CursorInWidgetSeconds = @($ticks | Where-Object { $_.CurInWidget }).Count
        $result.RequestedState = $Phase
        $result.StateHeldTicks = @($ticks | Where-Object { $_.StateHeld }).Count
        $result.StateReasserts = $reasserts
        $result.InputIdleFirst = $ticks[0].InputIdleSec
        $result.InputIdleLast = $ticks[$ticks.Count - 1].InputIdleSec
        $result.InputIdleGrowth = [math]::Round($result.InputIdleLast - $result.InputIdleFirst, 2)
        # the age of the last input must grow by the whole sampled span if nothing arrived during it
        $result.InputIdleExpectedGrowth = [math]::Round($ticks[$ticks.Count - 1].Wall - $ticks[0].Wall, 2)
        $result.InputDuringWindow = ($result.InputIdleGrowth -lt ($result.InputIdleExpectedGrowth - 0.5))

        # --- hottest thread ---
        $hotId = $null
        $hot = -1.0
        foreach ($k in $threadBusy.Keys) { if ($threadBusy[$k] -gt $hot) { $hot = $threadBusy[$k]; $hotId = $k } }
        $result.HotThreadId = $hotId
        if ($null -ne $hotId) {
            $result.HotThreadCpuSeconds = [math]::Round($hot, 3)
            $result.HotThreadSharePct = [math]::Round($hot / $cpuTotal * 100, 1)
            $states = @($threadStates[$hotId])
            $result.HotThreadStateSamples = $states.Count
            $hist = [ordered]@{}
            foreach ($g in ($states | Group-Object)) { $hist[$g.Name] = $g.Count }
            $result.HotThreadStateHistogram = $hist
            $result.ThreadCpuTop = @(
                $threadBusy.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 5 |
                ForEach-Object { [pscustomobject]@{ ThreadId = $_.Key; CpuSeconds = [math]::Round($_.Value, 3); SharePct = [math]::Round($_.Value / $cpuTotal * 100, 1) } }
            )
        }

        # --- log: api cadence and render stats appended inside the window ---
        $newText = Read-LogRange -Path $LogPath -From $logLenWindowStart -To $logLenWindowEnd
        $result.LogLinesInWindow = @($newText -split "`n" | Where-Object { $_.Trim().Length -gt 0 }).Count
        $result.LogRawInWindow = @($newText -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 })

        $apiIn = @([regex]::Matches($newText, '\[api t=([0-9.]+)s\]') | ForEach-Object { [double]$_.Groups[1].Value })
        $result.ApiTInWindow = $apiIn
        # the interval that opens the window: previous api line to the first one inside
        $back = [Math]::Max(0, $logLenWindowStart - 8192)
        $priorText = Read-LogRange -Path $LogPath -From $back -To $logLenWindowStart
        $priorApi = @([regex]::Matches($priorText, '\[api t=([0-9.]+)s\]') | ForEach-Object { [double]$_.Groups[1].Value })
        $prevApi = if ($priorApi.Count -gt 0) { $priorApi[$priorApi.Count - 1] } else { $null }
        $intervals = New-Object System.Collections.Generic.List[object]
        if ($null -ne $prevApi -and $apiIn.Count -gt 0) {
            $intervals.Add([pscustomobject]@{ From = $prevApi; To = $apiIn[0]; Delta = [math]::Round($apiIn[0] - $prevApi, 1); CrossesWindowStart = $true })
        }
        for ($k = 1; $k -lt $apiIn.Count; $k++) {
            $intervals.Add([pscustomobject]@{ From = $apiIn[$k - 1]; To = $apiIn[$k]; Delta = [math]::Round($apiIn[$k] - $apiIn[$k - 1], 1); CrossesWindowStart = $false })
        }
        $result.ApiIntervals = $intervals
        $inWindow = @($intervals | Where-Object { -not $_.CrossesWindowStart })
        $result.ApiIntervalMeanInWindow = if ($inWindow.Count -gt 0) { [math]::Round((($inWindow | Measure-Object -Property Delta -Average).Average), 1) } else { $null }

        $renders = @([regex]::Matches($newText, '\[render\] frames=(\d+) elapsed=([0-9.]+)s\s+.*?([0-9.]+)ms\s+.*?([0-9.]+)Hz'))
        $result.RenderLines = @($renders | ForEach-Object {
                [pscustomobject]@{
                    Frames = [int]$_.Groups[1].Value
                    Elapsed = [double]$_.Groups[2].Value
                    Fps = [math]::Round([int]$_.Groups[1].Value / [double]$_.Groups[2].Value, 1)
                    MsPerFrame = [double]$_.Groups[3].Value
                    ReportedHz = [double]$_.Groups[4].Value
                }
            })

        # --- counters ---
        $raw = @(Receive-Job -Job $job -Wait -AutoRemoveJob)
        $job = $null
        $result.CounterRawLines = $raw.Count
        $samples = @{}
        foreach ($line in $raw) {
            $parts = "$line" -split '\|'
            if ($parts.Count -lt 3) { continue }
            if ($parts[0] -eq 'ERROR') { $result.CounterError = $parts[1]; continue }
            $t = [long]$parts[0]
            if ($t -lt $result.WindowStartTicks -or $t -gt $result.WindowTicksEnd) { continue }
            if (-not $samples.ContainsKey($t)) { $samples[$t] = New-Object System.Collections.Generic.List[object] }
            $samples[$t].Add([pscustomobject]@{ Path = $parts[1]; Value = [double]$parts[2] })
        }
        $result.CounterSamplesInWindow = $samples.Count

        $ioRead = New-Object System.Collections.Generic.List[double]
        $ioWrite = New-Object System.Collections.Generic.List[double]
        $gpu = @{}
        foreach ($t in $samples.Keys) {
            foreach ($s in $samples[$t]) {
                $p = $s.Path.ToLowerInvariant()
                if ($p -like '*io read operations*') { $ioRead.Add($s.Value); continue }
                if ($p -like '*io write operations*') { $ioWrite.Add($s.Value); continue }
                $m = [regex]::Match($p, 'engtype_([^)\\]*)')
                if ($m.Success) {
                    $et = $m.Groups[1].Value
                    if (-not $gpu.ContainsKey($et)) { $gpu[$et] = New-Object System.Collections.Generic.List[double] }
                    $gpu[$et].Add($s.Value)
                }
            }
        }
        $result.IoReadMean = if ($ioRead.Count -gt 0) { [math]::Round(($ioRead | Measure-Object -Average).Average, 2) } else { $null }
        $result.IoWriteMean = if ($ioWrite.Count -gt 0) { [math]::Round(($ioWrite | Measure-Object -Average).Average, 2) } else { $null }
        $result.IoReadMax = if ($ioRead.Count -gt 0) { [math]::Round(($ioRead | Measure-Object -Maximum).Maximum, 2) } else { $null }
        $result.IoWriteMax = if ($ioWrite.Count -gt 0) { [math]::Round(($ioWrite | Measure-Object -Maximum).Maximum, 2) } else { $null }
        $result.Gpu = [ordered]@{}
        foreach ($et in ($gpu.Keys | Sort-Object)) {
            $vals = $gpu[$et]
            $result.Gpu[$et] = [pscustomobject]@{
                Samples = $vals.Count
                Mean    = [math]::Round(($vals | Measure-Object -Average).Average, 2)
                Max     = [math]::Round(($vals | Measure-Object -Maximum).Maximum, 2)
            }
        }

        $result.Ok = $true
    } finally {
        if ($job) { Stop-Job -Job $job -ErrorAction SilentlyContinue; Remove-Job -Job $job -Force -ErrorAction SilentlyContinue }
        if ($form) { $form.Hide(); $form.Dispose() }
        if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Milliseconds 800
        if ($form) { [System.Windows.Forms.Application]::DoEvents() }
    }

    $result.LogLenBefore = $logLenBefore
    $result.LogLenAfter = (Get-Item -LiteralPath $LogPath).Length
    $result.ConfigHashBefore = $configHashBefore
    $result.ConfigHashAfter = (Get-FileHash -LiteralPath $ConfigPath -Algorithm SHA256).Hash
    $result.ConfigUnchanged = ($result.ConfigHashBefore -eq $result.ConfigHashAfter)
    return [pscustomobject]$result
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
$phases = if ($Mode -eq 'all') { @('visible', 'minimized', 'occluded') } else { @($Mode) }

$exeItem = Get-Item -LiteralPath $ExePath
Say "=== dshb CPU baseline ==="
Say "time            : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') local"
Say "exe             : $ExePath"
Say "exe size/mtime  : $($exeItem.Length) bytes / $($exeItem.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))"
Say "exe sha256      : $((Get-FileHash -LiteralPath $ExePath -Algorithm SHA256).Hash)"
Say "phases          : $($phases -join ', ')"
Say "window          : $Seconds x 1 s sampling, ${StartupSeconds}s start-up wait, settle ${SettleSeconds}s, idle threshold < $IdlePct % of one core"
Say "log             : $LogPath"
Say "config          : $ConfigPath"
Say "cpu count       : $env:NUMBER_OF_PROCESSORS logical processors"
Say "overall idle sec: $([math]::Round([DshbWin]::IdleSeconds(),1)) at start (host-side, before any measurement)"
Say ""

# park the cursor away from where the widget will sit: an un-hovered widget is the idle state
$cursorBefore = [DshbWin]::Cursor()
Say "cursor before   : $($cursorBefore -join ',') -> parking at 4,4"
[void][DshbWin]::SetCursorPos(4, 4)

$results = New-Object System.Collections.Generic.List[object]
foreach ($phase in $phases) {
    Say "--- phase '$phase' : launching a fresh $ProcName ---"
    if (Restore-ConfigSnapshot) {
        Say "  config.json had been rewritten (the app saves it when a drag ends); restored the run-start bytes so every phase launches from identical settings"
    }
    try {
        $r = Measure-Phase -Phase $phase
    } catch {
        Say "phase '$phase' FAILED: $($_.Exception.Message)"
        Say "  at line $($_.InvocationInfo.ScriptLineNumber): $($_.InvocationInfo.Line.Trim())"
        Say "  stack: $($_.ScriptStackTrace)"
        $r = [pscustomobject]@{ Phase = $phase; Ok = $false; Error = $_.Exception.Message }
    }
    $results.Add($r)
    Say ""
}

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
foreach ($r in $results) {
    Say "================ phase: $($r.Phase) ================"
    if (-not $r.Ok) { Say "  NOT MEASURED: $($r.Error)"; Say ""; continue }

    Say "  window state evidence"
    Say "    widget window rect               : $($r.WidgetRect)   size=$($r.WindowSize)   on-screen fraction=$($r.OnScreenFraction)"
    Say "    on-screen part probed            : $($r.OnScreenRect)   desktop $($r.ScreenBounds)"
    Say "    requested state '$($r.RequestedState)'        : held on $($r.StateHeldTicks) of $($r.CpuPerSecondPct.Count) measured ticks; harness had to re-assert it $($r.StateReasserts) time(s); state applied after a $($r.StartupSeconds)s start-up wait"
    Say "    before window: iconic=$($r.PreState.Iconic) visible=$($r.PreState.Visible) cloaked=$($r.PreState.Cloaked)"
    Say "      window at widget centre ($($r.PreState.CenterX),$($r.PreState.CenterY)): hwnd=$($r.PreState.TopHwnd) pid=$($r.PreState.TopPid) class='$($r.PreState.TopClass)' isWidget=$($r.PreState.TopIsWidget)"
    Say "      pixels over widget rect          : $($r.PreState.Pixels)"
    Say "      cursor=$($r.PreState.Cursor)  secs since last user input=$($r.PreState.IdleSec)"
    Say "    after  window: iconic=$($r.PostState.Iconic) visible=$($r.PostState.Visible) cloaked=$($r.PostState.Cloaked)"
    Say "      window at widget centre ($($r.PostState.CenterX),$($r.PostState.CenterY)): hwnd=$($r.PostState.TopHwnd) pid=$($r.PostState.TopPid) class='$($r.PostState.TopClass)' isWidget=$($r.PostState.TopIsWidget)"
    Say "      pixels over widget rect          : $($r.PostState.Pixels)"
    Say "      cursor=$($r.PostState.Cursor)  secs since last user input=$($r.PostState.IdleSec)"
    Say "  settle ($($r.SettleSeconds)s): process CPU = $(Fmt $r.SettleCpuPct)% of one core"
    Say "  (1) process CPU over the window, % of ONE logical core"
    Say "      path : delta of Get-Process($ProcName).TotalProcessorTime (per-thread sum, = Win32 'Process(*)\\% Processor Time' x N cores)"
    Say "      norm : (CPU seconds consumed) / (wall seconds) x 100, NOT divided by $env:NUMBER_OF_PROCESSORS"
    Say "      window = $(Fmt $r.WindowSeconds 3) s wall, $(Fmt $r.CpuSeconds 3) CPU s"
    Say "      mean=$(Fmt $r.CpuMeanPct)%  min=$(Fmt $r.CpuMinPct)%  max=$(Fmt $r.CpuMaxPct)%"
    Say "      per-second: $(Series $r.CpuPerSecondPct 1)"
    Say "  (2) idle seconds : $($r.IdleSeconds) of $($r.CpuPerSecondPct.Count) below $($r.IdlePctThreshold)% of one core; $($r.Below5PctSeconds) below 5%"
    Say "      input/hover  : cursor inside the widget for $($r.CursorInWidgetSeconds) of $($r.CpuPerSecondPct.Count) ticks; secs since last user input $($r.InputIdleFirst) -> $($r.InputIdleLast) (grew $(Fmt $r.InputIdleGrowth 1)s over a $(Fmt $r.InputIdleExpectedGrowth 1)s sampled span) => user input during the window = $($r.InputDuringWindow)"
    Say "  (3) hottest thread: id=$($r.HotThreadId) cpu=$(Fmt $r.HotThreadCpuSeconds 3)s share=$(Fmt $r.HotThreadSharePct 1)% of process CPU"
    Say "      ProcessThread.ThreadState over $($r.HotThreadStateSamples) samples (one per second): $(($r.HotThreadStateHistogram.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join '  ')"
    if ($r.ThreadCpuTop) {
        Say "      top threads by CPU: $(($r.ThreadCpuTop | ForEach-Object { "id$($_.ThreadId)=$(Fmt $_.CpuSeconds 2)s($(Fmt $_.SharePct 1)%)" }) -join '  ')"
    }
    Say "  (4) IO ops/sec   : read mean=$(Fmt $r.IoReadMean) max=$(Fmt $r.IoReadMax) | write mean=$(Fmt $r.IoWriteMean) max=$(Fmt $r.IoWriteMax)   ($($r.CounterSamplesInWindow) counter samples inside the window)"
    if ($r.CounterError) { Say "      counter error: $($r.CounterError)" }
    Say "  (5) GPU engine utilization % (mean over every sample x instance of that engtype)"
    foreach ($et in $r.Gpu.Keys) {
        Say "      $et : mean=$(Fmt $r.Gpu[$et].Mean) max=$(Fmt $r.Gpu[$et].Max) over $($r.Gpu[$et].Samples) values"
    }
    Say "  (6) '[api t=' cadence inside the window: $($r.ApiTInWindow.Count) line(s)"
    Say "      t values: $(($r.ApiTInWindow | ForEach-Object { (Fmt $_ 1) }) -join ' ')"
    foreach ($iv in $r.ApiIntervals) {
        Say "      interval $(Fmt $iv.From 1) -> $(Fmt $iv.To 1) = $(Fmt $iv.Delta 1) s$(if ($iv.CrossesWindowStart) { '   [opens the window: measured from the last line before it]' } else { '' })"
    }
    if ($null -ne $r.ApiIntervalMeanInWindow) { Say "      mean of fully-inside intervals: $(Fmt $r.ApiIntervalMeanInWindow 1) s" }
    if ($r.RenderLines.Count -gt 0) {
        Say "  supporting: [render] lines appended in the window"
        foreach ($rl in $r.RenderLines) { Say "      frames=$($rl.Frames) elapsed=$($rl.Elapsed)s -> $(Fmt $rl.Fps 1) fps (app-reported $(Fmt $rl.ReportedHz 1) Hz, $(Fmt $rl.MsPerFrame 1) ms/frame)" }
    } else {
        Say "  supporting: no [render] stats line was appended in the window"
    }
    Say "  log: $($r.LogBytesInWindow) byte(s) appended inside the window ($($r.LogLinesInWindow) line(s)); whole phase $($r.LogLenBefore) -> $($r.LogLenAfter); app rewrote config.json during the phase = $(-not $r.ConfigUnchanged)"
    if ($r.LogRawInWindow.Count -gt 0 -and $r.LogRawInWindow.Count -le 40) {
        Say "  raw log lines appended inside the window:"
        foreach ($l in $r.LogRawInWindow) { Say "      $l" }
    }
    Say ""
}

# deltas against the visible phase
$base = $results | Where-Object { $_.Phase -eq 'visible' -and $_.Ok } | Select-Object -First 1
if ($base) {
    Say "================ deltas vs visible ================"
    foreach ($r in $results) {
        if (-not $r.Ok -or $r.Phase -eq 'visible') { continue }
        Say ("  {0,-10} cpu mean {1} -> {2} ({3})   min {4} -> {5}   max {6} -> {7}   idle {8} -> {9}   hot-thread share {10}% -> {11}%   gpu3d {12} -> {13}   gpub copy {14} -> {15}" -f `
                $r.Phase, (Fmt $base.CpuMeanPct), (Fmt $r.CpuMeanPct), (Fmt ($r.CpuMeanPct - $base.CpuMeanPct)), `
                (Fmt $base.CpuMinPct), (Fmt $r.CpuMinPct), (Fmt $base.CpuMaxPct), (Fmt $r.CpuMaxPct), `
                $base.IdleSeconds, $r.IdleSeconds, (Fmt $base.HotThreadSharePct 1), (Fmt $r.HotThreadSharePct 1), `
                (Fmt $(if ($base.Gpu.Contains('3d')) { $base.Gpu['3d'].Mean } else { $null })), (Fmt $(if ($r.Gpu.Contains('3d')) { $r.Gpu['3d'].Mean } else { $null })), `
                (Fmt $(if ($base.Gpu.Contains('copy')) { $base.Gpu['copy'].Mean } else { $null })), (Fmt $(if ($r.Gpu.Contains('copy')) { $r.Gpu['copy'].Mean } else { $null })))
    }
    Say ""
}

# cursor back where the user left it
[void][DshbWin]::SetCursorPos($cursorBefore[0], $cursorBefore[1])
Say "cursor restored to $($cursorBefore -join ',')"

if (Restore-ConfigSnapshot) { Say 'config.json differed from the run-start bytes at the end of the run (the app rewrote it); restored the original bytes' }
$leftover = @(Get-Process -Name $ProcName -ErrorAction SilentlyContinue)
Say "left running: $ProcName processes = $($leftover.Count)$(if ($leftover.Count -gt 0) { ' (ids ' + (($leftover | ForEach-Object { $_.Id }) -join ',') + ')' })"

if (-not (Test-Path -LiteralPath $OutDir)) { [void](New-Item -ItemType Directory -Path $OutDir -Force) }
$txtPath = Join-Path $OutDir "run-$RunStamp.txt"
$jsonPath = Join-Path $OutDir "run-$RunStamp.json"
[System.IO.File]::WriteAllLines($txtPath, $script:Transcript, (New-Object System.Text.UTF8Encoding($false)))
$payload = [pscustomobject]@{
    RunStamp = $RunStamp
    Exe      = $ExePath
    ExeBytes = $exeItem.Length
    ExeMtime = $exeItem.LastWriteTime.ToString('o')
    ExeSha256 = (Get-FileHash -LiteralPath $ExePath -Algorithm SHA256).Hash
    Seconds  = $Seconds
    IdlePct  = $IdlePct
    LogicalCpus = $env:NUMBER_OF_PROCESSORS
    Phases   = $results
}
[System.IO.File]::WriteAllText($jsonPath, ($payload | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding($false)))
Say "wrote: $txtPath"
Say "wrote: $jsonPath"
