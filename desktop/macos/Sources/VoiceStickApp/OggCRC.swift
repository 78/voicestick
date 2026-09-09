import Foundation

enum OggCRC {
    private static let table: [UInt32] = (0..<256).map { i in
        var r = UInt32(i) << 24
        for _ in 0..<8 {
            r = (r & 0x80000000) != 0 ? (r << 1) ^ 0x04C11DB7 : (r << 1)
        }
        return r
    }

    static func checksum(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0
        for byte in data {
            let index = Int(((crc >> 24) & 0xff) ^ UInt32(byte))
            crc = (crc << 8) ^ table[index]
        }
        return crc
    }
}
