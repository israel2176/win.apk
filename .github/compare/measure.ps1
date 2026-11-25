# One step of comparison for one app in one runtime: its install, or a run of
# -Seconds seconds (cold: nothing of the runtime running before it; warm: right after a cold
# run, as a user opening the app a second time). Writes <Out>\<runtime>-<phase>.json, the
# runtime's logs and PNGs. Both runtimes are measured by the same code from outside:
#   first frame   the runtime's own "first frame" log line, timed by this script's clock
#                 from the launch command (Win.apk: "[win.apk] first frame presented" or
#                 "native window: first frame"; the other runtime: "first frame drawn (N presented)")
#   non-blank     the app's window read back (PrintWindow, PW_CLIENTONLY|PW_RENDERFULLCONTENT,
#                 as the other runtime's own e2e reads it, and a copy of the screen under it while nothing of
#                 another process covers it): the share of pixels away from its most common color
#                 (24 per channel, the other runtime's e2e rule); "drawn" at the end of the run is 0.5% or
#                 more, the other runtime's e2e threshold, or a sparse screen of anti-aliased text and icons
#                 (edges and colors: CmpWin.Judge)
#   memory        the peak, over 1-second samples, of the summed working sets of the run's
#                 processes: every process whose image is in the runtime's folder, and their
#                 descendants (WebView2's, a translator's). the other runtime's waiting launcher (the
#                 dalvikvm of com.other.app.Launcher) is kept apart: a Start menu or Explorer
#                 launch detaches and ends it (other-launch, docs/installer.md)
#   idle CPU      the run's processes' CPU time over the last 30 s of the run, in % of one core
param(
  [Parameter(Mandatory)] [ValidateSet('winapk', 'other')] [string] $Runtime,
  [Parameter(Mandatory)] [ValidateSet('install', 'cold', 'warm')] [string] $Phase,
  [Parameter(Mandatory)] [string] $Apk,
  [Parameter(Mandatory)] [string] $Package,
  [Parameter(Mandatory)] [string] $Out,
  [int] $Seconds = 60,
  [string] $WinApkRoot = 'C:\cmp\winapk',
  [string] $WinApkData = 'C:\cmp\winapk-data'
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$the other runtimeApp = Join-Path $env:LOCALAPPDATA 'Programs\the other runtime'
$root = if ($Runtime -eq 'winapk') { $WinApkRoot } else { $the other runtimeApp }
$tag = "$Runtime-$Phase"
$result = [ordered]@{ runtime = $Runtime; phase = $Phase; package = $Package; seconds = $Seconds;
  started_utc = [DateTime]::UtcNow.ToString('o'); display = "$env:WINAPK_PHONE_SIZE / $env:OTHER_DISPLAY" }

function Save() { $result | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $Out "$tag.json") }
# A log another process is still writing.
function ReadText([string] $f) {
  try {
    $s = [IO.FileStream]::new($f, 'Open', 'Read', [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try { return [IO.StreamReader]::new($s).ReadToEnd() } finally { $s.Dispose() }
  } catch { return '' }
}
# `cmd /c <command line> > log 2>&1`, as the other runtime's e2e starts other-app (StartLogged): the same
# for both runtimes, so that each one's output is one file. Returns the cmd process.
function StartLogged([string] $commandLine, [string] $log) {
  $psi = [Diagnostics.ProcessStartInfo]::new('cmd.exe', "/d /s /c `"$commandLine > `"$log`" 2>&1`"")
  $psi.UseShellExecute = $false
  $psi.WorkingDirectory = $root
  return [Diagnostics.Process]::Start($psi)
}

Add-Type -AssemblyName System.Drawing
if (-not ('CmpWin' -as [type])) {
  # By location: PowerShell 7 does not resolve System.Drawing's facade by name for the compiler.
  $drawing = @([Drawing.Bitmap].Assembly.Location, [Drawing.Rectangle].Assembly.Location) | Sort-Object -Unique
  # Naming any assembly replaces Add-Type's defaults, so the facades the code's other namespaces
  # (List<>, Marshal, DllImport) compile against are named too.
  # And what System.Drawing's own types name (.NET 10: System.Private.Windows.GdiPlus' IGraphics).
  $drawingDeps = foreach ($a in [Drawing.Bitmap].Assembly.GetReferencedAssemblies()) {
    try { [Reflection.Assembly]::Load($a).Location } catch { }
  }
  $refs = @($drawing) + @($drawingDeps | Where-Object { $_ }) +
          @('System.Runtime', 'System.Collections', 'System.Runtime.InteropServices', 'System.Console', 'System.Linq')
  $refs = $refs | Sort-Object -Unique
  Add-Type -ReferencedAssemblies $refs -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class CmpWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h, uint cmd);
  // Visible top-level windows with no owner: {hwnd, pid, client width, client height}.
  public static List<long[]> Windows() {
    var list = new List<long[]>();
    EnumWindows((h, l) => {
      if (!IsWindowVisible(h) || GetWindow(h, 4 /* GW_OWNER */) != IntPtr.Zero) return true;
      uint pid; GetWindowThreadProcessId(h, out pid);
      RECT r; GetClientRect(h, out r);
      list.Add(new long[] { h.ToInt64(), pid, r.Right - r.Left, r.Bottom - r.Top });
      return true;
    }, IntPtr.Zero);
    return list;
  }
  public static Bitmap Print(IntPtr h) {
    RECT r; if (!GetClientRect(h, out r) || r.Right <= 0 || r.Bottom <= 0) return null;
    var b = new Bitmap(r.Right, r.Bottom, PixelFormat.Format32bppRgb);
    using (var g = Graphics.FromImage(b)) {
      IntPtr dc = g.GetHdc();
      bool ok = PrintWindow(h, dc, 1 /* PW_CLIENTONLY */ | 2 /* PW_RENDERFULLCONTENT */);
      g.ReleaseHdc(dc);
      if (!ok) { b.Dispose(); return null; }
    }
    return b;
  }
  public static Bitmap Screen(IntPtr h) {
    RECT r; if (!GetClientRect(h, out r) || r.Right <= 0 || r.Bottom <= 0) return null;
    var p = new POINT(); ClientToScreen(h, ref p);
    var b = new Bitmap(r.Right, r.Bottom, PixelFormat.Format32bppRgb);
    using (var g = Graphics.FromImage(b)) g.CopyFromScreen(p.X, p.Y, 0, 0, b.Size);
    return b;
  }
  static bool Near(int a, int b) {
    return Math.Abs(((a >> 16) & 255) - ((b >> 16) & 255)) <= 24 && Math.Abs(((a >> 8) & 255) - ((b >> 8) & 255)) <= 24 &&
        Math.Abs((a & 255) - (b & 255)) <= 24;
  }
  // What a window read back shows. Share: the share (0..1) of pixels away from the most common
  // color (the other runtime's e2e rule). Edges: the share of neighbouring pixel pairs, across and down, that
  // differ as much. Colors: the distinct colors away from the background. Drawn: Share from 0.5%
  // (the other runtime's e2e threshold), or a sparse screen with anti-aliased text or icons on it (edges from
  // 0.4%, 32 colors or more): an app's first screen can be an empty note or list under a toolbar
  // of icons (Fossify Notes: 0.31%, 0.58%, 90+ colors in both runtimes), where a blank window has
  // no edge, a hairline one color and a lone 48 px progress ring 0.24% edges.
  public class Look { public double Share, Edges; public int Colors; public bool Drawn; }
  public static Look Judge(Bitmap b) {
    var look = new Look();
    if (b == null || b.Width == 0 || b.Height == 0) return look;
    int w = b.Width, h = b.Height;
    var d = b.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppRgb);
    var px = new int[w * h];
    for (int y = 0; y < h; y++) Marshal.Copy(IntPtr.Add(d.Scan0, y * d.Stride), px, y * w, w);
    b.UnlockBits(d);
    var counts = new Dictionary<int, int>();
    for (int i = 0; i < px.Length; i++) { int c = px[i] & 0xffffff, n; px[i] = c; counts.TryGetValue(c, out n); counts[c] = n + 1; }
    int bg = 0, best = -1;
    foreach (var kv in counts) if (kv.Value > best) { best = kv.Value; bg = kv.Key; }
    int other = 0, colors = 0; long edges = 0;
    foreach (var kv in counts) if (!Near(kv.Key, bg)) colors++;
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        int c = px[y * w + x];
        if (!Near(c, bg)) other++;
        if (x + 1 < w && !Near(c, px[y * w + x + 1])) edges++;
        if (y + 1 < h && !Near(c, px[(y + 1) * w + x])) edges++;
      }
    }
    look.Share = (double)other / px.Length;
    look.Edges = (double)edges / px.Length;
    look.Colors = colors;
    look.Drawn = look.Share >= 0.005 || (look.Edges >= 0.004 && look.Colors >= 32);
    return look;
  }
  [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr h, uint flags);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
  // Whether the screen shows this app at its window's client centre and four inner corners: the
  // window itself, one it owns or another of its process's but a console (a dialog a runtime shows
  // as its own window). A copy of the screen under a covered window reads whatever covers it
  // (run 45513190823's the other runtime warm runs: a console of log lines over the app).
  public static bool OnTop(IntPtr h) {
    RECT r; if (!GetClientRect(h, out r) || r.Right <= 0 || r.Bottom <= 0) return false;
    uint pid; GetWindowThreadProcessId(h, out pid);
    int[][] at = { new[] { r.Right / 2, r.Bottom / 2 }, new[] { r.Right / 8, r.Bottom / 8 },
                   new[] { r.Right * 7 / 8, r.Bottom / 8 }, new[] { r.Right / 8, r.Bottom * 7 / 8 },
                   new[] { r.Right * 7 / 8, r.Bottom * 7 / 8 } };
    foreach (var a in at) {
      var p = new POINT { X = a[0], Y = a[1] }; ClientToScreen(h, ref p);
      IntPtr top = WindowFromPoint(p);
      if (top == IntPtr.Zero) return false;
      IntPtr root = GetAncestor(top, 2 /* GA_ROOT */);
      if (root == h || GetAncestor(top, 3 /* GA_ROOTOWNER */) == h) continue;
      // A console window answers for the process attached to it, so it is named by its class.
      var cls = new System.Text.StringBuilder(64); GetClassNameW(root, cls, cls.Capacity);
      string c = cls.ToString();
      uint other; GetWindowThreadProcessId(root, out other);
      if (other != pid || c == "ConsoleWindowClass" || c == "CASCADIA_HOSTING_WINDOW_CLASS") return false;
    }
    return true;
  }
}
'@
}

# The end of the run's verdict on the app's window $h, read back with PrintWindow and from the
# screen (PNGs of both kept): drawn when either read is (CmpWin.Judge); the screen's read counts
# only while the window is on top there, as a covering console would otherwise read as drawn.
function JudgeEnd([IntPtr] $h) {
  $looks = @()
  foreach ($way in 'print', 'screen') {
    $b = if ($way -eq 'print') { [CmpWin]::Print($h) } else { [CmpWin]::Screen($h) }
    if (-not $b) { continue }
    $l = [CmpWin]::Judge($b)
    $result["nonblank_pct_$way"] = [Math]::Round(100 * $l.Share, 2)
    $result["edges_pct_$way"] = [Math]::Round(100 * $l.Edges, 2)
    $result["colors_$way"] = $l.Colors
    $b.Save((Join-Path $Out "$tag.end-$way.png"), [Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
    if ($way -eq 'screen' -and -not [CmpWin]::OnTop($h)) { $result.screen_covered = $true; continue }
    $looks += $l
  }
  $best = $looks | Sort-Object { [int]$_.Drawn }, { $_.Share } -Descending | Select-Object -First 1
  if ($best) {
    $result.nonblank_pct = [Math]::Round(100 * $best.Share, 2)
    $result.edges_pct = [Math]::Round(100 * $best.Edges, 2)
    $result.colors = $best.Colors
    $result.drawn = $best.Drawn
  }
}

# The run's processes now: {pid -> process}, the waiting the other runtime launcher apart.
function Sample() {
  $all = @{}
  foreach ($p in Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId, ExecutablePath, CommandLine, Name, WorkingSetSize, KernelModeTime, UserModeTime) {
    $all[[int]$p.ProcessId] = $p
  }
  $in = @{}
  $folder = $root.TrimEnd('\') + '\'
  foreach ($p in $all.Values) {
    if ($p.ExecutablePath -and $p.ExecutablePath.StartsWith($folder, [StringComparison]::OrdinalIgnoreCase)) { $in[[int]$p.ProcessId] = $true }
  }
  do {  # their descendants, by parent
    $grew = $false
    foreach ($p in $all.Values) {
      $id = [int]$p.ProcessId
      if (-not $in.ContainsKey($id) -and $id -ne 0 -and $in.ContainsKey([int]$p.ParentProcessId) -and $p.ParentProcessId -ne $p.ProcessId) {
        $in[$id] = $true; $grew = $true
      }
    }
  } while ($grew)
  $set = @{}; $launcher = @{}
  foreach ($id in $in.Keys) {
    $p = $all[$id]
    if ($Runtime -eq 'other' -and "$($p.CommandLine)" -match 'com\.other\.app\.Launcher') { $launcher[$id] = $p } else { $set[$id] = $p }
  }
  return @{ Set = $set; Launcher = $launcher }
}
function SumMB($procs) { ($procs.Values | Measure-Object -Property WorkingSetSize -Sum).Sum / 1MB }
function CpuOf($p) { ([double]$p.KernelModeTime + [double]$p.UserModeTime) / 1e7 }

# The app's window: the largest visible top-level window of the run's processes.
function AppWindow($set) {
  $best = $null
  foreach ($w in [CmpWin]::Windows()) {
    if ($set.ContainsKey([int]$w[1]) -and $w[2] -ge 100 -and $w[3] -ge 100 -and ($null -eq $best -or $w[2] * $w[3] -gt $best[2] * $best[3])) { $best = $w }
  }
  return $best
}

if ($Phase -eq 'install') {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $log = Join-Path $Out "$tag.log"
  $p = if ($Runtime -eq 'winapk') {
    StartLogged "`"$WinApkRoot\bin\winapk-runtime.exe`" --apk `"$Apk`" --data `"$WinApkData`" --install-only" $log
  } else {
    StartLogged "`"$the other runtimeApp\other-app.cmd`" install `"$Apk`"" $log
  }
  $done = $p.WaitForExit(25 * 60 * 1000)
  if (-not $done) { taskkill /F /T /PID $p.Id | Out-Null }
  $p.WaitForExit()
  $result.install_s = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
  $result.exit = if ($done) { $p.ExitCode } else { 'timed out' }
  $text = ReadText $log
  # the other runtime's own e2e takes an install as done on exit 0 and its "installed <package>" line.
  $result.install_ok = $done -and $p.ExitCode -eq 0 -and
    ($Runtime -eq 'winapk' -or $text -match "installed $([regex]::Escape($Package))|$([regex]::Escape($Package)) is installed")
  Save
  "$tag`: ok $($result.install_ok), exit $($result.exit) after $($result.install_s) s"
  exit 0
}

# --- A run.
$log = Join-Path $Out "$tag.log"
$appLog = Join-Path $env:LOCALAPPDATA "the other runtime\logs\$Package.log"  # the other runtime's app process writes there too
$appLogMark = (ReadText $appLog).Length
if ($Runtime -eq 'other') {
  $state = ReadText (Join-Path $env:LOCALAPPDATA 'the other runtime\data\system\other-device')
  $result.device_up_at_start = [bool]($state -match 'system_server=\d+')
}
$sw = [Diagnostics.Stopwatch]::StartNew()
if ($Runtime -eq 'winapk') {
  New-Item -ItemType Directory -Force $WinApkData | Out-Null
  # What the launcher runs for an installed app (its library's data folder aside).
  $p = StartLogged "`"$WinApkRoot\bin\winapk-runtime.exe`" --apk `"$Apk`" --data `"$WinApkData`"" $log
  $frameRx = '\[win\.apk\] (first frame presented|native window: first frame)'
} else {
  # The Start menu shortcut's command (other-app launch <package>), with a shown window; with its
  # output redirected the launch waits for the app, as on a console (docs/installer.md).
  $p = StartLogged "`"$the other runtimeApp\other-app.cmd`" launch $Package --window" $log
  $frameRx = 'first frame drawn \(\d+ presented\)'
}
$peak = 0.0; $peakLauncher = 0.0; $peakNames = ''
$idleFrom = if ($Seconds -ge 45) { $Seconds - 30 } else { [int]($Seconds / 2) }
$idleStart = $null; $last = $null; $lastT = 0
$frameAt = $null; $nonBlankAt = $null; $exitedAt = $null
$next = 0.0
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
  $t = $sw.Elapsed.TotalSeconds
  if ($null -eq $frameAt) {
    $text = ReadText $log
    if ($Runtime -eq 'other') { $a = ReadText $appLog; if ($a.Length -ge $appLogMark) { $text += $a.Substring($appLogMark) } }
    if ($text -match $frameRx) { $frameAt = [Math]::Round($t, 2) }
  }
  if ($t -ge $next) {
    $next = $t + 1.0
    $s = Sample
    $last = $s; $lastT = $sw.Elapsed.TotalSeconds
    $mb = SumMB $s.Set
    if ($mb -gt $peak) { $peak = $mb; $peakNames = (($s.Set.Values | ForEach-Object { $_.Name }) | Sort-Object | Get-Unique) -join ' ' }
    $peakLauncher = [Math]::Max($peakLauncher, (SumMB $s.Launcher))
    if ($null -eq $idleStart -and $lastT -ge $idleFrom) { $idleStart = @{ T = $lastT; Set = $s.Set } }
    if ($null -eq $nonBlankAt) {
      $w = AppWindow $s.Set
      if ($w) {
        $b = [CmpWin]::Print([IntPtr]$w[0])
        if ($b -and [CmpWin]::Judge($b).Drawn) {
          $nonBlankAt = [Math]::Round($lastT, 1)
          $b.Save((Join-Path $Out "$tag.first-nonblank.png"), [Drawing.Imaging.ImageFormat]::Png)
        }
        if ($b) { $b.Dispose() }
      }
    }
  }
  if ($null -eq $exitedAt -and $p.HasExited) { $exitedAt = [Math]::Round($sw.Elapsed.TotalSeconds, 1) }
  if ($null -ne $exitedAt -and $sw.Elapsed.TotalSeconds -gt $exitedAt + 3) { break }  # nothing more to see
  Start-Sleep -Milliseconds 250
}
$result.first_frame_s = $frameAt
$result.first_nonblank_s = $nonBlankAt
$self = [regex]::Match((ReadText $log), 'startup: first frame at ([\d.]+) s')  # Win.apk's own clock, from process start
$result.first_frame_self_s = if ($self.Success) { [double]$self.Groups[1].Value } else { $null }
$result.peak_mb = [Math]::Round($peak)
$result.peak_processes = $peakNames
if ($Runtime -eq 'other') { $result.peak_launcher_mb = [Math]::Round($peakLauncher) }
$result.exited_by_itself_s = $exitedAt
$result.exit = if ($p.HasExited) { $p.ExitCode } else { $null }

# Idle CPU: the processes' CPU time from the sample at the window's start to the last one.
$result.idle_cpu_pct = $null
if ($idleStart -and $last -and $lastT -gt $idleStart.T + 5 -and $null -eq $exitedAt) {
  $cpu = 0.0
  foreach ($id in $last.Set.Keys) {
    $now = CpuOf $last.Set[$id]
    $then = if ($idleStart.Set.ContainsKey($id)) { CpuOf $idleStart.Set[$id] } else { 0 }
    $cpu += [Math]::Max(0, $now - $then)
  }
  $result.idle_cpu_pct = [Math]::Round(100 * $cpu / ($lastT - $idleStart.T), 1)
  $result.idle_window_s = [Math]::Round($lastT - $idleStart.T, 1)
}

# The end of the run: the window read back two ways, and the whole screen.
$s = Sample
$w = AppWindow $s.Set
$result.nonblank_pct = 0.0; $result.drawn = $false
if ($w) {
  $h = [IntPtr]$w[0]
  $result.window = "$($w[2])x$($w[3])"
  [void][CmpWin]::SetForegroundWindow($h)
  Start-Sleep -Milliseconds 700
  JudgeEnd $h
}
try {
  Add-Type -AssemblyName System.Windows.Forms
  $v = [Windows.Forms.SystemInformation]::VirtualScreen
  $bmp = New-Object Drawing.Bitmap $v.Width, $v.Height
  $g = [Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($v.Left, $v.Top, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $Out "$tag.screen.png"), [Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
} catch { "screenshot failed: $_" }

# Close it as a user does (WM_CLOSE to its windows), and give it 20 s to end.
$closeAt = $sw.Elapsed.TotalSeconds
foreach ($x in [CmpWin]::Windows()) { if ($s.Set.ContainsKey([int]$x[1])) { [void][CmpWin]::PostMessageW([IntPtr]$x[0], 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) } }
$result.closed_cleanly = $p.HasExited -or $p.WaitForExit(20000)
if (-not $result.closed_cleanly) {
  if ($Runtime -eq 'winapk') { taskkill /F /T /PID $p.Id 2>&1 | Out-Null }
  else {
    # Not the tree: a cold launch's launcher started the device, which a warm run keeps.
    $owners = @{}; foreach ($x in [CmpWin]::Windows()) { if ($s.Set.ContainsKey([int]$x[1])) { $owners[[int]$x[1]] = $true } }
    foreach ($id in @($owners.Keys) + @($s.Launcher.Keys)) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
    Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe' OR Name = 'pwsh.exe'" |
      Where-Object { "$($_.CommandLine)" -match 'other-app\.ps1' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
  }
}
$result.close_s = [Math]::Round($sw.Elapsed.TotalSeconds - $closeAt, 1)
if ($Runtime -eq 'other' -and (Test-Path $appLog)) {
  $a = ReadText $appLog
  Set-Content -Encoding utf8 (Join-Path $Out "$tag.app.log") $(if ($a.Length -ge $appLogMark) { $a.Substring($appLogMark) } else { $a })
}
Save
"$tag`: first frame $($result.first_frame_s) s, non-blank $($result.first_nonblank_s) s, $($result.nonblank_pct)% drawn at the end, peak $($result.peak_mb) MB, idle CPU $($result.idle_cpu_pct)%"
