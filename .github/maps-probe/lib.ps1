# The probe's screenshot of the whole desktop.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
function Shot($path) {
  try {
    $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bmp = New-Object Drawing.Bitmap $b.Width, $b.Height
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
    $bmp.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
  } catch { "screenshot failed: $_" }
}

# A left click inside the first top-level window with this title, at a
# fraction of its client area.
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class ProbeWin {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
  public struct RECT { public int l, t, r, b; }
  public struct POINT { public int x, y; }
  public static IntPtr Find(string title) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { var s = new StringBuilder(256); GetWindowText(h, s, 256);
      if (IsWindowVisible(h) && s.ToString() == title) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  public static bool Click(string title, double fx, double fy) {
    IntPtr h = Find(title); if (h == IntPtr.Zero) return false;
    RECT r; GetClientRect(h, out r);
    POINT p; p.x = (int)(r.r * fx); p.y = (int)(r.b * fy); ClientToScreen(h, ref p);
    SetForegroundWindow(h); SetCursorPos(p.x, p.y);
    mouse_event(2, 0, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(60); mouse_event(4, 0, 0, 0, UIntPtr.Zero);
    return true;
  }
}
"@
function ClickIn($title, $fx, $fy) { if (-not [ProbeWin]::Click($title, $fx, $fy)) { "no window titled $title" } }
