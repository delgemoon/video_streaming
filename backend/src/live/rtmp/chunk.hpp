#pragma once

// RTMP chunk-stream (de)multiplexer. Pure protocol logic, no I/O, no allocation on the per-chunk
// path: messages are assembled directly into a destination buffer provided by the owner so the
// payload can be handed to GStreamer without another copy.

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vsp::rtmp {

enum MessageType : uint8_t {
    kSetChunkSize = 1,
    kAbort = 2,
    kAck = 3,
    kUserControl = 4,
    kWindowAckSize = 5,
    kSetPeerBandwidth = 6,
    kAudio = 8,
    kVideo = 9,
    kDataAmf3 = 15,
    kSharedObjectAmf3 = 16,
    kCommandAmf3 = 17,
    kDataAmf0 = 18,
    kSharedObjectAmf0 = 19,
    kCommandAmf0 = 20,
    kAggregate = 22,
};

struct MessageHeader {
    uint32_t timestamp = 0;      // absolute, ms
    uint32_t length = 0;
    uint8_t type = 0;
    uint32_t stream_id = 0;
};

// Owner-supplied storage for one in-flight message. `data` must have room for `length` bytes.
struct MessageBuffer {
    uint8_t* data = nullptr;
    uint32_t length = 0;
    void* user = nullptr;        // opaque handle (e.g. GstBuffer*) owned by the allocator
};

class ChunkReader {
public:
    // Called when a new message starts; must return storage for `hdr.length` bytes.
    using Allocate = std::function<MessageBuffer(const MessageHeader& hdr)>;
    // Called when a message is complete. Ownership of `buf.user` passes to the callee.
    using Complete = std::function<void(const MessageHeader& hdr, MessageBuffer buf)>;

    ChunkReader(Allocate alloc, Complete done) : alloc_(std::move(alloc)), done_(std::move(done)) {}

    void set_chunk_size(uint32_t n) { chunk_size_ = n ? n : 128; }
    uint32_t chunk_size() const { return chunk_size_; }

    // Consumes as much of [data, data+len) as possible. Returns bytes consumed; the caller must
    // keep the remainder and present it again with more data. Sets `error` on protocol violation.
    size_t feed(const uint8_t* data, size_t len, std::string& error);

private:
    struct Stream {
        MessageHeader hdr;           // last header seen on this chunk stream
        uint32_t ts_delta = 0;
        bool ext_ts = false;         // last header carried an extended timestamp
        MessageBuffer buf;
        uint32_t received = 0;       // bytes of the current message received so far
        bool active = false;         // a message is in progress
    };

    Stream& stream(uint32_t csid) {
        auto it = streams_.find(csid);
        if (it == streams_.end()) it = streams_.emplace(csid, Stream{}).first;
        return it->second;
    }

    Allocate alloc_;
    Complete done_;
    uint32_t chunk_size_ = 128;
    std::unordered_map<uint32_t, Stream> streams_;
};

// Serialises outgoing messages into chunks. Used only for the (tiny) control/command traffic.
class ChunkWriter {
public:
    void set_chunk_size(uint32_t n) { chunk_size_ = n ? n : 128; }
    // Appends the chunked message to `out`. Always uses a type-0 header for the first chunk.
    void write(std::string& out, uint32_t csid, const MessageHeader& hdr, const uint8_t* payload, size_t len) const;
    void write(std::string& out, uint32_t csid, const MessageHeader& hdr, const std::string& payload) const {
        write(out, csid, hdr, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    }

private:
    uint32_t chunk_size_ = 128;
};

} // namespace vsp::rtmp
