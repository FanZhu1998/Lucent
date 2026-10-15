// Evidence for reviewing Lucent's branding (tools\branding\LucentBranding.psm1, ADR 0025): a contact
// sheet of the derivatives, and executables' icons as Windows draws them. GDI+ (System.Drawing) draws
// both; it composites in sRGB as Windows does. Nothing here makes a derivative. Loaded by Add-Type in
// Windows PowerShell 5.1 (C# 5), on its own: it takes pixels and icon resources as arrays.

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Runtime.InteropServices;

namespace LucentBrandingEvidence
{
    internal static class Drawing
    {
        public static readonly Color[] Backgrounds = { Color.FromArgb(244, 244, 244), Color.FromArgb(128, 128, 128), Color.FromArgb(30, 30, 30) };
        public static readonly string[] BackgroundNames = { "light #f4f4f4", "mid-grey #808080", "dark #1e1e1e" };
        // Dotted frames just outside each canvas, so transparent padding and placement show.
        public static readonly Color[] Frames = { Color.FromArgb(150, 150, 150), Color.FromArgb(190, 190, 190), Color.FromArgb(95, 95, 95) };

        // A dotted frame one pixel outside `inner`.
        public static void DrawFrame(Graphics graphics, Rectangle inner, int background)
        {
            graphics.PixelOffsetMode = PixelOffsetMode.None;
            graphics.SmoothingMode = SmoothingMode.None;
            using (Pen pen = new Pen(Frames[background], 1f))
            {
                pen.DashStyle = DashStyle.Dot;
                graphics.DrawRectangle(pen, inner.Left - 1, inner.Top - 1, inner.Width + 1, inner.Height + 1);
            }
        }

        // A 32-bit ARGB (straight alpha) bitmap of RGBA bytes.
        public static Bitmap FromRgba(int width, int height, byte[] rgba)
        {
            Bitmap bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb);
            BitmapData data = bitmap.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
            byte[] row = new byte[width * 4];
            for (int y = 0; y < height; y++)
            {
                for (int x = 0; x < width; x++)
                {
                    int s = (y * width + x) * 4;
                    row[x * 4] = rgba[s + 2];
                    row[x * 4 + 1] = rgba[s + 1];
                    row[x * 4 + 2] = rgba[s];
                    row[x * 4 + 3] = rgba[s + 3];
                }
                Marshal.Copy(row, 0, data.Scan0 + y * data.Stride, width * 4);
            }
            bitmap.UnlockBits(data);
            return bitmap;
        }

        // The bounding box of the pixels with at least this alpha.
        public static Rectangle Content(int width, int height, byte[] rgba, int threshold)
        {
            int left = width, top = height, right = -1, bottom = -1;
            for (int y = 0; y < height; y++)
            {
                for (int x = 0; x < width; x++)
                {
                    if (rgba[(y * width + x) * 4 + 3] < threshold) { continue; }
                    if (x < left) { left = x; }
                    if (x > right) { right = x; }
                    if (y < top) { top = y; }
                    if (y > bottom) { bottom = y; }
                }
            }
            return right < 0 ? Rectangle.Empty : Rectangle.FromLTRB(left, top, right + 1, bottom + 1);
        }

        public static void DrawPixels(Graphics graphics, Image image, Rectangle target, Rectangle source)
        {
            graphics.InterpolationMode = InterpolationMode.NearestNeighbor;
            graphics.PixelOffsetMode = PixelOffsetMode.Half;
            graphics.CompositingMode = CompositingMode.SourceOver;
            graphics.DrawImage(image, target, source, GraphicsUnit.Pixel);
        }

        public static Font LabelFont() { return new Font("Segoe UI", 9f, FontStyle.Regular, GraphicsUnit.Point); }

        public static Bitmap NewSheet(int width, int height)
        {
            Bitmap sheet = new Bitmap(Math.Max(1, width), Math.Max(1, height), PixelFormat.Format24bppRgb);
            using (Graphics graphics = Graphics.FromImage(sheet)) { graphics.Clear(Color.White); }
            return sheet;
        }
    }

    // Rows of: the label, then on each of three backgrounds the image at 1:1 and magnified with
    // nearest-neighbour sampling (small images whole; larger ones as a crop of their artwork's top-left
    // corner, where halos and clipped corners would show; the master as a crop across its artwork's
    // left edge, where the generator clears faint pixels), each inside a dotted frame that marks the
    // canvas (or the crop).
    public sealed class ContactSheet
    {
        private sealed class Item
        {
            public string Label;
            public string Role;
            public Bitmap Image;
            public Rectangle Content;
            public bool Master;
        }

        private const int LabelWidth = 330;
        private const int Gap = 12;
        private const int MagnifiedTarget = 192;
        private const int CornerCrop = 40;
        private readonly List<Item> items = new List<Item>();

        public void AddMaster(string label, int width, int height, byte[] rgba)
        {
            Item item = new Item();
            item.Label = label;
            item.Role = "master";
            item.Image = Drawing.FromRgba(width, height, rgba);
            item.Content = Drawing.Content(width, height, rgba, 128);  // the silhouette, not the master's faint specks
            item.Master = true;
            items.Add(item);
        }

        public void Add(string label, int width, int height, byte[] rgba, string role)
        {
            Item item = new Item();
            item.Label = label;
            item.Role = role;
            item.Image = Drawing.FromRgba(width, height, rgba);
            item.Content = Drawing.Content(width, height, rgba, 1);
            items.Add(item);
        }

        // What one background panel of an item shows: the 1:1 (or, for the master, scaled) image and the
        // magnified view, side by side.
        private static Size PanelSize(Item item, out Rectangle magnifiedSource, out int zoom)
        {
            int width = item.Image.Width, height = item.Image.Height;
            if (item.Master)
            {
                magnifiedSource = new Rectangle(item.Content.Left - 12, item.Content.Top + item.Content.Height / 2 - 12, 24, 24);
                zoom = 8;
                return new Size(256 + Gap + magnifiedSource.Width * zoom, Math.Max(256, magnifiedSource.Height * zoom));
            }
            if (Math.Max(width, height) <= 128)
            {
                magnifiedSource = new Rectangle(0, 0, width, height);
                zoom = Math.Max(2, Math.Min(12, MagnifiedTarget / Math.Max(width, height)));
            }
            else
            {
                magnifiedSource = new Rectangle(Math.Max(0, item.Content.Left - 4), Math.Max(0, item.Content.Top - 4), CornerCrop, CornerCrop);
                zoom = 4;
            }
            return new Size(width + Gap + magnifiedSource.Width * zoom, Math.Max(height, magnifiedSource.Height * zoom));
        }

        public void Save(string path, string title)
        {
            int panelWidth = 0;
            List<int> heights = new List<int>();
            foreach (Item item in items)
            {
                Rectangle source;
                int zoom;
                Size size = PanelSize(item, out source, out zoom);
                panelWidth = Math.Max(panelWidth, size.Width);
                heights.Add(Math.Max(size.Height, 40));
            }
            int headerHeight = 56;
            int width = LabelWidth + 3 * (panelWidth + 2 * Gap);
            int height = headerHeight;
            foreach (int h in heights) { height += h + 2 * Gap; }
            using (Bitmap sheet = Drawing.NewSheet(width, height))
            using (Graphics graphics = Graphics.FromImage(sheet))
            using (Font font = Drawing.LabelFont())
            using (Font bold = new Font(font, FontStyle.Bold))
            {
                graphics.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
                graphics.DrawString(title, bold, Brushes.Black, Gap, 8);
                for (int b = 0; b < 3; b++)
                {
                    graphics.DrawString(Drawing.BackgroundNames[b] + ": 1:1, then magnified (nearest neighbour)", font, Brushes.Black, LabelWidth + b * (panelWidth + 2 * Gap) + Gap, 32);
                }
                int y = headerHeight;
                for (int i = 0; i < items.Count; i++)
                {
                    Item item = items[i];
                    int rowHeight = heights[i] + 2 * Gap;
                    graphics.DrawString(item.Label + "\n" + item.Role + ", " + item.Image.Width + "x" + item.Image.Height + " px", font, Brushes.Black,
                                        new RectangleF(Gap, y + Gap, LabelWidth - 2 * Gap, rowHeight - 2 * Gap));
                    for (int b = 0; b < 3; b++)
                    {
                        int x = LabelWidth + b * (panelWidth + 2 * Gap);
                        using (SolidBrush brush = new SolidBrush(Drawing.Backgrounds[b])) { graphics.FillRectangle(brush, x, y, panelWidth + 2 * Gap, rowHeight); }
                        Rectangle source;
                        int zoom;
                        PanelSize(item, out source, out zoom);
                        int left = x + Gap;
                        if (item.Master)
                        {
                            graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
                            graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
                            graphics.CompositingMode = CompositingMode.SourceOver;
                            graphics.DrawImage(item.Image, new Rectangle(left, y + Gap, 256, 256));
                            Drawing.DrawFrame(graphics, new Rectangle(left, y + Gap, 256, 256), b);
                            left += 256 + Gap;
                        }
                        else
                        {
                            Rectangle actual = new Rectangle(left, y + Gap, item.Image.Width, item.Image.Height);
                            Drawing.DrawPixels(graphics, item.Image, actual, new Rectangle(0, 0, item.Image.Width, item.Image.Height));
                            Drawing.DrawFrame(graphics, actual, b);
                            left += item.Image.Width + Gap;
                        }
                        Rectangle magnified = new Rectangle(left, y + Gap, source.Width * zoom, source.Height * zoom);
                        Drawing.DrawPixels(graphics, item.Image, magnified, source);
                        Drawing.DrawFrame(graphics, magnified, b);
                    }
                    y += rowHeight;
                }
                sheet.Save(path, ImageFormat.Png);
            }
        }
    }

    // Executables' icons as Windows draws them: for each size, the group's best image
    // (LookupIconIdFromDirectoryEx), made an icon (CreateIconFromResourceEx) and drawn (DrawIconEx) on a
    // 32-bit bitmap of a light, a grey and a dark background, then magnified 4x with nearest-neighbour
    // sampling up to 48 px.
    public sealed class ProgramIconSheet
    {
        private sealed class Program
        {
            public string Label;
            public byte[] Directory;
            public int[] Ids;
            public byte[][] Data;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct BITMAPINFOHEADER
        {
            public int Size;
            public int Width;
            public int Height;
            public ushort Planes;
            public ushort BitCount;
            public uint Compression;
            public uint SizeImage;
            public int XPelsPerMeter;
            public int YPelsPerMeter;
            public uint ClrUsed;
            public uint ClrImportant;
        }

        [DllImport("user32.dll", SetLastError = true)]
        private static extern int LookupIconIdFromDirectoryEx(byte[] directory, bool icon, int width, int height, uint flags);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr CreateIconFromResourceEx(byte[] bits, uint size, bool icon, uint version, int width, int height, uint flags);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool DrawIconEx(IntPtr dc, int x, int y, IntPtr icon, int width, int height, uint step, IntPtr brush, uint flags);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool DestroyIcon(IntPtr icon);

        [DllImport("gdi32.dll", SetLastError = true)]
        private static extern IntPtr CreateCompatibleDC(IntPtr dc);

        [DllImport("gdi32.dll", SetLastError = true)]
        private static extern IntPtr CreateDIBSection(IntPtr dc, ref BITMAPINFOHEADER header, uint usage, out IntPtr bits, IntPtr section, uint offset);

        [DllImport("gdi32.dll")]
        private static extern IntPtr SelectObject(IntPtr dc, IntPtr gdiObject);

        [DllImport("gdi32.dll")]
        private static extern bool DeleteObject(IntPtr gdiObject);

        [DllImport("gdi32.dll")]
        private static extern bool DeleteDC(IntPtr dc);

        [DllImport("gdi32.dll")]
        private static extern bool GdiFlush();

        private const int Gap = 12;
        private const int LabelWidth = 330;
        private readonly List<Program> programs = new List<Program>();

        public void Add(string label, byte[] directory, int[] ids, byte[][] data)
        {
            Program program = new Program();
            program.Label = label;
            program.Directory = directory;
            program.Ids = ids;
            program.Data = data;
            programs.Add(program);
        }

        // The icon drawn by Windows at size x size on an opaque background; also which image it chose. It
        // is drawn on a 32-bit bitmap (a DIB section), where DrawIconEx blends the images' alpha: on a
        // 24-bit GDI+ bitmap it drew the soft edge pixels at full opacity (measured on 2026-10-14 with
        // Lucent's icon at 16 to 256 px: up to 228 levels from the image composited on the same grey; on
        // a 32-bit bitmap within 1).
        private static Bitmap Draw(Program program, int size, Color background, out int chosenId)
        {
            chosenId = LookupIconIdFromDirectoryEx(program.Directory, true, size, size, 0);
            byte[] data = null;
            for (int i = 0; i < program.Ids.Length; i++) { if (program.Ids[i] == chosenId) { data = program.Data[i]; } }
            if (data == null) { throw new InvalidOperationException(program.Label + ": Windows chose icon " + chosenId + ", which the group does not have"); }
            IntPtr icon = CreateIconFromResourceEx(data, (uint)data.Length, true, 0x00030000, size, size, 0);
            if (icon == IntPtr.Zero) { throw new InvalidOperationException(program.Label + ": Windows cannot read icon " + chosenId + " (error " + Marshal.GetLastWin32Error() + ")"); }
            IntPtr dc = IntPtr.Zero;
            IntPtr section = IntPtr.Zero;
            IntPtr previous = IntPtr.Zero;
            try
            {
                dc = CreateCompatibleDC(IntPtr.Zero);
                if (dc == IntPtr.Zero) { throw new InvalidOperationException("CreateCompatibleDC failed (error " + Marshal.GetLastWin32Error() + ")"); }
                BITMAPINFOHEADER header = new BITMAPINFOHEADER();
                header.Size = Marshal.SizeOf(typeof(BITMAPINFOHEADER));
                header.Width = size;
                header.Height = -size;  // top-down
                header.Planes = 1;
                header.BitCount = 32;
                IntPtr bits;
                section = CreateDIBSection(dc, ref header, 0, out bits, IntPtr.Zero, 0);
                if (section == IntPtr.Zero) { throw new InvalidOperationException("CreateDIBSection failed (error " + Marshal.GetLastWin32Error() + ")"); }
                previous = SelectObject(dc, section);
                byte[] pixels = new byte[size * size * 4];  // BGRA
                for (int i = 0; i < pixels.Length; i += 4)
                {
                    pixels[i] = background.B;
                    pixels[i + 1] = background.G;
                    pixels[i + 2] = background.R;
                    pixels[i + 3] = 255;
                }
                Marshal.Copy(pixels, 0, bits, pixels.Length);
                if (!DrawIconEx(dc, 0, 0, icon, size, size, 0, IntPtr.Zero, 3))
                {
                    throw new InvalidOperationException(program.Label + ": DrawIconEx failed (error " + Marshal.GetLastWin32Error() + ")");
                }
                GdiFlush();
                Marshal.Copy(bits, pixels, 0, pixels.Length);
                byte[] rgba = new byte[pixels.Length];
                for (int i = 0; i < pixels.Length; i += 4)
                {
                    rgba[i] = pixels[i + 2];
                    rgba[i + 1] = pixels[i + 1];
                    rgba[i + 2] = pixels[i];
                    rgba[i + 3] = 255;
                }
                return Drawing.FromRgba(size, size, rgba);
            }
            finally
            {
                if (previous != IntPtr.Zero) { SelectObject(dc, previous); }
                if (section != IntPtr.Zero) { DeleteObject(section); }
                if (dc != IntPtr.Zero) { DeleteDC(dc); }
                DestroyIcon(icon);
            }
        }

        public void Save(string path, int[] sizes)
        {
            int columnWidth = 0;
            foreach (int size in sizes) { columnWidth += size + (size <= 48 ? size * 4 + Gap : 0) + 2 * Gap; }
            int rowHeight = Gap;
            foreach (int size in sizes) { rowHeight = Math.Max(rowHeight, Math.Max(size, size <= 48 ? size * 4 : 0) + 2 * Gap); }
            int headerHeight = 56;
            int width = LabelWidth + 3 * columnWidth;
            int height = headerHeight + programs.Count * (rowHeight + 24);
            using (Bitmap sheet = Drawing.NewSheet(width, height))
            using (Graphics graphics = Graphics.FromImage(sheet))
            using (Font font = Drawing.LabelFont())
            using (Font bold = new Font(font, FontStyle.Bold))
            {
                graphics.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
                graphics.DrawString("Executables' icons as Windows draws them (LookupIconIdFromDirectoryEx, CreateIconFromResourceEx, DrawIconEx on a 32-bit bitmap) at " + string.Join(", ", Array.ConvertAll(sizes, delegate(int s) { return s.ToString(); })) + " px; up to 48 px also magnified 4x", bold, Brushes.Black, Gap, 8);
                for (int b = 0; b < 3; b++) { graphics.DrawString(Drawing.BackgroundNames[b], font, Brushes.Black, LabelWidth + b * columnWidth + Gap, 32); }
                int y = headerHeight;
                foreach (Program program in programs)
                {
                    List<string> chosen = new List<string>();
                    for (int b = 0; b < 3; b++)
                    {
                        int x = LabelWidth + b * columnWidth;
                        using (SolidBrush brush = new SolidBrush(Drawing.Backgrounds[b])) { graphics.FillRectangle(brush, x, y, columnWidth, rowHeight); }
                        x += Gap;
                        foreach (int size in sizes)
                        {
                            int id;
                            using (Bitmap tile = Draw(program, size, Drawing.Backgrounds[b], out id))
                            {
                                if (b == 0) { chosen.Add(size + " px: icon " + id); }
                                Drawing.DrawPixels(graphics, tile, new Rectangle(x, y + Gap, size, size), new Rectangle(0, 0, size, size));
                                Drawing.DrawFrame(graphics, new Rectangle(x, y + Gap, size, size), b);
                                x += size + Gap;
                                if (size <= 48)
                                {
                                    Drawing.DrawPixels(graphics, tile, new Rectangle(x, y + Gap, size * 4, size * 4), new Rectangle(0, 0, size, size));
                                    Drawing.DrawFrame(graphics, new Rectangle(x, y + Gap, size * 4, size * 4), b);
                                    x += size * 4 + Gap;
                                }
                                x += Gap;
                            }
                        }
                    }
                    graphics.DrawString(program.Label + "\n" + string.Join(", ", chosen.ToArray()), font, Brushes.Black, new RectangleF(Gap, y + Gap, LabelWidth - 2 * Gap, rowHeight));
                    y += rowHeight + 24;
                }
                sheet.Save(path, ImageFormat.Png);
            }
        }
    }
}
