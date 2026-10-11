// Compressed content inside shipped files, decompressed for the packaging audit's builder-path scan
// (installer\scripts\LucentPackaging.psm1, Find-BuilderPathsInContent; W12 review SEC-07, ADR 0009
// amendment). Loaded by Add-Type in Windows PowerShell 5.1, so C# 5 only.
//
//   CompressedContent.FindDeflateStreams  every zlib (RFC 1950) and gzip (RFC 1952) stream in a byte
//                                         array that decodes completely and matches its checksum: Qt's
//                                         rcc resources (qCompress), PNG data, anything else.
//   CompressedContent.FindZstdFrames      zstd frames (RFC 8878 magic and a valid frame and first block
//                                         header): not decompressed, reported, so the audit fails closed.
//   InnoSetupContent.Extract              an Inno Setup 6 executable's compressed parts: the setup data
//                                         blocks (LZMA), the setup engine (LZMA) and the installed files
//                                         (LZMA2 or LZMA), from SetupLdr's offset table.
//
// The decoders are written from the format specifications (RFC 1951; the LZMA SDK's specification,
// lzma-specification.txt, public domain) and check everything they can: a stream that does not decode
// completely, or whose checksum differs, is not a stream. Sizes are bounded (DecodeLimitException).

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace LucentPackaging
{
    public sealed class DecodeLimitException : Exception
    {
        public DecodeLimitException(string message) : base(message) { }
    }

    // A decompressed part of a file: what it was, where its compressed bytes are, its content.
    public sealed class DecodedPart
    {
        public string Kind;            // "zlib", "gzip", "Inno Setup setup data", ...
        public long Offset;            // of the compressed data in the container
        public long CompressedLength;
        public byte[] Data;
    }

    // A growable output buffer that is also the decoders' dictionary.
    internal sealed class OutputBuffer
    {
        private byte[] data;
        private int length;
        private readonly long limit;

        public OutputBuffer(int capacity, long limit)
        {
            data = new byte[Math.Max(16, capacity)];
            this.limit = limit;
        }

        public int Length { get { return length; } }
        public byte this[int index] { get { return data[index]; } }

        private void Grow(int needed)
        {
            if ((long)length + needed > limit)
            {
                throw new DecodeLimitException("decompressed data exceeds " + limit + " bytes");
            }
            if (length + needed <= data.Length) { return; }
            long capacity = Math.Max((long)data.Length * 2, (long)length + needed);
            capacity = Math.Min(capacity, Math.Max(limit, (long)length + needed));
            capacity = Math.Min(capacity, 0x7FFFFFC7L);
            byte[] grown = new byte[(int)capacity];
            Buffer.BlockCopy(data, 0, grown, 0, length);
            data = grown;
        }

        public void Append(byte value)
        {
            if (length == data.Length || length >= limit) { Grow(1); }
            data[length++] = value;
        }

        public void Append(byte[] source, int offset, int count)
        {
            Grow(count);
            Buffer.BlockCopy(source, offset, data, length, count);
            length += count;
        }

        // Copies `count` bytes from `distance` bytes back (overlapping copies repeat the pattern).
        public void CopyMatch(int distance, int count)
        {
            Grow(count);
            int from = length - distance;
            for (int i = 0; i < count; i++) { data[length++] = data[from++]; }
        }

        public byte[] ToArray()
        {
            byte[] result = new byte[length];
            Buffer.BlockCopy(data, 0, result, 0, length);
            return result;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Deflate (RFC 1951)
    // ---------------------------------------------------------------------------------------------

    internal sealed class InflateException : Exception
    {
        public InflateException(string message) : base(message) { }
    }

    internal sealed class Inflater
    {
        private sealed class Huffman
        {
            public readonly short[] Count = new short[16];
            public readonly short[] Symbol;
            public Huffman(int symbols) { Symbol = new short[symbols]; }
        }

        private static readonly short[] LengthBase = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        private static readonly short[] LengthExtra = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        private static readonly short[] DistanceBase = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        private static readonly short[] DistanceExtra = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
        private static readonly byte[] CodeLengthOrder = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

        private static Huffman fixedLengths;
        private static Huffman fixedDistances;

        private readonly byte[] input;
        private int position;
        private readonly int end;
        private uint bitBuffer;
        private int bitCount;
        private readonly OutputBuffer output;

        private Inflater(byte[] input, int start, int end, long limit)
        {
            this.input = input;
            position = start;
            this.end = end;
            output = new OutputBuffer(256, limit);
        }

        // The byte position after the deflate data (whole bytes; the last byte's padding bits included).
        private int BytePosition { get { return position - (bitCount >> 3); } }

        private int Bits(int count)
        {
            while (bitCount < count)
            {
                if (position >= end) { throw new InflateException("end of input"); }
                bitBuffer |= (uint)input[position++] << bitCount;
                bitCount += 8;
            }
            int value = (int)(bitBuffer & ((1u << count) - 1));
            bitBuffer >>= count;
            bitCount -= count;
            return value;
        }

        private int Decode(Huffman h)
        {
            int code = 0, first = 0, index = 0;
            for (int length = 1; length <= 15; length++)
            {
                if (bitCount == 0)
                {
                    if (position >= end) { throw new InflateException("end of input"); }
                    bitBuffer = input[position++];
                    bitCount = 8;
                }
                code |= (int)(bitBuffer & 1);
                bitBuffer >>= 1;
                bitCount--;
                int count = h.Count[length];
                if (code - count < first) { return h.Symbol[index + (code - first)]; }
                index += count;
                first += count;
                first <<= 1;
                code <<= 1;
            }
            throw new InflateException("invalid code");
        }

        // Builds the decoding tables; returns 0 for a complete code, > 0 incomplete, < 0 over-subscribed.
        private static int Construct(Huffman h, short[] lengths, int offset, int n)
        {
            for (int len = 0; len < 16; len++) { h.Count[len] = 0; }
            for (int symbol = 0; symbol < n; symbol++) { h.Count[lengths[offset + symbol]]++; }
            if (h.Count[0] == n) { return 0; }
            int left = 1;
            for (int len = 1; len < 16; len++)
            {
                left <<= 1;
                left -= h.Count[len];
                if (left < 0) { return left; }
            }
            short[] offs = new short[16];
            for (int len = 1; len < 15; len++) { offs[len + 1] = (short)(offs[len] + h.Count[len]); }
            for (int symbol = 0; symbol < n; symbol++)
            {
                if (lengths[offset + symbol] != 0) { h.Symbol[offs[lengths[offset + symbol]]++] = (short)symbol; }
            }
            return left;
        }

        private void Stored()
        {
            int drop = bitCount & 7;
            bitBuffer >>= drop;
            bitCount -= drop;
            int length = Bits(16);
            int complement = Bits(16);
            if (length != (~complement & 0xFFFF)) { throw new InflateException("stored block length"); }
            // Whole bytes still in the bit buffer come first.
            while (length > 0 && bitCount >= 8)
            {
                output.Append((byte)Bits(8));
                length--;
            }
            if (length > end - position) { throw new InflateException("end of input"); }
            output.Append(input, position, length);
            position += length;
        }

        private void Codes(Huffman lengths, Huffman distances)
        {
            while (true)
            {
                int symbol = Decode(lengths);
                if (symbol < 256)
                {
                    output.Append((byte)symbol);
                    continue;
                }
                if (symbol == 256) { return; }
                symbol -= 257;
                if (symbol >= 29) { throw new InflateException("invalid length code"); }
                int length = LengthBase[symbol] + Bits(LengthExtra[symbol]);
                symbol = Decode(distances);
                if (symbol >= 30) { throw new InflateException("invalid distance code"); }
                int distance = DistanceBase[symbol] + Bits(DistanceExtra[symbol]);
                if (distance > output.Length) { throw new InflateException("distance too far back"); }
                output.CopyMatch(distance, length);
            }
        }

        private static void InitializeFixed()
        {
            if (fixedLengths != null) { return; }
            short[] lengths = new short[288 + 30];
            int symbol = 0;
            for (; symbol < 144; symbol++) { lengths[symbol] = 8; }
            for (; symbol < 256; symbol++) { lengths[symbol] = 9; }
            for (; symbol < 280; symbol++) { lengths[symbol] = 7; }
            for (; symbol < 288; symbol++) { lengths[symbol] = 8; }
            for (int d = 0; d < 30; d++) { lengths[288 + d] = 5; }
            Huffman fl = new Huffman(288);
            Huffman fd = new Huffman(30);
            Construct(fl, lengths, 0, 288);
            Construct(fd, lengths, 288, 30);
            fixedDistances = fd;
            fixedLengths = fl;
        }

        private void Dynamic()
        {
            int nlen = Bits(5) + 257;
            int ndist = Bits(5) + 1;
            int ncode = Bits(4) + 4;
            if (nlen > 286 || ndist > 30) { throw new InflateException("bad counts"); }
            short[] lengths = new short[320];
            for (int index = 0; index < ncode; index++) { lengths[CodeLengthOrder[index]] = (short)Bits(3); }
            Huffman lencode = new Huffman(286);
            Huffman distcode = new Huffman(30);
            if (Construct(lencode, lengths, 0, 19) != 0) { throw new InflateException("incomplete code-length code"); }
            int at = 0;
            while (at < nlen + ndist)
            {
                int symbol = Decode(lencode);
                if (symbol < 16)
                {
                    lengths[at++] = (short)symbol;
                    continue;
                }
                short repeated = 0;
                int count;
                if (symbol == 16)
                {
                    if (at == 0) { throw new InflateException("repeat without a length"); }
                    repeated = lengths[at - 1];
                    count = 3 + Bits(2);
                }
                else if (symbol == 17) { count = 3 + Bits(3); }
                else { count = 11 + Bits(7); }
                if (at + count > nlen + ndist) { throw new InflateException("too many lengths"); }
                while (count-- > 0) { lengths[at++] = repeated; }
            }
            if (lengths[256] == 0) { throw new InflateException("no end-of-block code"); }
            int err = Construct(lencode, lengths, 0, nlen);
            // An incomplete code only when it is a single code of length 1 (as zlib's and puff's).
            if (err < 0 || (err > 0 && nlen != lencode.Count[0] + lencode.Count[1])) { throw new InflateException("bad literal/length code"); }
            err = Construct(distcode, lengths, nlen, ndist);
            if (err < 0 || (err > 0 && ndist != distcode.Count[0] + distcode.Count[1])) { throw new InflateException("bad distance code"); }
            Codes(lencode, distcode);
        }

        // Decodes the deflate data at input[start, end). True with the output and the byte position after
        // the data; false when it is not complete, valid deflate data. DecodeLimitException beyond `limit`.
        public static bool TryInflate(byte[] input, int start, int end, long limit, out byte[] output, out int after)
        {
            output = null;
            after = start;
            InitializeFixed();
            Inflater state = new Inflater(input, start, end, limit);
            try
            {
                int last;
                do
                {
                    last = state.Bits(1);
                    int type = state.Bits(2);
                    if (type == 0) { state.Stored(); }
                    else if (type == 1) { state.Codes(fixedLengths, fixedDistances); }
                    else if (type == 2) { state.Dynamic(); }
                    else { return false; }
                } while (last == 0);
            }
            catch (InflateException)
            {
                return false;
            }
            catch (IndexOutOfRangeException)
            {
                return false;
            }
            output = state.output.ToArray();
            after = state.BytePosition;
            return true;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Checksums
    // ---------------------------------------------------------------------------------------------

    public static class Checksums
    {
        private static uint[] crcTable;

        public static uint Adler32(byte[] data)
        {
            uint a = 1, b = 0;
            int index = 0;
            int remaining = data.Length;
            while (remaining > 0)
            {
                int block = Math.Min(remaining, 5552);
                remaining -= block;
                while (block-- > 0)
                {
                    a += data[index++];
                    b += a;
                }
                a %= 65521;
                b %= 65521;
            }
            return (b << 16) | a;
        }

        public static uint Crc32(byte[] data, int offset, int count)
        {
            if (crcTable == null)
            {
                uint[] table = new uint[256];
                for (uint n = 0; n < 256; n++)
                {
                    uint c = n;
                    for (int k = 0; k < 8; k++) { c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1; }
                    table[n] = c;
                }
                crcTable = table;
            }
            uint crc = 0xFFFFFFFFu;
            for (int i = offset; i < offset + count; i++) { crc = crcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8); }
            return crc ^ 0xFFFFFFFFu;
        }

        public static uint Crc32(byte[] data) { return Crc32(data, 0, data.Length); }
    }

    // ---------------------------------------------------------------------------------------------
    // zlib, gzip and zstd in arbitrary bytes
    // ---------------------------------------------------------------------------------------------

    public static class CompressedContent
    {
        private static uint BigEndian32(byte[] data, int offset)
        {
            return ((uint)data[offset] << 24) | ((uint)data[offset + 1] << 16) | ((uint)data[offset + 2] << 8) | data[offset + 3];
        }

        private static uint LittleEndian32(byte[] data, int offset)
        {
            return data[offset] | ((uint)data[offset + 1] << 8) | ((uint)data[offset + 2] << 16) | ((uint)data[offset + 3] << 24);
        }

        // Every zlib and gzip stream of `data` that decodes completely and matches its checksum, in order
        // of position; a found stream's compressed bytes are not searched again. Each stream's output is
        // at most `streamLimit` bytes and all of them together at most `totalLimit`
        // (DecodeLimitException: the caller cannot scan what it cannot decompress).
        public static List<DecodedPart> FindDeflateStreams(byte[] data, long streamLimit, long totalLimit)
        {
            List<DecodedPart> parts = new List<DecodedPart>();
            long total = 0;
            int i = 0;
            while (i + 2 < data.Length)
            {
                byte cmf = data[i];
                byte flg = data[i + 1];
                DecodedPart part = null;
                if ((cmf & 0x0F) == 8 && (cmf >> 4) <= 7 && (flg & 0x20) == 0 && ((cmf << 8) | flg) % 31 == 0)
                {
                    part = TryZlib(data, i, streamLimit);
                }
                else if (cmf == 0x1F && flg == 0x8B && data[i + 2] == 8)
                {
                    part = TryGzip(data, i, streamLimit);
                }
                if (part == null)
                {
                    i++;
                    continue;
                }
                total += part.Data.Length;
                if (total > totalLimit)
                {
                    throw new DecodeLimitException("the compressed streams decompress to more than " + totalLimit + " bytes");
                }
                parts.Add(part);
                i = (int)(part.Offset + part.CompressedLength);
            }
            return parts;
        }

        private static DecodedPart TryZlib(byte[] data, int at, long limit)
        {
            byte[] output;
            int after;
            if (!Inflater.TryInflate(data, at + 2, data.Length, limit, out output, out after)) { return null; }
            if (after + 4 > data.Length || BigEndian32(data, after) != Checksums.Adler32(output)) { return null; }
            if (output.Length == 0) { return null; }
            DecodedPart part = new DecodedPart();
            part.Kind = "zlib";
            part.Offset = at;
            part.CompressedLength = after + 4 - at;
            part.Data = output;
            return part;
        }

        private static DecodedPart TryGzip(byte[] data, int at, long limit)
        {
            int p = at + 3;
            if (p >= data.Length) { return null; }
            byte flags = data[p];
            if ((flags & 0xE0) != 0) { return null; }
            p += 7;  // flags, MTIME, XFL, OS
            if (p > data.Length) { return null; }
            if ((flags & 4) != 0)
            {
                if (p + 2 > data.Length) { return null; }
                p += 2 + (data[p] | (data[p + 1] << 8));
            }
            if ((flags & 8) != 0)
            {
                while (p < data.Length && data[p] != 0) { p++; }
                p++;
            }
            if ((flags & 16) != 0)
            {
                while (p < data.Length && data[p] != 0) { p++; }
                p++;
            }
            if ((flags & 2) != 0) { p += 2; }
            if (p >= data.Length) { return null; }
            byte[] output;
            int after;
            if (!Inflater.TryInflate(data, p, data.Length, limit, out output, out after)) { return null; }
            if (after + 8 > data.Length) { return null; }
            if (LittleEndian32(data, after) != Checksums.Crc32(output) || LittleEndian32(data, after + 4) != (uint)output.Length) { return null; }
            if (output.Length == 0) { return null; }
            DecodedPart part = new DecodedPart();
            part.Kind = "gzip";
            part.Offset = at;
            part.CompressedLength = after + 8 - at;
            part.Data = output;
            return part;
        }

        // Offsets of zstd frames: the magic number 28 B5 2F FD, a frame header descriptor without its
        // reserved bit, and a first block header of a valid type and size (RFC 8878, 3.1.1).
        public static List<long> FindZstdFrames(byte[] data)
        {
            List<long> frames = new List<long>();
            for (int i = 0; i + 4 < data.Length; i++)
            {
                if (data[i] != 0x28 || data[i + 1] != 0xB5 || data[i + 2] != 0x2F || data[i + 3] != 0xFD) { continue; }
                int p = i + 4;
                byte descriptor = data[p++];
                if ((descriptor & 0x08) != 0) { continue; }  // reserved bit
                int fcsFlag = descriptor >> 6;
                bool singleSegment = (descriptor & 0x20) != 0;
                int dictionaryIdFlag = descriptor & 3;
                if (!singleSegment) { p++; }  // window descriptor
                p += dictionaryIdFlag == 3 ? 4 : dictionaryIdFlag;
                p += fcsFlag == 0 ? (singleSegment ? 1 : 0) : (1 << fcsFlag);
                if (p + 3 > data.Length) { continue; }
                int blockHeader = data[p] | (data[p + 1] << 8) | (data[p + 2] << 16);
                int blockType = (blockHeader >> 1) & 3;
                int blockSize = blockHeader >> 3;
                if (blockType == 3 || blockSize > 131072) { continue; }
                if (blockType != 1 && p + 3 + blockSize > data.Length) { continue; }
                frames.Add(i);
            }
            return frames;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // LZMA and LZMA2 (the LZMA SDK's specification)
    // ---------------------------------------------------------------------------------------------

    internal sealed class LzmaException : Exception
    {
        public LzmaException(string message) : base(message) { }
    }

    internal sealed class LzmaDecoder
    {
        private const int NumStates = 12;
        private const int PosBitsMax = 4;
        private const int NumLenToPosStates = 4;
        private const int EndPosModelIndex = 14;
        private const int NumFullDistances = 128;
        private const int NumAlignBits = 4;
        private const int MatchMinLen = 2;

        private readonly OutputBuffer output;
        private int dictionaryStart;
        private int lc, lp, pb;

        private ushort[] literalProbs = new ushort[0];
        private readonly ushort[] isMatch = new ushort[NumStates << PosBitsMax];
        private readonly ushort[] isRep = new ushort[NumStates];
        private readonly ushort[] isRepG0 = new ushort[NumStates];
        private readonly ushort[] isRepG1 = new ushort[NumStates];
        private readonly ushort[] isRepG2 = new ushort[NumStates];
        private readonly ushort[] isRep0Long = new ushort[NumStates << PosBitsMax];
        private readonly ushort[] posSlot = new ushort[NumLenToPosStates << 6];
        private readonly ushort[] posDecoders = new ushort[1 + NumFullDistances - EndPosModelIndex];
        private readonly ushort[] align = new ushort[1 << NumAlignBits];
        private readonly ushort[] lenProbs = new ushort[LenProbCount];
        private readonly ushort[] repLenProbs = new ushort[LenProbCount];

        // A length decoder: choice, choice 2, low[16][8], mid[16][8], high[256].
        private const int LenChoice = 0;
        private const int LenChoice2 = 1;
        private const int LenLow = 2;
        private const int LenMid = LenLow + (16 << 3);
        private const int LenHigh = LenMid + (16 << 3);
        private const int LenProbCount = LenHigh + 256;

        private int state;
        private uint rep0, rep1, rep2, rep3;

        // Range decoder.
        private byte[] input;
        private int inPos;
        private int inEnd;
        private uint range;
        private uint code;

        public LzmaDecoder(OutputBuffer output)
        {
            this.output = output;
        }

        public OutputBuffer Output { get { return output; } }

        public void SetProperties(int lcValue, int lpValue, int pbValue)
        {
            if (lcValue > 8 || lpValue > 4 || pbValue > 4) { throw new LzmaException("invalid properties"); }
            lc = lcValue;
            lp = lpValue;
            pb = pbValue;
            int needed = 0x300 << (lc + lp);
            if (literalProbs.Length != needed) { literalProbs = new ushort[needed]; }
        }

        public void SetProperties(byte properties)
        {
            if (properties >= 9 * 5 * 5) { throw new LzmaException("invalid properties"); }
            SetProperties(properties % 9, (properties / 9) % 5, properties / 45);
        }

        private static void Fill(ushort[] probs)
        {
            for (int i = 0; i < probs.Length; i++) { probs[i] = 1024; }
        }

        public void ResetState()
        {
            Fill(literalProbs); Fill(isMatch); Fill(isRep); Fill(isRepG0); Fill(isRepG1); Fill(isRepG2);
            Fill(isRep0Long); Fill(posSlot); Fill(posDecoders); Fill(align); Fill(lenProbs); Fill(repLenProbs);
            state = 0;
            rep0 = rep1 = rep2 = rep3 = 0;
        }

        public void ResetDictionary() { dictionaryStart = output.Length; }

        private byte NextByte()
        {
            if (inPos >= inEnd) { throw new LzmaException("end of input"); }
            return input[inPos++];
        }

        private void InitializeRange(byte[] source, int start, int end)
        {
            input = source;
            inPos = start;
            inEnd = end;
            if (NextByte() != 0) { throw new LzmaException("bad range coder start"); }
            code = 0;
            range = 0xFFFFFFFFu;
            for (int i = 0; i < 4; i++) { code = (code << 8) | NextByte(); }
            if (code == range) { throw new LzmaException("bad range coder start"); }
        }

        private int DecodeBit(ushort[] probs, int index)
        {
            uint p = probs[index];
            uint bound = (range >> 11) * p;
            int bit;
            if (code < bound)
            {
                range = bound;
                probs[index] = (ushort)(p + ((2048 - p) >> 5));
                bit = 0;
            }
            else
            {
                range -= bound;
                code -= bound;
                probs[index] = (ushort)(p - (p >> 5));
                bit = 1;
            }
            if (range < (1u << 24))
            {
                range <<= 8;
                code = (code << 8) | NextByte();
            }
            return bit;
        }

        private uint DecodeDirectBits(int count)
        {
            uint result = 0;
            do
            {
                range >>= 1;
                code -= range;
                uint t = 0u - (code >> 31);
                code += range & t;
                if (code == range) { throw new LzmaException("corrupted data"); }
                if (range < (1u << 24))
                {
                    range <<= 8;
                    code = (code << 8) | NextByte();
                }
                result = (result << 1) + (t + 1);
            } while (--count > 0);
            return result;
        }

        private int BitTree(ushort[] probs, int offset, int bits)
        {
            int m = 1;
            for (int i = 0; i < bits; i++) { m = (m << 1) + DecodeBit(probs, offset + m); }
            return m - (1 << bits);
        }

        private int ReverseBitTree(ushort[] probs, int offset, int bits)
        {
            int m = 1, symbol = 0;
            for (int i = 0; i < bits; i++)
            {
                int bit = DecodeBit(probs, offset + m);
                m = (m << 1) + bit;
                symbol |= bit << i;
            }
            return symbol;
        }

        private int DecodeLength(ushort[] probs, int posState)
        {
            if (DecodeBit(probs, LenChoice) == 0) { return BitTree(probs, LenLow + (posState << 3), 3); }
            if (DecodeBit(probs, LenChoice2) == 0) { return 8 + BitTree(probs, LenMid + (posState << 3), 3); }
            return 16 + BitTree(probs, LenHigh, 8);
        }

        private uint DecodeDistance(int length)
        {
            int lenState = Math.Min(length, NumLenToPosStates - 1);
            int slot = BitTree(posSlot, lenState << 6, 6);
            if (slot < 4) { return (uint)slot; }
            int directBits = (slot >> 1) - 1;
            uint distance = (uint)((2 | (slot & 1)) << directBits);
            if (slot < EndPosModelIndex)
            {
                distance += (uint)ReverseBitTree(posDecoders, (int)distance - slot, directBits);
            }
            else
            {
                distance += DecodeDirectBits(directBits - NumAlignBits) << NumAlignBits;
                distance += (uint)ReverseBitTree(align, 0, NumAlignBits);
            }
            return distance;
        }

        // Decodes the LZMA data at source[start, end): exactly `unpackSize` bytes, or (unpackSize < 0) up
        // to the end marker, which must then be present. Returns the input position after the data.
        public int Decode(byte[] source, int start, int end, long unpackSize)
        {
            InitializeRange(source, start, end);
            long produced = 0;
            int pbMask = (1 << pb) - 1;
            int lpMask = (1 << lp) - 1;
            while (unpackSize < 0 || produced < unpackSize)
            {
                int position = output.Length - dictionaryStart;
                int posState = position & pbMask;
                if (DecodeBit(isMatch, (state << PosBitsMax) + posState) == 0)
                {
                    int previous = output.Length > dictionaryStart ? output[output.Length - 1] : 0;
                    int offset = 0x300 * (((position & lpMask) << lc) + (previous >> (8 - lc)));
                    int symbol = 1;
                    if (state >= 7)
                    {
                        int matchByte = output[output.Length - (int)rep0 - 1];
                        do
                        {
                            int matchBit = (matchByte >> 7) & 1;
                            matchByte <<= 1;
                            int bit = DecodeBit(literalProbs, offset + ((1 + matchBit) << 8) + symbol);
                            symbol = (symbol << 1) | bit;
                            if (matchBit != bit) { break; }
                        } while (symbol < 0x100);
                    }
                    while (symbol < 0x100) { symbol = (symbol << 1) | DecodeBit(literalProbs, offset + symbol); }
                    output.Append((byte)(symbol - 0x100));
                    produced++;
                    state = state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
                    continue;
                }
                int length;
                if (DecodeBit(isRep, state) != 0)
                {
                    if (position == 0) { throw new LzmaException("a repeated match before any data"); }
                    if (DecodeBit(isRepG0, state) == 0)
                    {
                        if (DecodeBit(isRep0Long, (state << PosBitsMax) + posState) == 0)
                        {
                            if (rep0 >= (uint)position) { throw new LzmaException("distance too far back"); }
                            state = state < 7 ? 9 : 11;
                            output.Append(output[output.Length - (int)rep0 - 1]);
                            produced++;
                            continue;
                        }
                    }
                    else
                    {
                        uint distance;
                        if (DecodeBit(isRepG1, state) == 0)
                        {
                            distance = rep1;
                        }
                        else
                        {
                            if (DecodeBit(isRepG2, state) == 0)
                            {
                                distance = rep2;
                            }
                            else
                            {
                                distance = rep3;
                                rep3 = rep2;
                            }
                            rep2 = rep1;
                        }
                        rep1 = rep0;
                        rep0 = distance;
                    }
                    length = DecodeLength(repLenProbs, posState);
                    state = state < 7 ? 8 : 11;
                }
                else
                {
                    rep3 = rep2;
                    rep2 = rep1;
                    rep1 = rep0;
                    length = DecodeLength(lenProbs, posState);
                    state = state < 7 ? 7 : 10;
                    rep0 = DecodeDistance(length);
                    if (rep0 == 0xFFFFFFFFu)
                    {
                        if (unpackSize >= 0 && produced != unpackSize) { throw new LzmaException("unexpected end marker"); }
                        return inPos;  // the end marker
                    }
                    if (rep0 >= (uint)position) { throw new LzmaException("distance too far back"); }
                }
                length += MatchMinLen;
                if (rep0 >= (uint)position) { throw new LzmaException("distance too far back"); }
                if (unpackSize >= 0 && produced + length > unpackSize) { throw new LzmaException("match beyond the chunk"); }
                output.CopyMatch((int)rep0 + 1, length);
                produced += length;
            }
            return inPos;
        }
    }

    public static class Lzma
    {
        // LZMA data with its 5-byte properties (lc/lp/pb byte, dictionary size) and an end marker, as an
        // Inno Setup block holds it. Returns the decoded bytes; `after` is the position after the data.
        public static byte[] DecodeWithProperties(byte[] data, int start, int end, long limit, out int after)
        {
            if (end - start < 5) { throw new InvalidDataException("LZMA data too short"); }
            LzmaDecoder decoder = new LzmaDecoder(new OutputBuffer(1 << 16, limit));
            decoder.SetProperties(data[start]);
            decoder.ResetState();
            decoder.ResetDictionary();
            try
            {
                after = decoder.Decode(data, start + 5, end, -1);
            }
            catch (LzmaException e)
            {
                throw new InvalidDataException("LZMA: " + e.Message);
            }
            return decoder.Output.ToArray();
        }

        // An LZMA2 stream (RFC-less: the xz format's LZMA2 chunks) after its 1-byte dictionary property,
        // up to its end control byte. `after` is the position after that byte.
        public static byte[] DecodeLzma2(byte[] data, int start, int end, long limit, out int after)
        {
            if (start >= end || data[start] > 40) { throw new InvalidDataException("LZMA2: bad dictionary property"); }
            LzmaDecoder decoder = new LzmaDecoder(new OutputBuffer(1 << 20, limit));
            int p = start + 1;
            bool needDictionaryReset = true;
            bool needProperties = true;
            try
            {
                while (true)
                {
                    if (p >= end) { throw new InvalidDataException("LZMA2: no end"); }
                    int control = data[p++];
                    if (control == 0)
                    {
                        after = p;
                        return decoder.Output.ToArray();
                    }
                    if (control == 1 || control == 2)
                    {
                        if (control == 1) { decoder.ResetDictionary(); needDictionaryReset = false; }
                        else if (needDictionaryReset) { throw new InvalidDataException("LZMA2: no dictionary reset"); }
                        if (p + 2 > end) { throw new InvalidDataException("LZMA2: truncated"); }
                        int size = ((data[p] << 8) | data[p + 1]) + 1;
                        p += 2;
                        if (p + size > end) { throw new InvalidDataException("LZMA2: truncated"); }
                        decoder.Output.Append(data, p, size);
                        p += size;
                        continue;
                    }
                    if (control < 0x80) { throw new InvalidDataException("LZMA2: invalid control byte"); }
                    if (p + 4 > end) { throw new InvalidDataException("LZMA2: truncated"); }
                    int unpacked = ((control & 0x1F) << 16) + (data[p] << 8) + data[p + 1] + 1;
                    int packed = (data[p + 2] << 8) + data[p + 3] + 1;
                    p += 4;
                    int reset = (control >> 5) & 3;
                    if (reset == 3) { decoder.ResetDictionary(); needDictionaryReset = false; }
                    else if (needDictionaryReset) { throw new InvalidDataException("LZMA2: no dictionary reset"); }
                    if (reset >= 2)
                    {
                        if (p >= end) { throw new InvalidDataException("LZMA2: truncated"); }
                        int properties = data[p++];
                        if (properties >= 9 * 5 * 5 || (properties % 9) + ((properties / 9) % 5) > 4) { throw new InvalidDataException("LZMA2: bad properties"); }
                        decoder.SetProperties((byte)properties);
                        needProperties = false;
                    }
                    else if (needProperties) { throw new InvalidDataException("LZMA2: no properties"); }
                    if (reset >= 1) { decoder.ResetState(); }
                    if (p + packed > end) { throw new InvalidDataException("LZMA2: truncated"); }
                    int used = decoder.Decode(data, p, p + packed, unpacked);
                    if (used > p + packed) { throw new InvalidDataException("LZMA2: chunk overrun"); }
                    p += packed;
                }
            }
            catch (LzmaException e)
            {
                throw new InvalidDataException("LZMA2: " + e.Message);
            }
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Inno Setup 6 executables
    // ---------------------------------------------------------------------------------------------

    public static class InnoSetupContent
    {
        private static readonly byte[] OffsetTableId = { 0x72, 0x44, 0x6C, 0x50, 0x74, 0x53, 0xCD, 0xE6, 0xD7, 0x7B, 0x0B, 0x2A };
        private static readonly byte[] FilesId = { 0x7A, 0x6C, 0x62, 0x1A };  // "zlb" #26
        private const string SetupDataId = "Inno Setup Setup Data (";

        private static int Find(byte[] data, byte[] pattern, int from)
        {
            for (int i = from; i + pattern.Length <= data.Length; i++)
            {
                int k = 0;
                while (k < pattern.Length && data[i + k] == pattern[k]) { k++; }
                if (k == pattern.Length) { return i; }
            }
            return -1;
        }

        private static long Int64At(byte[] d, int o) { return (long)UInt32At(d, o) | ((long)UInt32At(d, o + 4) << 32); }
        private static uint UInt32At(byte[] d, int o) { return d[o] | ((uint)d[o + 1] << 8) | ((uint)d[o + 2] << 16) | ((uint)d[o + 3] << 24); }

        private static int Checked(long value, byte[] exe, string what)
        {
            if (value < 0 || value > exe.Length) { throw new InvalidDataException("Inno Setup: " + what + " lies outside the file"); }
            return (int)value;
        }

        // Reads one compressed block (TCompressedBlockReader): CRC-32 of the header, the stored size
        // (64-bit since Inno Setup 6.4, 32-bit before), the compressed flag, then chunks of a CRC-32 and up
        // to 4096 bytes. Returns the decoded bytes; `after` is the position after the block.
        private static byte[] ReadBlock(byte[] exe, int at, long limit, out int after, out long stored)
        {
            if (at + 13 > exe.Length) { throw new InvalidDataException("Inno Setup: truncated block"); }
            int headerSize;
            uint crc = UInt32At(exe, at);
            if (Checksums.Crc32(exe, at + 4, 9) == crc)
            {
                stored = Int64At(exe, at + 4);
                headerSize = 13;
            }
            else if (Checksums.Crc32(exe, at + 4, 5) == crc)
            {
                stored = UInt32At(exe, at + 4);
                headerSize = 9;
            }
            else
            {
                throw new InvalidDataException("Inno Setup: no compressed block at " + at);
            }
            bool compressed = exe[at + headerSize - 1] != 0;
            int p = at + headerSize;
            if (stored < 0 || p + stored > exe.Length) { throw new InvalidDataException("Inno Setup: block size outside the file"); }
            MemoryStream payload = new MemoryStream((int)stored);
            long remaining = stored;
            while (remaining > 0)
            {
                if (remaining < 5) { throw new InvalidDataException("Inno Setup: truncated chunk"); }
                int count = (int)Math.Min(4096, remaining - 4);
                uint chunkCrc = UInt32At(exe, p);
                if (Checksums.Crc32(exe, p + 4, count) != chunkCrc) { throw new InvalidDataException("Inno Setup: chunk checksum mismatch at " + p); }
                payload.Write(exe, p + 4, count);
                p += 4 + count;
                remaining -= 4 + count;
            }
            after = p;
            byte[] bytes = payload.ToArray();
            if (!compressed) { return bytes; }
            int used;
            return Lzma.DecodeWithProperties(bytes, 0, bytes.Length, limit, out used);
        }

        // Inno Setup stores executables with the 24-bit relative addresses of x86 CALL and JMP instructions
        // (E8, E9 followed by an address whose high byte is 00 or FF) made relative to the buffer's start,
        // and the high byte complemented when the result's top bit is set. This undoes it (found by
        // reproducing the CRC-32 the loader's offset table records for the setup engine).
        public static void UndoCallTransform(byte[] buffer)
        {
            int size = buffer.Length - 4;
            int i = 0;
            while (i < size)
            {
                if (buffer[i] != 0xE8 && buffer[i] != 0xE9)
                {
                    i++;
                    continue;
                }
                i++;
                if (buffer[i + 3] == 0x00 || buffer[i + 3] == 0xFF)
                {
                    uint address = buffer[i] | ((uint)buffer[i + 1] << 8) | ((uint)buffer[i + 2] << 16);
                    uint relative = (address - (uint)(i + 4)) & 0xFFFFFFu;
                    buffer[i] = (byte)relative;
                    buffer[i + 1] = (byte)(relative >> 8);
                    buffer[i + 2] = (byte)(relative >> 16);
                    if ((relative & 0x800000u) != 0) { buffer[i + 3] = (byte)~buffer[i + 3]; }
                }
                i += 4;
            }
        }

        // The compressed parts of an Inno Setup executable (see the file comment). InvalidDataException
        // when `exe` is not one whose layout this reader knows (then its compressed content cannot be
        // scanned, and the caller fails).
        public static List<DecodedPart> Extract(byte[] exe, long limit)
        {
            int table = Find(exe, OffsetTableId, 0);
            if (table < 0 || table + 64 > exe.Length) { throw new InvalidDataException("Inno Setup: no loader offset table"); }
            uint version = UInt32At(exe, table + 12);
            if (version != 2) { throw new InvalidDataException("Inno Setup: loader offset table version " + version + " is not known (2 is)"); }
            if (Checksums.Crc32(exe, table, 60) != UInt32At(exe, table + 60)) { throw new InvalidDataException("Inno Setup: loader offset table checksum mismatch"); }
            long totalSize = Int64At(exe, table + 16);
            int offsetExe = Checked(Int64At(exe, table + 24), exe, "the setup engine");
            uint engineSize = UInt32At(exe, table + 32);
            uint engineCrc = UInt32At(exe, table + 36);
            int offset0 = Checked(Int64At(exe, table + 40), exe, "the setup data");
            int offset1 = Checked(Int64At(exe, table + 48), exe, "the file data");
            if (totalSize > exe.Length) { throw new InvalidDataException("Inno Setup: the file is shorter than its offset table says"); }

            List<DecodedPart> parts = new List<DecodedPart>();
            // The setup data: its identifier, the encryption header (Inno Setup 6.4 and later: a CRC-32
            // and the header it covers), then compressed blocks up to the setup engine.
            string id = Encoding.ASCII.GetString(exe, offset0, Math.Min(64, exe.Length - offset0));
            if (!id.StartsWith(SetupDataId, StringComparison.Ordinal)) { throw new InvalidDataException("Inno Setup: no setup data identifier"); }
            int p = offset0 + 64;
            int blockStart = p;
            int dummy;
            long storedSize;
            bool blockFound = false;
            try
            {
                ReadBlock(exe, p, 1, out dummy, out storedSize);
                blockFound = true;
            }
            catch (DecodeLimitException) { blockFound = true; }
            catch (InvalidDataException) { blockFound = false; }
            if (!blockFound)
            {
                uint crc = UInt32At(exe, p);
                for (int n = 1; n <= 512 && p + 4 + n <= exe.Length; n++)
                {
                    if (Checksums.Crc32(exe, p + 4, n) == crc) { blockStart = p + 4 + n; break; }
                }
                if (blockStart == p) { throw new InvalidDataException("Inno Setup: no setup data block"); }
            }
            p = blockStart;
            int index = 0;
            while (p < offsetExe)
            {
                int start = p;
                long stored;
                byte[] block = ReadBlock(exe, p, limit, out p, out stored);
                DecodedPart part = new DecodedPart();
                part.Kind = "Inno Setup setup data, block " + index;
                part.Offset = start;
                part.CompressedLength = p - start;
                part.Data = block;
                parts.Add(part);
                index++;
                if (index > 16) { throw new InvalidDataException("Inno Setup: too many setup data blocks"); }
            }
            if (p != offsetExe) { throw new InvalidDataException("Inno Setup: the setup data blocks do not end at the setup engine"); }
            // The setup engine (Setup.e32): its size and CRC-32 are in the offset table.
            int engineEnd;
            long engineStored;
            byte[] engine = ReadBlock(exe, offsetExe, limit, out engineEnd, out engineStored);
            UndoCallTransform(engine);
            if (engine.Length != engineSize || Checksums.Crc32(engine) != engineCrc) { throw new InvalidDataException("Inno Setup: the setup engine does not match its size and CRC-32"); }
            DecodedPart enginePart = new DecodedPart();
            enginePart.Kind = "Inno Setup setup engine";
            enginePart.Offset = offsetExe;
            enginePart.CompressedLength = engineEnd - offsetExe;
            enginePart.Data = engine;
            parts.Add(enginePart);
            // The installed files: "zlb" #26 chunks between the file data offset and the setup data.
            int q = offset1;
            int chunk = 0;
            int filesEnd = offset0 > offset1 ? offset0 : exe.Length;
            while (q + 4 <= filesEnd && exe[q] == FilesId[0] && exe[q + 1] == FilesId[1] && exe[q + 2] == FilesId[2] &&
                   exe[q + 3] == FilesId[3])
            {
                int start = q + 4;
                int after;
                byte[] files;
                try
                {
                    files = Lzma.DecodeLzma2(exe, start, filesEnd, limit, out after);
                }
                catch (InvalidDataException)
                {
                    files = Lzma.DecodeWithProperties(exe, start, filesEnd, limit, out after);
                }
                DecodedPart filesPart = new DecodedPart();
                filesPart.Kind = "Inno Setup installed files, chunk " + chunk;
                filesPart.Offset = q;
                filesPart.CompressedLength = after - q;
                filesPart.Data = files;
                parts.Add(filesPart);
                q = after;
                chunk++;
            }
            if (chunk == 0) { throw new InvalidDataException("Inno Setup: no file data at the offset table's position"); }
            return parts;
        }
    }
}
