using System.Security.Cryptography;
using System.Text.Json;
using Img2Ffu.Reader;
using Img2Ffu.Reader.Data;
Console.Error.WriteLine("FFU oracle opening");
using (var packed = File.OpenRead(args[0])) {
    var image = new SignedImage(packed);
    Console.Error.WriteLine($"FFU oracle parsed {image.TotalChunkCount} chunks");
    image.VerifyFFU();
    Console.Error.WriteLine("FFU oracle hashes verified");
}
using var decoded = new FullFlashUpdateReaderStream(args[0], 0);
// The upstream reader returns the requested count past EOF and reports an
// extra zero block for all-DISK_BEGIN captures. Bound reads to the manifest's
// original disk size; leave the independent descriptor/payload decoder intact.
long originalBytes = (long)decoded.MinSectorCount * decoded.SectorSize;
using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
byte[] buffer = new byte[65536];
for (long done = 0; done < originalBytes;) {
    int count = decoded.Read(buffer, 0, (int)Math.Min(buffer.Length, originalBytes - done));
    if (count <= 0) throw new EndOfStreamException();
    hash.AppendData(buffer, 0, count); done += count;
}
Console.WriteLine(JsonSerializer.Serialize(new { bytes = originalBytes, sector = decoded.SectorSize,
    sha256 = Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant(), hashesVerified = true }));
