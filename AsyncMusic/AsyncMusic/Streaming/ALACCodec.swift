//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AVFoundation

/// Apple Lossless at 16 bits, stereo, one packet per stream chunk.
///
/// ALAC packets decode independently and carry no encoder delay, so a chunk of N frames at
/// frame F decodes to exactly those N frames at F: frame-accurate placement is unaffected,
/// unlike AAC. The decoder needs the encoder's magic cookie, which is sent with each stream
/// start.
nonisolated enum ALACCodec {
    static let framesPerPacket = 4096

    static func compressedFormat(sampleRate: Double) -> AVAudioFormat? {
        var description = streamDescription(sampleRate: sampleRate)
        return AVAudioFormat(streamDescription: &description)
    }

    static func streamDescription(sampleRate: Double) -> AudioStreamBasicDescription {
        AudioStreamBasicDescription(
            mSampleRate: sampleRate,
            mFormatID: kAudioFormatAppleLossless,
            mFormatFlags: kAppleLosslessFormatFlag_16BitSourceData,
            mBytesPerPacket: 0,
            mFramesPerPacket: UInt32(framesPerPacket),
            mBytesPerFrame: 0,
            mChannelsPerFrame: 2,
            mBitsPerChannel: 0,
            mReserved: 0
        )
    }
}

/// Encodes stereo float chunks (at most `framesPerPacket` frames) into single ALAC packets.
nonisolated final class ALACEncoder {
    let sampleRate: Double
    let magicCookie: Data

    private let converter: AVAudioConverter
    private let input: AVAudioPCMBuffer
    private let output: AVAudioCompressedBuffer

    init?(sampleRate: Double) {
        guard let compressed = ALACCodec.compressedFormat(sampleRate: sampleRate),
              let pcm = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: sampleRate, channels: 2, interleaved: true),
              let converter = AVAudioConverter(from: pcm, to: compressed),
              let input = AVAudioPCMBuffer(pcmFormat: pcm, frameCapacity: AVAudioFrameCount(ALACCodec.framesPerPacket))
        else { return nil }
        self.sampleRate = sampleRate
        self.converter = converter
        self.input = input
        output = AVAudioCompressedBuffer(format: compressed, packetCapacity: 2, maximumPacketSize: max(converter.maximumOutputPacketSize, 1))
        magicCookie = converter.magicCookie ?? Data()
    }

    /// Encodes `count` frames, given as 16-bit interleaved stereo samples.
    func encode(interleaved samples: UnsafePointer<Int16>, count: Int) -> Data? {
        precondition(count > 0 && count <= ALACCodec.framesPerPacket)
        input.int16ChannelData![0].update(from: samples, count: count * 2)
        input.frameLength = AVAudioFrameCount(count)
        output.packetCount = 0
        output.byteLength = 0

        // A full packet's worth comes out as one packet with "no more data for now". A short
        // chunk must be flushed with end-of-stream instead: otherwise the encoder keeps those
        // frames (even across `reset()`) and prepends them to the next chunk. End-of-stream
        // is not used for full chunks because it appends an empty trailer packet.
        let isFull = count == ALACCodec.framesPerPacket
        var supplied = false
        var error: NSError?
        let status = converter.convert(to: output, error: &error) { [input] _, inputStatus in
            if supplied {
                inputStatus.pointee = isFull ? .noDataNow : .endOfStream
                return nil
            }
            supplied = true
            inputStatus.pointee = .haveData
            return input
        }
        if !isFull {
            converter.reset()
        }
        guard status != .error, output.packetCount >= 1, let description = output.packetDescriptions?[0],
              description.mVariableFramesInPacket == 0 || Int(description.mVariableFramesInPacket) == count
        else { return nil }
        return Data(bytes: output.data + Int(description.mStartOffset), count: Int(description.mDataByteSize))
    }
}

/// Decodes single ALAC packets back into stereo float frames.
///
/// Uses the AudioToolbox converter directly: `AVAudioConverter` returned silence for single
/// ALAC packets fed through `AVAudioCompressedBuffer`, while this path takes the packet and its
/// description explicitly.
nonisolated final class ALACDecoder {
    /// Decoded frames of the last packet, non-interleaved.
    let left: UnsafeMutablePointer<Float>
    let right: UnsafeMutablePointer<Float>

    private let converter: AudioConverterRef
    private let outputList: UnsafeMutableAudioBufferListPointer
    private let packetDescription: UnsafeMutablePointer<AudioStreamPacketDescription>
    private var packetBytes: UnsafeRawPointer?
    private var packetSize: UInt32 = 0

    init?(sampleRate: Double, magicCookie: Data) {
        var input = ALACCodec.streamDescription(sampleRate: sampleRate)
        var output = AudioStreamBasicDescription(
            mSampleRate: sampleRate,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved,
            mBytesPerPacket: 4,
            mFramesPerPacket: 1,
            mBytesPerFrame: 4,
            mChannelsPerFrame: 2,
            mBitsPerChannel: 32,
            mReserved: 0
        )
        var converter: AudioConverterRef?
        guard AudioConverterNew(&input, &output, &converter) == noErr, let converter else { return nil }
        let cookieStatus = magicCookie.withUnsafeBytes { cookie in
            AudioConverterSetProperty(converter, kAudioConverterDecompressionMagicCookie, UInt32(cookie.count), cookie.baseAddress!)
        }
        guard cookieStatus == noErr else {
            AudioConverterDispose(converter)
            return nil
        }
        self.converter = converter
        left = .allocate(capacity: ALACCodec.framesPerPacket)
        right = .allocate(capacity: ALACCodec.framesPerPacket)
        outputList = AudioBufferList.allocate(maximumBuffers: 2)
        packetDescription = .allocate(capacity: 1)
    }

    deinit {
        AudioConverterDispose(converter)
        left.deallocate()
        right.deallocate()
        free(outputList.unsafeMutablePointer)
        packetDescription.deallocate()
    }

    /// Decodes one packet into `left`/`right`; returns the number of frames, which must be
    /// `frameCount`.
    func decode(_ packet: Data, frameCount: Int) -> Int? {
        guard frameCount > 0, frameCount <= ALACCodec.framesPerPacket else { return nil }
        let byteCount = UInt32(ALACCodec.framesPerPacket * MemoryLayout<Float>.size)
        outputList[0] = AudioBuffer(mNumberChannels: 1, mDataByteSize: byteCount, mData: left)
        outputList[1] = AudioBuffer(mNumberChannels: 1, mDataByteSize: byteCount, mData: right)

        var frames = UInt32(ALACCodec.framesPerPacket)
        let status = packet.withUnsafeBytes { raw -> OSStatus in
            packetBytes = raw.baseAddress
            packetSize = UInt32(raw.count)
            packetDescription.pointee = AudioStreamPacketDescription(mStartOffset: 0, mVariableFramesInPacket: 0, mDataByteSize: packetSize)
            defer { packetBytes = nil }
            return AudioConverterFillComplexBuffer(converter, Self.supplyPacket, Unmanaged.passUnretained(self).toOpaque(), &frames, outputList.unsafeMutablePointer, nil)
        }
        AudioConverterReset(converter)
        guard status == noErr || status == Self.noMoreInput, Int(frames) == frameCount else { return nil }
        return frameCount
    }

    /// Returned by the input callback once the single packet has been handed over.
    private static let noMoreInput: OSStatus = 0x6E6F_6D6F  // 'nomo'

    private static let supplyPacket: AudioConverterComplexInputDataProc = { _, packetCount, data, descriptions, context in
        let decoder = Unmanaged<ALACDecoder>.fromOpaque(context!).takeUnretainedValue()
        guard let bytes = decoder.packetBytes else {
            packetCount.pointee = 0
            return noMoreInput
        }
        decoder.packetBytes = nil
        data.pointee.mNumberBuffers = 1
        data.pointee.mBuffers = AudioBuffer(mNumberChannels: 2, mDataByteSize: decoder.packetSize, mData: UnsafeMutableRawPointer(mutating: bytes))
        packetCount.pointee = 1
        descriptions?.pointee = decoder.packetDescription
        return noErr
    }
}
