using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public sealed class WindowPresentationObservation
{
    public long Window;
    public int VisibleSamples;
    public int BlackSamples;
    public int CloakedSamples;
}

// Samples the actual desktop, rather than a window's offscreen render target.
public static class WindowPresentationCapture
{
    private delegate bool EnumProc(IntPtr hwnd, IntPtr parameter);
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumProc callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr hwnd, StringBuilder name, int count);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr hwnd, out Rect rectangle);
    [DllImport("user32.dll")] private static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll")] private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("dwmapi.dll")] private static extern int DwmGetWindowAttribute(IntPtr hwnd, int attribute, out uint value, int size);

    private static Thread sampler;
    private static volatile bool stopping;
    private static Exception failure;
    private static readonly Dictionary<long, WindowPresentationObservation> observations = new Dictionary<long, WindowPresentationObservation>();
    public static string OutputDirectory;
    private static string captureId;

    public static void Start(uint processId, IntPtr[] existing)
    {
        if (sampler != null) { throw new InvalidOperationException("Presentation capture is already running."); }
        observations.Clear();
        failure = null;
        stopping = false;
        captureId = DateTime.UtcNow.Ticks.ToString();
        var known = new HashSet<IntPtr>(existing);
        sampler = new Thread(() =>
        {
            SetThreadDpiAwarenessContext(new IntPtr(-4));
            try
            {
                while (!stopping)
                {
                    EnumWindows((hwnd, unused) =>
                    {
                        uint pid;
                        GetWindowThreadProcessId(hwnd, out pid);
                        if (pid != processId || known.Contains(hwnd) || !IsWindowVisible(hwnd)) { return true; }
                        var name = new StringBuilder(64);
                        GetClassName(hwnd, name, name.Capacity);
                        if (name.ToString() != "WinUIDesktopWin32WindowClass") { return true; }

                        WindowPresentationObservation observation;
                        if (!observations.TryGetValue(hwnd.ToInt64(), out observation))
                        {
                            observation = new WindowPresentationObservation { Window = hwnd.ToInt64() };
                            observations.Add(observation.Window, observation);
                        }
                        uint cloak;
                        if (DwmGetWindowAttribute(hwnd, 14, out cloak, sizeof(uint)) == 0 && cloak != 0)
                        {
                            ++observation.CloakedSamples;
                            return true;
                        }
                        Rect rect;
                        if (!GetWindowRect(hwnd, out rect) || rect.Right - rect.Left < 200 || rect.Bottom - rect.Top < 200) { return true; }
                        int left = Math.Max(rect.Left, GetSystemMetrics(76));
                        int top = Math.Max(rect.Top, GetSystemMetrics(77));
                        int right = Math.Min(rect.Right, GetSystemMetrics(76) + GetSystemMetrics(78));
                        int bottom = Math.Min(rect.Bottom, GetSystemMetrics(77) + GetSystemMetrics(79));
                        if (right <= left || bottom <= top) { return true; }
                        int valid = 0, black = 0;
                        using (var bitmap = new Bitmap(right - left, bottom - top))
                        using (var graphics = Graphics.FromImage(bitmap))
                        {
                            // One desktop readback per sample; querying screen
                            // pixels individually can miss short startup flashes.
                            graphics.CopyFromScreen(left, top, 0, 0, bitmap.Size);
                            foreach (double y in new[] { 0.1, 0.25, 0.5 })
                            foreach (double x in new[] { 0.25, 0.5, 0.75 })
                            {
                                int sampleX = rect.Left + (int)((rect.Right - rect.Left) * x) - left;
                                int sampleY = rect.Top + (int)((rect.Bottom - rect.Top) * y) - top;
                                if (sampleX < 0 || sampleY < 0 || sampleX >= bitmap.Width || sampleY >= bitmap.Height) { continue; }
                                Color color = bitmap.GetPixel(sampleX, sampleY);
                                ++valid;
                                if (color.R < 8 && color.G < 8 && color.B < 8) { ++black; }
                            }
                            if (valid == 9 && !String.IsNullOrEmpty(OutputDirectory))
                            {
                                string kind = observation.VisibleSamples == 0 ? "first" : (black == valid && observation.BlackSamples == 0 ? "black" : null);
                                if (kind != null)
                                {
                                    Directory.CreateDirectory(OutputDirectory);
                                    bitmap.Save(Path.Combine(OutputDirectory, captureId + "-" + observation.Window + "-" + kind + ".png"), System.Drawing.Imaging.ImageFormat.Png);
                                }
                            }
                        }
                        if (valid == 9)
                        {
                            ++observation.VisibleSamples;
                            if (black == valid) { ++observation.BlackSamples; }
                        }
                        return true;
                    }, IntPtr.Zero);
                    Thread.Sleep(4);
                }
            }
            catch (Exception error) { failure = error; }
        }) { IsBackground = true };
        sampler.Start();
    }

    public static WindowPresentationObservation[] Stop()
    {
        stopping = true;
        if (sampler == null) { return new WindowPresentationObservation[0]; }
        if (!sampler.Join(5000)) { throw new TimeoutException("Presentation capture did not stop."); }
        sampler = null;
        if (failure != null) { throw new InvalidOperationException("Presentation capture failed.", failure); }
        var result = new WindowPresentationObservation[observations.Count];
        observations.Values.CopyTo(result, 0);
        return result;
    }
}
