// The image code of Lucent's branding generator (tools\branding\LucentBranding.psm1, ADR 0025). Loaded
// by Add-Type in Windows PowerShell 5.1, so C# 5 only, and without System.Drawing: GDI+ never touches a
// derivative (LucentBrandingEvidence.cs, which draws the contact sheets, does).
//
//   Png            strict reader (8-bit RGBA, not interlaced; every chunk CRC and the zlib checksum
//                  checked; inflating is .NET's DeflateStream) and a deterministic writer that writes
//                  IHDR, IDAT and IEND only: no metadata, no timestamps
//   Zlib           a deterministic zlib writer (RFC 1950/1951: greedy LZ77, fixed Huffman codes): the
//                  same pixels give the same bytes with every .NET version on every machine
//   Srgb           the sRGB transfer function (IEC 61966-2-1) with a power function made of +, -, *, /
//                  only (DeterministicMath), so no C runtime's pow (whose results may depend on the
//                  CPU, see apps\lucent\src\main.cpp) can change a pixel
//   PreparedMaster validation and analysis of the master (noise floor, content box, interior alpha),
//                  and the renderer: area averages of premultiplied linear light
//   Ico            Windows icon files: a writer (32-bit BMP images below 256 px, PNG at 256 px) and a
//                  strict reader
//   ProgramIcons   the icon groups of an executable, read with LoadLibraryEx as a data file
//
// Determinism: every pixel is computed with IEEE double additions, multiplications and divisions in a
// fixed order, which x64 .NET performs identically on every machine; rounding to 8 bits uses fixed
// thresholds. Re-running the generator on the same master gives the same bytes.

using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;

namespace LucentBranding
{
    public sealed class BrandingException : Exception
    {
        public BrandingException(string message) : base(message) { }
    }

    // 8-bit RGBA pixels with straight (not premultiplied) alpha, rows top to bottom.
    public sealed class RgbaImage
    {
        private readonly int width;
        private readonly int height;
        private readonly byte[] pixels;

        public RgbaImage(int width, int height)
        {
            CheckSize(width, height);
            this.width = width;
            this.height = height;
            pixels = new byte[width * height * 4];
        }

        public RgbaImage(int width, int height, byte[] pixels)
        {
            CheckSize(width, height);
            if (pixels == null || pixels.Length != width * height * 4)
            {
                throw new BrandingException("the pixel buffer does not hold " + width + "x" + height + " RGBA pixels");
            }
            this.width = width;
            this.height = height;
            this.pixels = pixels;
        }

        private static void CheckSize(int width, int height)
        {
            if (width <= 0 || height <= 0 || width > Png.MaxDimension || height > Png.MaxDimension)
            {
                throw new BrandingException("an image of " + width + "x" + height + " pixels is not supported");
            }
        }

        public int Width { get { return width; } }
        public int Height { get { return height; } }
        public byte[] Pixels { get { return pixels; } }

        public int Alpha(int x, int y) { return pixels[(y * width + x) * 4 + 3]; }

        // How many pixels differ from `other` (same size) and the largest difference of one channel.
        public int CountDifferences(RgbaImage other, out int largestDifference)
        {
            if (other == null || other.width != width || other.height != height)
            {
                throw new BrandingException("images of different sizes cannot be compared");
            }
            int count = 0;
            largestDifference = 0;
            for (int i = 0; i < pixels.Length; i += 4)
            {
                bool differs = false;
                for (int c = 0; c < 4; c++)
                {
                    int delta = Math.Abs(pixels[i + c] - other.pixels[i + c]);
                    if (delta > 0)
                    {
                        differs = true;
                        if (delta > largestDifference) { largestDifference = delta; }
                    }
                }
                if (differs) { count++; }
            }
            return count;
        }
    }

    // CRC-32 (PNG chunks), Adler-32 (zlib) and big-endian integers. Public for the tests
    // (tests\branding\Test-Branding.ps1 crafts damaged and refused masters with valid checksums).
    public static class Checksums
    {
        private static readonly uint[] CrcTable = BuildCrcTable();

        private static uint[] BuildCrcTable()
        {
            uint[] table = new uint[256];
            for (uint n = 0; n < 256; n++)
            {
                uint c = n;
                for (int k = 0; k < 8; k++) { c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1; }
                table[n] = c;
            }
            return table;
        }

        public static uint Crc32(byte[] data, int offset, int count)
        {
            uint c = 0xFFFFFFFFu;
            for (int i = offset; i < offset + count; i++) { c = CrcTable[(c ^ data[i]) & 0xFF] ^ (c >> 8); }
            return c ^ 0xFFFFFFFFu;
        }

        public static uint Adler32(byte[] data, int offset, int count)
        {
            uint a = 1, b = 0;
            int i = offset;
            int end = offset + count;
            while (i < end)
            {
                int block = Math.Min(5552, end - i);
                for (int k = 0; k < block; k++) { a += data[i++]; b += a; }
                a %= 65521;
                b %= 65521;
            }
            return (b << 16) | a;
        }

        public static uint ReadBigEndian32(byte[] data, int offset)
        {
            return ((uint)data[offset] << 24) | ((uint)data[offset + 1] << 16) | ((uint)data[offset + 2] << 8) | data[offset + 3];
        }

        public static void WriteBigEndian32(Stream stream, uint value)
        {
            stream.WriteByte((byte)(value >> 24));
            stream.WriteByte((byte)(value >> 16));
            stream.WriteByte((byte)(value >> 8));
            stream.WriteByte((byte)value);
        }
    }

    // A zlib stream (RFC 1950) holding one fixed-Huffman deflate block (RFC 1951, section 3.2.6), with
    // greedy LZ77 matching over a 32 KiB window. Every choice is deterministic: the most recent of the
    // longest matches among at most MaxChain candidates.
    public static class Zlib
    {
        private const int WindowSize = 32768;
        private const int MinMatch = 3;
        private const int MaxMatch = 258;
        private const int MaxChain = 256;
        private const int HashBits = 16;

        private static readonly int[] LengthBase = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        private static readonly int[] LengthExtra = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        private static readonly int[] DistanceBase = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        private static readonly int[] DistanceExtra = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

        private sealed class BitWriter
        {
            private readonly MemoryStream stream;
            private ulong buffer;
            private int count;

            public BitWriter(MemoryStream stream) { this.stream = stream; }

            // Bits least significant first (deflate's order for header bits and extra bits).
            public void Write(uint bits, int length)
            {
                buffer |= (ulong)bits << count;
                count += length;
                while (count >= 8)
                {
                    stream.WriteByte((byte)buffer);
                    buffer >>= 8;
                    count -= 8;
                }
            }

            // A Huffman code, most significant bit first.
            public void WriteCode(uint code, int length)
            {
                uint reversed = 0;
                for (int i = 0; i < length; i++) { reversed = (reversed << 1) | ((code >> i) & 1); }
                Write(reversed, length);
            }

            public void Flush()
            {
                if (count > 0) { stream.WriteByte((byte)buffer); }
                buffer = 0;
                count = 0;
            }
        }

        private static void WriteSymbol(BitWriter writer, int symbol)
        {
            if (symbol <= 143) { writer.WriteCode((uint)(0x30 + symbol), 8); }
            else if (symbol <= 255) { writer.WriteCode((uint)(0x190 + symbol - 144), 9); }
            else if (symbol <= 279) { writer.WriteCode((uint)(symbol - 256), 7); }
            else { writer.WriteCode((uint)(0xC0 + symbol - 280), 8); }
        }

        private static void WriteMatch(BitWriter writer, int length, int distance)
        {
            int code = LengthBase.Length - 1;
            while (LengthBase[code] > length) { code--; }
            WriteSymbol(writer, 257 + code);
            if (LengthExtra[code] > 0) { writer.Write((uint)(length - LengthBase[code]), LengthExtra[code]); }
            int distanceCode = DistanceBase.Length - 1;
            while (DistanceBase[distanceCode] > distance) { distanceCode--; }
            writer.WriteCode((uint)distanceCode, 5);
            if (DistanceExtra[distanceCode] > 0) { writer.Write((uint)(distance - DistanceBase[distanceCode]), DistanceExtra[distanceCode]); }
        }

        private static int Hash(byte[] data, int position)
        {
            uint key = ((uint)data[position] << 16) | ((uint)data[position + 1] << 8) | data[position + 2];
            return (int)((key * 2654435761u) >> (32 - HashBits));
        }

        public static byte[] Compress(byte[] data)
        {
            MemoryStream output = new MemoryStream(data.Length / 4 + 64);
            output.WriteByte(0x78);
            output.WriteByte(0x01);
            BitWriter writer = new BitWriter(output);
            writer.Write(1, 1);  // BFINAL
            writer.Write(1, 2);  // BTYPE 01: fixed Huffman codes
            int[] head = new int[1 << HashBits];
            for (int i = 0; i < head.Length; i++) { head[i] = -1; }
            int[] previous = new int[WindowSize];
            int n = data.Length;
            int position = 0;
            while (position < n)
            {
                int bestLength = 0;
                int bestDistance = 0;
                if (position + MinMatch <= n)
                {
                    int limit = Math.Min(MaxMatch, n - position);
                    int candidate = head[Hash(data, position)];
                    int chain = 0;
                    while (candidate >= 0 && candidate < position && position - candidate <= WindowSize && chain < MaxChain)
                    {
                        if (data[candidate + bestLength] == data[position + bestLength])
                        {
                            int length = 0;
                            while (length < limit && data[candidate + length] == data[position + length]) { length++; }
                            if (length > bestLength)
                            {
                                bestLength = length;
                                bestDistance = position - candidate;
                                if (length == limit) { break; }
                            }
                        }
                        int next = previous[candidate & (WindowSize - 1)];
                        if (next >= candidate) { break; }
                        candidate = next;
                        chain++;
                    }
                }
                int advance;
                if (bestLength >= MinMatch)
                {
                    WriteMatch(writer, bestLength, bestDistance);
                    advance = bestLength;
                }
                else
                {
                    WriteSymbol(writer, data[position]);
                    advance = 1;
                }
                for (int k = 0; k < advance; k++, position++)
                {
                    if (position + MinMatch <= n)
                    {
                        int h = Hash(data, position);
                        previous[position & (WindowSize - 1)] = head[h];
                        head[h] = position;
                    }
                }
            }
            WriteSymbol(writer, 256);  // end of block
            writer.Flush();
            Checksums.WriteBigEndian32(output, Checksums.Adler32(data, 0, data.Length));
            return output.ToArray();
        }

        // Inflates a zlib stream that must hold exactly `expectedLength` bytes (.NET's DeflateStream, an
        // implementation independent of Compress) and checks its header and Adler-32.
        public static byte[] Decompress(byte[] data, int offset, int count, int expectedLength)
        {
            if (count < 6) { throw new BrandingException("the zlib stream is truncated"); }
            int cmf = data[offset];
            int flg = data[offset + 1];
            if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20) != 0)
            {
                throw new BrandingException("the zlib stream has an invalid header");
            }
            byte[] result = new byte[expectedLength];
            int total = 0;
            using (MemoryStream compressed = new MemoryStream(data, offset + 2, count - 6, false))
            using (DeflateStream inflater = new DeflateStream(compressed, CompressionMode.Decompress))
            {
                while (total < expectedLength)
                {
                    int read = inflater.Read(result, total, expectedLength - total);
                    if (read <= 0) { break; }
                    total += read;
                }
                if (total != expectedLength) { throw new BrandingException("the image data is " + total + " bytes, not " + expectedLength); }
                byte[] probe = new byte[1];
                if (inflater.Read(probe, 0, 1) > 0) { throw new BrandingException("the image data is longer than " + expectedLength + " bytes"); }
            }
            uint adler = Checksums.ReadBigEndian32(data, offset + count - 4);
            if (adler != Checksums.Adler32(result, 0, result.Length)) { throw new BrandingException("the image data fails its Adler-32 check"); }
            return result;
        }
    }

    public sealed class PngChunk
    {
        public string Type;
        public long Offset;  // of the chunk's length field
        public int Length;   // of its data
    }

    public sealed class PngFile
    {
        public RgbaImage Image;
        public List<PngChunk> Chunks = new List<PngChunk>();

        public bool HasChunk(string type)
        {
            foreach (PngChunk chunk in Chunks) { if (chunk.Type == type) { return true; } }
            return false;
        }

        public string ChunkList()
        {
            StringBuilder text = new StringBuilder();
            foreach (PngChunk chunk in Chunks)
            {
                if (text.Length > 0) { text.Append(", "); }
                text.Append(chunk.Type);
            }
            return text.ToString();
        }
    }

    public static class Png
    {
        public const int MaxDimension = 16384;
        private static readonly byte[] Signature = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };

        public static bool HasSignature(byte[] data, int offset, int count)
        {
            if (count < Signature.Length) { return false; }
            for (int i = 0; i < Signature.Length; i++) { if (data[offset + i] != Signature[i]) { return false; } }
            return true;
        }

        public static PngFile Read(byte[] data) { return Read(data, 0, data.Length); }

        // Strict: the signature, every chunk's CRC, IHDR first (8-bit RGBA, deflate, adaptive filtering,
        // no interlacing), consecutive IDAT chunks, IEND last and nothing after it, no unknown critical
        // chunk, image data of exactly the right length with a valid Adler-32 and valid filter types.
        public static PngFile Read(byte[] data, int offset, int count)
        {
            if (!HasSignature(data, offset, count)) { throw new BrandingException("not a PNG file (no PNG signature)"); }
            PngFile file = new PngFile();
            int end = offset + count;
            int position = offset + Signature.Length;
            int width = 0, height = 0;
            bool headerSeen = false, endSeen = false, imageDataEnded = false;
            MemoryStream imageData = new MemoryStream();
            while (position < end)
            {
                if (endSeen) { throw new BrandingException("data follows the IEND chunk"); }
                if (end - position < 12) { throw new BrandingException("a chunk is truncated"); }
                uint length = Checksums.ReadBigEndian32(data, position);
                if (length > (uint)(end - position - 12)) { throw new BrandingException("a chunk is longer than the file"); }
                for (int i = 4; i < 8; i++)
                {
                    byte c = data[position + i];
                    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) { throw new BrandingException("a chunk type is not four letters"); }
                }
                string type = Encoding.ASCII.GetString(data, position + 4, 4);
                uint crc = Checksums.ReadBigEndian32(data, position + 8 + (int)length);
                if (crc != Checksums.Crc32(data, position + 4, 4 + (int)length)) { throw new BrandingException("chunk " + type + " fails its CRC check"); }
                PngChunk chunk = new PngChunk();
                chunk.Type = type;
                chunk.Offset = position - offset;
                chunk.Length = (int)length;
                file.Chunks.Add(chunk);
                int body = position + 8;
                if (!headerSeen && type != "IHDR") { throw new BrandingException("the first chunk is " + type + ", not IHDR"); }
                if (type != "IDAT" && imageData.Length > 0) { imageDataEnded = true; }
                switch (type)
                {
                    case "IHDR":
                        if (headerSeen) { throw new BrandingException("IHDR appears twice"); }
                        if (length != 13) { throw new BrandingException("IHDR is not 13 bytes"); }
                        headerSeen = true;
                        uint w = Checksums.ReadBigEndian32(data, body);
                        uint h = Checksums.ReadBigEndian32(data, body + 4);
                        if (w == 0 || h == 0 || w > MaxDimension || h > MaxDimension) { throw new BrandingException("the image size " + w + "x" + h + " is not supported"); }
                        width = (int)w;
                        height = (int)h;
                        if (data[body + 8] != 8 || data[body + 9] != 6)
                        {
                            throw new BrandingException("the image is not 8-bit RGBA (bit depth " + data[body + 8] + ", colour type " + data[body + 9] + ")");
                        }
                        if (data[body + 10] != 0 || data[body + 11] != 0) { throw new BrandingException("unknown compression or filter method"); }
                        if (data[body + 12] != 0) { throw new BrandingException("the image is interlaced"); }
                        break;
                    case "IDAT":
                        if (imageDataEnded) { throw new BrandingException("the IDAT chunks are not consecutive"); }
                        imageData.Write(data, body, (int)length);
                        break;
                    case "IEND":
                        if (length != 0) { throw new BrandingException("IEND carries data"); }
                        endSeen = true;
                        break;
                    case "PLTE":
                        throw new BrandingException("an RGBA image has a palette (PLTE)");
                    default:
                        if (type[0] >= 'A' && type[0] <= 'Z') { throw new BrandingException("unknown critical chunk " + type); }
                        break;
                }
                position = body + (int)length + 4;
            }
            if (!endSeen) { throw new BrandingException("the IEND chunk is missing"); }
            if (imageData.Length == 0) { throw new BrandingException("the image has no IDAT data"); }
            int stride = width * 4;
            long expected = (long)height * (stride + 1);
            if (expected > int.MaxValue) { throw new BrandingException("the image is too large"); }
            byte[] compressed = imageData.ToArray();
            byte[] raw = Zlib.Decompress(compressed, 0, compressed.Length, (int)expected);
            byte[] pixels = new byte[height * stride];
            for (int y = 0; y < height; y++)
            {
                int filter = raw[y * (stride + 1)];
                int source = y * (stride + 1) + 1;
                int row = y * stride;
                int above = row - stride;
                for (int x = 0; x < stride; x++)
                {
                    int a = x >= 4 ? pixels[row + x - 4] : 0;
                    int b = y > 0 ? pixels[above + x] : 0;
                    int c = (x >= 4 && y > 0) ? pixels[above + x - 4] : 0;
                    int predictor;
                    switch (filter)
                    {
                        case 0: predictor = 0; break;
                        case 1: predictor = a; break;
                        case 2: predictor = b; break;
                        case 3: predictor = (a + b) >> 1; break;
                        case 4: predictor = Paeth(a, b, c); break;
                        default: throw new BrandingException("row " + y + " has the unknown filter type " + filter);
                    }
                    pixels[row + x] = (byte)(raw[source + x] + predictor);
                }
            }
            file.Image = new RgbaImage(width, height, pixels);
            return file;
        }

        private static int Paeth(int a, int b, int c)
        {
            int p = a + b - c;
            int pa = Math.Abs(p - a), pb = Math.Abs(p - b), pc = Math.Abs(p - c);
            if (pa <= pb && pa <= pc) { return a; }
            return pb <= pc ? b : c;
        }

        // IHDR (8-bit RGBA, not interlaced), one IDAT, IEND. Each row takes the filter whose output has
        // the smallest sum of absolute values (the lowest filter type on a tie).
        public static byte[] Write(RgbaImage image)
        {
            int width = image.Width, height = image.Height, stride = width * 4;
            byte[] pixels = image.Pixels;
            byte[] filtered = new byte[height * (stride + 1)];
            byte[] candidate = new byte[stride];
            byte[] best = new byte[stride];
            for (int y = 0; y < height; y++)
            {
                int row = y * stride;
                int above = row - stride;
                long bestScore = long.MaxValue;
                int bestFilter = 0;
                for (int filter = 0; filter <= 4; filter++)
                {
                    long score = 0;
                    for (int x = 0; x < stride; x++)
                    {
                        int a = x >= 4 ? pixels[row + x - 4] : 0;
                        int b = y > 0 ? pixels[above + x] : 0;
                        int c = (x >= 4 && y > 0) ? pixels[above + x - 4] : 0;
                        int predictor = filter == 0 ? 0 : filter == 1 ? a : filter == 2 ? b : filter == 3 ? (a + b) >> 1 : Paeth(a, b, c);
                        byte value = (byte)(pixels[row + x] - predictor);
                        candidate[x] = value;
                        score += value < 128 ? value : 256 - value;
                    }
                    if (score < bestScore)
                    {
                        bestScore = score;
                        bestFilter = filter;
                        Buffer.BlockCopy(candidate, 0, best, 0, stride);
                    }
                }
                filtered[y * (stride + 1)] = (byte)bestFilter;
                Buffer.BlockCopy(best, 0, filtered, y * (stride + 1) + 1, stride);
            }
            MemoryStream output = new MemoryStream();
            output.Write(Signature, 0, Signature.Length);
            byte[] header = new byte[13];
            header[0] = (byte)(width >> 24); header[1] = (byte)(width >> 16); header[2] = (byte)(width >> 8); header[3] = (byte)width;
            header[4] = (byte)(height >> 24); header[5] = (byte)(height >> 16); header[6] = (byte)(height >> 8); header[7] = (byte)height;
            header[8] = 8;   // bit depth
            header[9] = 6;   // RGBA
            WriteChunk(output, "IHDR", header);
            WriteChunk(output, "IDAT", Zlib.Compress(filtered));
            WriteChunk(output, "IEND", new byte[0]);
            return output.ToArray();
        }

        // A copy of a PNG file with one more chunk right after IHDR, its CRC computed: how the tests make
        // a master with a colour profile or with metadata.
        public static byte[] InsertChunk(byte[] png, string type, byte[] body)
        {
            if (type == null || type.Length != 4) { throw new BrandingException("a chunk type has four letters"); }
            PngFile parsed = Read(png);
            PngChunk header = parsed.Chunks[0];
            int insertAt = (int)header.Offset + 12 + header.Length;
            MemoryStream output = new MemoryStream();
            output.Write(png, 0, insertAt);
            WriteChunk(output, type, body);
            output.Write(png, insertAt, png.Length - insertAt);
            return output.ToArray();
        }

        private static void WriteChunk(MemoryStream output, string type, byte[] body)
        {
            byte[] typed = new byte[4 + body.Length];
            Encoding.ASCII.GetBytes(type, 0, 4, typed, 0);
            Buffer.BlockCopy(body, 0, typed, 4, body.Length);
            Checksums.WriteBigEndian32(output, (uint)body.Length);
            output.Write(typed, 0, typed.Length);
            Checksums.WriteBigEndian32(output, Checksums.Crc32(typed, 0, typed.Length));
        }
    }

    // log, exp and pow from IEEE additions, multiplications and divisions only, with a fixed number of
    // series terms: the same bits on every machine. Accurate to a few units in the last place.
    public static class DeterministicMath
    {
        private const double Ln2 = 0.69314718055994530942;
        private const double Sqrt2 = 1.4142135623730950488;

        public static double Log(double x)
        {
            if (!(x > 0) || double.IsInfinity(x)) { throw new BrandingException("Log needs a finite positive number"); }
            long bits = BitConverter.DoubleToInt64Bits(x);
            int exponent = (int)((bits >> 52) & 0x7FF);
            if (exponent == 0) { throw new BrandingException("Log does not take subnormal numbers"); }
            exponent -= 1023;
            double mantissa = BitConverter.Int64BitsToDouble((bits & 0x000FFFFFFFFFFFFFL) | 0x3FF0000000000000L);
            if (mantissa > Sqrt2)
            {
                mantissa *= 0.5;
                exponent++;
            }
            // ln(m) = 2 atanh(z), z = (m - 1) / (m + 1), |z| <= 0.1716
            double z = (mantissa - 1.0) / (mantissa + 1.0);
            double z2 = z * z;
            double term = z;
            double sum = 0.0;
            for (int k = 0; k < 24; k++)
            {
                sum += term / (2 * k + 1);
                term *= z2;
            }
            return exponent * Ln2 + 2.0 * sum;
        }

        public static double Exp(double t)
        {
            if (double.IsNaN(t) || t > 700 || t < -700) { throw new BrandingException("Exp is limited to |t| <= 700"); }
            double n = Math.Round(t / Ln2);
            double r = t - n * Ln2;  // |r| <= 0.35
            double sum = 1.0;
            double term = 1.0;
            for (int k = 1; k < 28; k++)
            {
                term *= r / k;
                sum += term;
            }
            int exponent = (int)n;
            double scale = BitConverter.Int64BitsToDouble((long)(exponent + 1023) << 52);
            return sum * scale;
        }

        public static double Pow(double x, double y)
        {
            if (x == 1.0) { return 1.0; }
            return Exp(y * Log(x));
        }
    }

    // The sRGB transfer function (IEC 61966-2-1). Decoding uses a table of the 256 code values; encoding
    // rounds in the encoded domain through the 255 midpoints between code values, decoded.
    public static class Srgb
    {
        private static readonly double[] Decoded = new double[256];
        private static readonly double[] Midpoints = new double[255];

        static Srgb()
        {
            for (int v = 0; v < 256; v++) { Decoded[v] = ToLinear(v / 255.0); }
            for (int k = 0; k < 255; k++) { Midpoints[k] = ToLinear((k + 0.5) / 255.0); }
        }

        public static double ToLinear(double encoded)
        {
            if (encoded <= 0.04045) { return encoded / 12.92; }
            return DeterministicMath.Pow((encoded + 0.055) / 1.055, 2.4);
        }

        public static double Decode(int value) { return Decoded[value]; }

        // The 8-bit code value nearest to `linear` in the encoded domain (a midpoint rounds up).
        public static byte Encode(double linear)
        {
            int low = 0, high = Midpoints.Length;  // the result is the number of midpoints <= linear
            while (low < high)
            {
                int middle = (low + high) >> 1;
                if (Midpoints[middle] <= linear) { low = middle + 1; } else { high = middle; }
            }
            return (byte)low;
        }
    }

    // What the analysis of a master found (recorded in branding.json).
    public sealed class MasterAnalysis
    {
        public int Width;
        public int Height;
        public int NoiseAlpha;        // alpha values 1 to NoiseAlpha were cleared as noise
        public int ClearedPixels;
        public int FaintComponents;   // groups of touching pixels without a pixel of alpha >= 128, cleared
        public int FaintPixels;
        public int ContentLeft;       // the content box: pixels with alpha >= 128, right and bottom exclusive
        public int ContentTop;
        public int ContentRight;
        public int ContentBottom;
        public int InteriorPixels;    // content pixels at least InteriorMargin pixels inside the silhouette
        public int SolidAlpha;        // the interior's alpha at its 1st percentile: what counts as opaque
        public long[] InteriorAlphaHistogram;
        public double ContentCentreX { get { return (ContentLeft + ContentRight) / 2.0; } }
        public double ContentCentreY { get { return (ContentTop + ContentBottom) / 2.0; } }
        public int ContentWidth { get { return ContentRight - ContentLeft; } }
        public int ContentHeight { get { return ContentBottom - ContentTop; } }
    }

    // The master as premultiplied linear light, ready to render. Prepare() applies the alpha rule:
    //   1. alpha 1 .. noiseAlpha is cleared (the image generator's faint specks);
    //   2. every group of touching (4-connected) non-transparent pixels without a single pixel of alpha
    //      >= 128 is cleared: the generator's faint specks and the ringing it leaves a few pixels outside
    //      an edge, separated from the artwork by transparent pixels. Each part of the artwork itself
    //      has a solid core, so a logo of several separate parts keeps them all;
    //   3. the content box is the bounding box of alpha >= 128 (the visible silhouette); it must not touch
    //      the master's edge;
    //   4. the solid interior (content pixels whose 7x7 neighbourhood is all content) gives SolidAlpha, its
    //      1st-percentile alpha; it must be at least minimumSolidAlpha (a translucent master needs a new
    //      rule, not this one);
    //   5. alpha becomes coverage min(1, alpha / SolidAlpha): the nearly opaque interior is opaque and the
    //      anti-aliased edges keep their ramp.
    public sealed class PreparedMaster
    {
        public const int InteriorMargin = 3;
        private readonly int width;
        private readonly int height;
        private readonly double[] red, green, blue, alpha;  // premultiplied linear RGB, coverage
        private readonly MasterAnalysis analysis;

        private PreparedMaster(int width, int height, MasterAnalysis analysis)
        {
            this.width = width;
            this.height = height;
            this.analysis = analysis;
            red = new double[width * height];
            green = new double[width * height];
            blue = new double[width * height];
            alpha = new double[width * height];
        }

        public MasterAnalysis Analysis { get { return analysis; } }

        public static PreparedMaster Prepare(RgbaImage master, int noiseAlpha, int minimumSolidAlpha)
        {
            int width = master.Width, height = master.Height;
            byte[] source = master.Pixels;
            MasterAnalysis analysis = new MasterAnalysis();
            analysis.Width = width;
            analysis.Height = height;
            analysis.NoiseAlpha = noiseAlpha;
            int[] a = new int[width * height];
            for (int i = 0; i < width * height; i++)
            {
                int value = source[i * 4 + 3];
                if (value > 0 && value <= noiseAlpha)
                {
                    value = 0;
                    analysis.ClearedPixels++;
                }
                a[i] = value;
            }
            ClearFaintComponents(a, width, height, analysis);
            int left = width, top = height, right = -1, bottom = -1;
            for (int y = 0; y < height; y++)
            {
                for (int x = 0; x < width; x++)
                {
                    if (a[y * width + x] >= 128)
                    {
                        if (x < left) { left = x; }
                        if (x > right) { right = x; }
                        if (y < top) { top = y; }
                        if (y > bottom) { bottom = y; }
                    }
                }
            }
            if (right < 0) { throw new BrandingException("the master has no artwork (no pixel with alpha >= 128)"); }
            analysis.ContentLeft = left;
            analysis.ContentTop = top;
            analysis.ContentRight = right + 1;
            analysis.ContentBottom = bottom + 1;
            if (left < 1 || top < 1 || right + 1 > width - 1 || bottom + 1 > height - 1)
            {
                throw new BrandingException("the artwork touches the edge of the master, so it may have been cut off");
            }

            // The interior: content pixels whose (2m+1)^2 neighbourhood is all content (two passes of
            // window counts).
            int m = InteriorMargin, window = 2 * m + 1;
            int[] across = new int[width * height];
            for (int y = 0; y < height; y++)
            {
                int run = 0;
                for (int x = 0; x < width + m; x++)
                {
                    if (x < width && a[y * width + x] >= 128) { run++; }
                    if (x - window >= 0 && a[y * width + x - window] >= 128) { run--; }
                    int centre = x - m;
                    if (centre >= 0 && centre < width) { across[y * width + centre] = run; }
                }
            }
            long[] histogram = new long[256];
            for (int x = 0; x < width; x++)
            {
                int run = 0;
                for (int y = 0; y < height + m; y++)
                {
                    if (y < height && across[y * width + x] == window) { run++; }
                    if (y - window >= 0 && across[(y - window) * width + x] == window) { run--; }
                    int centre = y - m;
                    if (centre >= 0 && centre < height && run == window && x >= m && x < width - m && centre >= m && centre < height - m)
                    {
                        histogram[a[centre * width + x]]++;
                        analysis.InteriorPixels++;
                    }
                }
            }
            if (analysis.InteriorPixels == 0) { throw new BrandingException("the master's artwork has no solid interior"); }
            long cumulative = 0;
            int solid = 255;
            for (int v = 0; v < 256; v++)
            {
                cumulative += histogram[v];
                if (cumulative * 100 > analysis.InteriorPixels) { solid = v; break; }
            }
            analysis.SolidAlpha = solid;
            analysis.InteriorAlphaHistogram = histogram;
            if (solid < minimumSolidAlpha)
            {
                throw new BrandingException("the master's artwork is translucent (interior alpha " + solid + " at its 1st percentile, below " +
                                            minimumSolidAlpha + "); the alpha rule of ADR 0025 does not cover it");
            }

            PreparedMaster prepared = new PreparedMaster(width, height, analysis);
            for (int i = 0; i < width * height; i++)
            {
                if (a[i] == 0) { continue; }
                double coverage = a[i] >= solid ? 1.0 : a[i] / (double)solid;
                prepared.alpha[i] = coverage;
                prepared.red[i] = Srgb.Decode(source[i * 4]) * coverage;
                prepared.green[i] = Srgb.Decode(source[i * 4 + 1]) * coverage;
                prepared.blue[i] = Srgb.Decode(source[i * 4 + 2]) * coverage;
            }
            return prepared;
        }

        // Step 2 of the alpha rule: flood-fills each 4-connected group of pixels with alpha > 0 (scanning
        // rows top to bottom) and clears the groups whose largest alpha is below 128.
        private static void ClearFaintComponents(int[] a, int width, int height, MasterAnalysis analysis)
        {
            bool[] visited = new bool[width * height];
            int[] stack = new int[width * height];
            List<int> members = new List<int>();
            for (int start = 0; start < width * height; start++)
            {
                if (a[start] == 0 || visited[start]) { continue; }
                members.Clear();
                int top = 0;
                stack[top++] = start;
                visited[start] = true;
                int largest = 0;
                while (top > 0)
                {
                    int index = stack[--top];
                    members.Add(index);
                    if (a[index] > largest) { largest = a[index]; }
                    int x = index % width;
                    int y = index / width;
                    if (x > 0 && a[index - 1] > 0 && !visited[index - 1]) { visited[index - 1] = true; stack[top++] = index - 1; }
                    if (x < width - 1 && a[index + 1] > 0 && !visited[index + 1]) { visited[index + 1] = true; stack[top++] = index + 1; }
                    if (y > 0 && a[index - width] > 0 && !visited[index - width]) { visited[index - width] = true; stack[top++] = index - width; }
                    if (y < height - 1 && a[index + width] > 0 && !visited[index + width]) { visited[index + width] = true; stack[top++] = index + width; }
                }
                if (largest >= 128) { continue; }
                analysis.FaintComponents++;
                analysis.FaintPixels += members.Count;
                foreach (int index in members) { a[index] = 0; }
            }
        }

        private struct Tap
        {
            public int Index;
            public double Weight;
        }

        // The master pixels under each output pixel of one axis and how much of the output pixel's
        // footprint each covers: output pixel i covers [i, i + 1), which is master
        // [centre + (i - outputCentre) / scale, ...) of length 1 / scale. Master pixels outside the image
        // are transparent: they keep their share of the footprint and contribute nothing.
        private static Tap[][] Footprints(int outputLength, double outputCentre, double masterCentre, double scale, int masterLength)
        {
            Tap[][] taps = new Tap[outputLength][];
            double length = 1.0 / scale;
            for (int i = 0; i < outputLength; i++)
            {
                double start = masterCentre + (i - outputCentre) / scale;
                double end = masterCentre + (i + 1 - outputCentre) / scale;
                int first = (int)Math.Floor(start);
                int last = (int)Math.Ceiling(end) - 1;
                List<Tap> list = new List<Tap>();
                for (int k = first; k <= last; k++)
                {
                    if (k < 0 || k >= masterLength) { continue; }
                    double overlap = Math.Min(k + 1, end) - Math.Max(k, start);
                    if (overlap <= 0) { continue; }
                    Tap tap;
                    tap.Index = k;
                    tap.Weight = overlap / length;
                    list.Add(tap);
                }
                taps[i] = list.ToArray();
            }
            return taps;
        }

        // Renders the artwork onto a transparent canvas of canvasWidth x canvasHeight pixels: the content
        // box's longer side spans targetSize pixels, and its centre sits at the centre of the square
        // [targetLeft, targetLeft + targetSize) x [targetTop, targetTop + targetSize). Each output pixel is
        // the area average of the master under it (box filter: exact footprint coverage) in premultiplied
        // linear light; its alpha is the covered fraction, its colour the average colour of what covers it.
        public RgbaImage Render(int canvasWidth, int canvasHeight, double targetLeft, double targetTop, double targetSize)
        {
            if (!(targetSize > 0)) { throw new BrandingException("the target size must be positive"); }
            double longest = Math.Max(analysis.ContentWidth, analysis.ContentHeight);
            double scale = targetSize / longest;
            if (scale > 1.0) { throw new BrandingException("the master is too small for a " + targetSize + " px rendering (it would be enlarged)"); }
            Tap[][] columns = Footprints(canvasWidth, targetLeft + targetSize / 2.0, analysis.ContentCentreX, scale, width);
            Tap[][] rows = Footprints(canvasHeight, targetTop + targetSize / 2.0, analysis.ContentCentreY, scale, height);
            int firstRow = height, lastRow = -1;
            foreach (Tap[] taps in rows)
            {
                foreach (Tap tap in taps)
                {
                    if (tap.Index < firstRow) { firstRow = tap.Index; }
                    if (tap.Index > lastRow) { lastRow = tap.Index; }
                }
            }
            RgbaImage result = new RgbaImage(canvasWidth, canvasHeight);
            if (lastRow < 0) { return result; }
            int rowCount = lastRow - firstRow + 1;
            double[] r = new double[rowCount * canvasWidth];
            double[] g = new double[rowCount * canvasWidth];
            double[] b = new double[rowCount * canvasWidth];
            double[] a = new double[rowCount * canvasWidth];
            for (int y = firstRow; y <= lastRow; y++)
            {
                int line = y * width;
                int target = (y - firstRow) * canvasWidth;
                for (int x = 0; x < canvasWidth; x++)
                {
                    double sr = 0, sg = 0, sb = 0, sa = 0;
                    foreach (Tap tap in columns[x])
                    {
                        int index = line + tap.Index;
                        sr += red[index] * tap.Weight;
                        sg += green[index] * tap.Weight;
                        sb += blue[index] * tap.Weight;
                        sa += alpha[index] * tap.Weight;
                    }
                    r[target + x] = sr;
                    g[target + x] = sg;
                    b[target + x] = sb;
                    a[target + x] = sa;
                }
            }
            byte[] output = result.Pixels;
            for (int j = 0; j < canvasHeight; j++)
            {
                for (int x = 0; x < canvasWidth; x++)
                {
                    double sr = 0, sg = 0, sb = 0, sa = 0;
                    foreach (Tap tap in rows[j])
                    {
                        int index = (tap.Index - firstRow) * canvasWidth + x;
                        sr += r[index] * tap.Weight;
                        sg += g[index] * tap.Weight;
                        sb += b[index] * tap.Weight;
                        sa += a[index] * tap.Weight;
                    }
                    if (!(sa > 0)) { continue; }
                    int alpha8 = (int)Math.Floor(sa * 255.0 + 0.5);
                    if (alpha8 <= 0) { continue; }
                    if (alpha8 > 255) { alpha8 = 255; }
                    int o = (j * canvasWidth + x) * 4;
                    output[o] = Srgb.Encode(sr / sa);
                    output[o + 1] = Srgb.Encode(sg / sa);
                    output[o + 2] = Srgb.Encode(sb / sa);
                    output[o + 3] = (byte)alpha8;
                }
            }
            return result;
        }
    }

    // One image of an icon file or icon group.
    public sealed class IconImage
    {
        public int Width;          // pixels (a 0 in the directory is 256)
        public int Height;
        public int ColorCount;
        public int Planes;
        public int BitCount;
        public int ResourceId;     // RT_ICON id (icon groups of executables only)
        public bool IsPng;
        public byte[] Data;        // the image's bytes as stored
        public RgbaImage Image;    // decoded (null when DecodeError says why not)
        public string DecodeError; // icon groups of executables only: an image this code does not read
                                   // (another program's 8-bit icon, for example); never for Ico.Read
    }

    // Windows icon files (.ico). The writer stores images below pngMinimumSize as 32-bit BMPs
    // (BITMAPINFOHEADER, BI_RGB, straight alpha, bottom-up rows, then the AND mask with a set bit where
    // alpha is 0): the form RC.exe, Explorer, LoadImage and Inno Setup all read. Larger images are PNG.
    public static class Ico
    {
        public static byte[] Write(IList<RgbaImage> images, int pngMinimumSize)
        {
            if (images.Count == 0 || images.Count > 255) { throw new BrandingException("an icon file holds 1 to 255 images"); }
            List<byte[]> encoded = new List<byte[]>();
            foreach (RgbaImage image in images)
            {
                if (image.Width != image.Height || image.Width > 256) { throw new BrandingException("icon images are square, at most 256 px"); }
                encoded.Add(image.Width >= pngMinimumSize ? Png.Write(image) : EncodeBmp(image));
            }
            MemoryStream output = new MemoryStream();
            BinaryWriter writer = new BinaryWriter(output);
            writer.Write((ushort)0);
            writer.Write((ushort)1);
            writer.Write((ushort)images.Count);
            int offset = 6 + 16 * images.Count;
            for (int i = 0; i < images.Count; i++)
            {
                int size = images[i].Width;
                writer.Write((byte)(size == 256 ? 0 : size));
                writer.Write((byte)(size == 256 ? 0 : size));
                writer.Write((byte)0);       // colour count
                writer.Write((byte)0);       // reserved
                writer.Write((ushort)1);     // planes
                writer.Write((ushort)32);    // bits per pixel
                writer.Write((uint)encoded[i].Length);
                writer.Write((uint)offset);
                offset += encoded[i].Length;
            }
            foreach (byte[] bytes in encoded) { writer.Write(bytes); }
            writer.Flush();
            return output.ToArray();
        }

        public static int MaskStride(int width) { return ((width + 31) / 32) * 4; }

        public static byte[] EncodeBmp(RgbaImage image)
        {
            int size = image.Width;
            int maskStride = MaskStride(size);
            int colourBytes = size * size * 4;
            byte[] result = new byte[40 + colourBytes + maskStride * size];
            WriteLittleEndian32(result, 0, 40);                      // biSize
            WriteLittleEndian32(result, 4, size);                    // biWidth
            WriteLittleEndian32(result, 8, size * 2);                // biHeight: colour and mask
            result[12] = 1;                                          // biPlanes
            result[14] = 32;                                         // biBitCount
            WriteLittleEndian32(result, 20, colourBytes + maskStride * size);  // biSizeImage
            byte[] pixels = image.Pixels;
            for (int y = 0; y < size; y++)
            {
                int source = (size - 1 - y) * size * 4;   // bottom-up
                int target = 40 + y * size * 4;
                int mask = 40 + colourBytes + y * maskStride;
                for (int x = 0; x < size; x++)
                {
                    int s = source + x * 4;
                    int t = target + x * 4;
                    result[t] = pixels[s + 2];
                    result[t + 1] = pixels[s + 1];
                    result[t + 2] = pixels[s];
                    result[t + 3] = pixels[s + 3];
                    if (pixels[s + 3] == 0) { result[mask + x / 8] |= (byte)(0x80 >> (x % 8)); }
                }
            }
            return result;
        }

        // A 32-bit BMP icon image of exactly this form (the writer's); its AND mask must agree with its
        // alpha (bits set exactly where alpha is 0).
        public static RgbaImage DecodeBmp(byte[] data, int size)
        {
            int maskStride = MaskStride(size);
            int colourBytes = size * size * 4;
            if (data.Length != 40 + colourBytes + maskStride * size) { throw new BrandingException("the " + size + " px BMP image is " + data.Length + " bytes, not " + (40 + colourBytes + maskStride * size)); }
            if (ReadLittleEndian32(data, 0) != 40) { throw new BrandingException("the " + size + " px image does not start with a BITMAPINFOHEADER"); }
            if (ReadLittleEndian32(data, 4) != size || ReadLittleEndian32(data, 8) != size * 2) { throw new BrandingException("the " + size + " px BMP header gives another size"); }
            if (data[12] != 1 || data[13] != 0 || data[14] != 32 || data[15] != 0) { throw new BrandingException("the " + size + " px BMP image is not one plane of 32 bits"); }
            if (ReadLittleEndian32(data, 16) != 0) { throw new BrandingException("the " + size + " px BMP image is compressed"); }
            byte[] pixels = new byte[colourBytes];
            for (int y = 0; y < size; y++)
            {
                int source = 40 + y * size * 4;
                int target = (size - 1 - y) * size * 4;
                int mask = 40 + colourBytes + y * maskStride;
                for (int x = 0; x < size; x++)
                {
                    int s = source + x * 4;
                    int t = target + x * 4;
                    pixels[t] = data[s + 2];
                    pixels[t + 1] = data[s + 1];
                    pixels[t + 2] = data[s];
                    pixels[t + 3] = data[s + 3];
                    bool transparent = (data[mask + x / 8] & (0x80 >> (x % 8))) != 0;
                    if (transparent != (data[s + 3] == 0)) { throw new BrandingException("the " + size + " px image's AND mask disagrees with its alpha at (" + x + ", " + (size - 1 - y) + ")"); }
                }
            }
            return new RgbaImage(size, size, pixels);
        }

        // Strict: header (reserved 0, type 1), every entry's data inside the file without overlaps, every
        // image a PNG or 32-bit BMP whose size is the directory's.
        public static List<IconImage> Read(byte[] data)
        {
            if (data.Length < 6 || ReadLittleEndian16(data, 0) != 0 || ReadLittleEndian16(data, 2) != 1) { throw new BrandingException("not an icon file (ICONDIR header)"); }
            int count = ReadLittleEndian16(data, 4);
            if (count == 0 || data.Length < 6 + 16 * count) { throw new BrandingException("the icon directory is empty or truncated"); }
            List<IconImage> images = new List<IconImage>();
            List<long[]> ranges = new List<long[]>();
            for (int i = 0; i < count; i++)
            {
                int entry = 6 + 16 * i;
                IconImage image = new IconImage();
                image.Width = data[entry] == 0 ? 256 : data[entry];
                image.Height = data[entry + 1] == 0 ? 256 : data[entry + 1];
                image.ColorCount = data[entry + 2];
                if (data[entry + 3] != 0) { throw new BrandingException("icon entry " + i + " has a reserved byte set"); }
                image.Planes = ReadLittleEndian16(data, entry + 4);
                image.BitCount = ReadLittleEndian16(data, entry + 6);
                long length = (uint)ReadLittleEndian32(data, entry + 8);
                long offset = (uint)ReadLittleEndian32(data, entry + 12);
                if (offset < 6 + 16 * count || offset + length > data.Length || length == 0) { throw new BrandingException("icon entry " + i + " points outside the file"); }
                foreach (long[] range in ranges)
                {
                    if (offset < range[1] && range[0] < offset + length) { throw new BrandingException("icon entry " + i + " overlaps another image"); }
                }
                ranges.Add(new long[] { offset, offset + length });
                image.Data = new byte[length];
                Buffer.BlockCopy(data, (int)offset, image.Data, 0, (int)length);
                DecodeImage(image, "icon entry " + i);
                images.Add(image);
            }
            return images;
        }

        // Decodes an icon image's bytes (PNG or 32-bit BMP) and checks that its size is the directory's.
        public static void DecodeImage(IconImage image, string what)
        {
            image.IsPng = Png.HasSignature(image.Data, 0, image.Data.Length);
            if (image.IsPng)
            {
                image.Image = Png.Read(image.Data).Image;
            }
            else
            {
                if (image.Width != image.Height) { throw new BrandingException(what + " is a BMP image that is not square"); }
                image.Image = DecodeBmp(image.Data, image.Width);
            }
            if (image.Image.Width != image.Width || image.Image.Height != image.Height)
            {
                throw new BrandingException(what + " is " + image.Image.Width + "x" + image.Image.Height + " px, but the directory says " + image.Width + "x" + image.Height);
            }
        }

        public static int ReadLittleEndian16(byte[] data, int offset) { return data[offset] | (data[offset + 1] << 8); }

        public static int ReadLittleEndian32(byte[] data, int offset)
        {
            return data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) | (data[offset + 3] << 24);
        }

        private static void WriteLittleEndian32(byte[] data, int offset, int value)
        {
            data[offset] = (byte)value;
            data[offset + 1] = (byte)(value >> 8);
            data[offset + 2] = (byte)(value >> 16);
            data[offset + 3] = (byte)(value >> 24);
        }
    }

    // An RT_GROUP_ICON resource of an executable with its RT_ICON images.
    public sealed class IconGroup
    {
        public string Name;      // "IDI_ICON1", or "#<id>" for a numeric name
        public List<IconImage> Images = new List<IconImage>();
        public byte[] Directory; // the group's GRPICONDIR bytes
    }

    // The icon groups of an executable or DLL, read as a data file (LoadLibraryEx with
    // LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE): nothing of it runs.
    public static class ProgramIcons
    {
        private const uint LoadLibraryAsDatafile = 0x00000002;
        private const uint LoadLibraryAsImageResource = 0x00000020;
        private static readonly IntPtr RtIcon = new IntPtr(3);
        private static readonly IntPtr RtGroupIcon = new IntPtr(14);

        private delegate bool EnumResourceNameCallback(IntPtr module, IntPtr type, IntPtr name, IntPtr parameter);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryExW(string fileName, IntPtr file, uint flags);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool FreeLibrary(IntPtr module);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool EnumResourceNamesW(IntPtr module, IntPtr type, EnumResourceNameCallback callback, IntPtr parameter);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr LoadResource(IntPtr module, IntPtr resource);

        [DllImport("kernel32.dll")]
        private static extern IntPtr LockResource(IntPtr data);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint SizeofResource(IntPtr module, IntPtr resource);

        private static byte[] ReadResource(IntPtr module, IntPtr name, IntPtr type)
        {
            IntPtr found = FindResourceW(module, name, type);
            if (found == IntPtr.Zero) { return null; }
            uint size = SizeofResource(module, found);
            IntPtr loaded = LoadResource(module, found);
            IntPtr locked = loaded == IntPtr.Zero ? IntPtr.Zero : LockResource(loaded);
            if (locked == IntPtr.Zero) { return null; }
            byte[] bytes = new byte[size];
            Marshal.Copy(locked, bytes, 0, (int)size);
            return bytes;
        }

        public static List<IconGroup> Read(string path)
        {
            IntPtr module = LoadLibraryExW(path, IntPtr.Zero, LoadLibraryAsDatafile | LoadLibraryAsImageResource);
            if (module == IntPtr.Zero) { throw new BrandingException("cannot open " + path + " as a data file (error " + Marshal.GetLastWin32Error() + ")"); }
            try
            {
                List<string> names = new List<string>();
                List<IntPtr> handles = new List<IntPtr>();
                List<IntPtr> pinned = new List<IntPtr>();
                EnumResourceNameCallback callback = delegate(IntPtr m, IntPtr type, IntPtr name, IntPtr parameter)
                {
                    long value = name.ToInt64();
                    if ((value >> 16) == 0)
                    {
                        names.Add("#" + value);
                        handles.Add(name);
                    }
                    else
                    {
                        string text = Marshal.PtrToStringUni(name);
                        names.Add(text);
                        IntPtr copy = Marshal.StringToHGlobalUni(text);
                        pinned.Add(copy);
                        handles.Add(copy);
                    }
                    return true;
                };
                List<IconGroup> groups = new List<IconGroup>();
                try
                {
                    // Fails with ERROR_RESOURCE_TYPE_NOT_FOUND when there is no icon group: no groups.
                    EnumResourceNamesW(module, RtGroupIcon, callback, IntPtr.Zero);
                    GC.KeepAlive(callback);
                    for (int g = 0; g < names.Count; g++)
                    {
                        byte[] directory = ReadResource(module, handles[g], RtGroupIcon);
                        if (directory == null || directory.Length < 6) { throw new BrandingException("icon group " + names[g] + " cannot be read"); }
                        if (Ico.ReadLittleEndian16(directory, 0) != 0 || Ico.ReadLittleEndian16(directory, 2) != 1) { throw new BrandingException("icon group " + names[g] + " has no GRPICONDIR header"); }
                        int count = Ico.ReadLittleEndian16(directory, 4);
                        if (directory.Length < 6 + 14 * count) { throw new BrandingException("icon group " + names[g] + " is truncated"); }
                        IconGroup group = new IconGroup();
                        group.Name = names[g];
                        group.Directory = directory;
                        for (int i = 0; i < count; i++)
                        {
                            int entry = 6 + 14 * i;
                            IconImage image = new IconImage();
                            image.Width = directory[entry] == 0 ? 256 : directory[entry];
                            image.Height = directory[entry + 1] == 0 ? 256 : directory[entry + 1];
                            image.ColorCount = directory[entry + 2];
                            image.Planes = Ico.ReadLittleEndian16(directory, entry + 4);
                            image.BitCount = Ico.ReadLittleEndian16(directory, entry + 6);
                            int length = Ico.ReadLittleEndian32(directory, entry + 8);
                            image.ResourceId = Ico.ReadLittleEndian16(directory, entry + 12);
                            image.Data = ReadResource(module, new IntPtr(image.ResourceId), RtIcon);
                            if (image.Data == null) { throw new BrandingException("icon " + image.ResourceId + " of group " + group.Name + " is missing"); }
                            if (image.Data.Length != length) { throw new BrandingException("icon " + image.ResourceId + " of group " + group.Name + " is " + image.Data.Length + " bytes, the group says " + length); }
                            // An image of another form (a program that is not Lucent's) is reported, not
                            // fatal: the comparison with the branding icon then names the difference.
                            try { Ico.DecodeImage(image, "icon " + image.ResourceId + " of group " + group.Name); }
                            catch (BrandingException e) { image.DecodeError = e.Message; }
                            group.Images.Add(image);
                        }
                        groups.Add(group);
                    }
                }
                finally
                {
                    foreach (IntPtr copy in pinned) { Marshal.FreeHGlobal(copy); }
                }
                return groups;
            }
            finally
            {
                FreeLibrary(module);
            }
        }
    }
}
