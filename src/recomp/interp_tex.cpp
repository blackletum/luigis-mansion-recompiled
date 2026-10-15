// Frame interpolation: texture and TLUT snapshots (interp.h, docs/INTERPOLATION.md).
//
// Aurora reads a texture's (and a TLUT's) data from guest memory when a replay first needs it
// (its caches are keyed by texture object id + data version). A tape is replayed up to a game
// frame after it was captured, and a late replay may be the first to resolve a texture (or the
// object's cache entry may already hold a newer version) while the game has already reused the
// memory: a closing Game Boy Horror, a cutscene shot loading its models. So the data each
// texture/TLUT load reads is copied at capture time and the tape's load points at the copy
// (textures: Aurora patch 0028's content pointer, so the texture keeps its address for EFB-copy
// lookups; TLUTs: the data pointer).
//
// EFB copies: a texture over memory an EFB copy wrote is the copy (Aurora's GPU copy texture or
// copy-to-RAM decode) only while the memory still holds what it held at the copy; the GBH, for
// one, later loads ordinary textures into a buffer it once copied the screen into. That is
// decided here, at capture time, by hashing the copy's range (at the copy and when a texture
// overlapping it is loaded): copy-backed textures are left to Aurora, everything else is copied.
//
// Copies are shared: by (object id, data version) without reading the data again, else by a hash
// of the data (games re-initialise texture objects every frame), so a texture is copied once.
#include "recomp/hle.h"
#include "recomp/interp.h"

#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <xxhash.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <vector>

namespace recomp::interp {
namespace {

using Blob = std::shared_ptr<const std::vector<u8>>;

struct Entry {
    Blob blob;
    u32 lastTape = 0;
};
std::unordered_map<u64, Entry> sById;    // (object id << 32 | version) | kind bit
std::unordered_map<u64, Entry> sByHash;  // content hash ^ size
std::vector<Blob> sUsed;                 // this tape's copies
u32 sTapeNo = 1;
u32 sMapTexture[8] = {};  // the texture last loaded into each map (address, size, format)
u64 sCopied = 0;  // bytes copied this tape
u32 sLoads = 0, sBacked = 0, sSnapped = 0;  // texture loads this tape (LM_INTERP_TRACE)

// EFB copy destinations whose memory still holds what it held when the copy was made.
struct CopyDest {
    u32 begin, end;  // physical
    u64 hash;        // of the memory at the copy
    u32 checkedTape;
};
std::vector<CopyDest> sCopies;

u64 hashRange(u32 phys, u32 size) { return XXH3_64bits(g_mem + phys, size); }

// Whether [phys, phys + size) is (partly) EFB-copy memory the CPU has not overwritten since.
bool copyBacked(u32 phys, u32 size) {
    bool backed = false;
    for (auto it = sCopies.begin(); it != sCopies.end();) {
        if (!(phys < it->end && it->begin < phys + size)) {
            ++it;
            continue;
        }
        if (it->checkedTape != sTapeNo) {
            it->checkedTape = sTapeNo;
            if (hashRange(it->begin, it->end - it->begin) != it->hash) {
                it = sCopies.erase(it);  // overwritten: the texture is plain memory now
                continue;
            }
        }
        backed = true;
        ++it;
    }
    return backed;
}

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
u64 be64(const u8* p) { return u64(be32(p)) << 32 | be32(p + 4); }
void put64(u8* p, u64 v) {
    for (int k = 0; k < 8; k++) p[k] = u8(v >> (56 - 8 * k));
}

// Finds the GX_AURORA subcommand `sub` written after tape offset `at` (the BP writes of the
// load come first). Returns a pointer to its first payload byte, or null.
u8* findAurora(u32 at, u16 sub) {
    const u32 end = mark();
    if (at == ~0u || end == ~0u) return nullptr;
    u8* base = detail::captureBytes();
    for (u32 i = at; i + 3 <= end && i < at + 256; i++)
        if (base[i] == GX_AURORA && base[i + 1] == u8(sub >> 8) && base[i + 2] == u8(sub)) return base + i + 3;
    return nullptr;
}

// Physical address of a host pointer into MEM1, or ~0.
u32 physOf(const void* data) {
    const uintptr_t mem = reinterpret_cast<uintptr_t>(g_mem), p = reinterpret_cast<uintptr_t>(data);
    return p >= mem && p < mem + kMem1Size ? u32(p - mem) : ~0u;
}

// The copy of `size` bytes at `phys` for object (id, version): reused if known.
const u8* snapshot(u64 idKey, u32 phys, u32 size) {
    size = std::min(size, kMem1Size - phys);
    const u8* data = g_mem + phys;
    Entry* e = nullptr;
    if (auto it = sById.find(idKey); it != sById.end() && it->second.blob->size() >= size) {
        e = &it->second;
    } else {
        const u64 h = XXH3_64bits(data, size) ^ (u64(size) << 40);
        auto& byHash = sByHash[h];
        if (!byHash.blob || byHash.blob->size() < size) {
            byHash.blob = std::make_shared<const std::vector<u8>>(data, data + size);
            sCopied += size;
        }
        byHash.lastTape = sTapeNo;
        e = &(sById[idKey] = byHash);
    }
    e->lastTape = sTapeNo;
    if (sUsed.empty() || sUsed.back() != e->blob) sUsed.push_back(e->blob);
    return e->blob->data();
}

u32 texSize(u32 w, u32 h, u32 fmt, bool mips) {
    u8 levels = 1;
    if (mips)
        for (u32 m = std::max(w, h); m > 1; m >>= 1) levels++;
    return GXGetTexBufferSize(u16(w), u16(h), fmt, GXBool(mips), levels);
}

}  // namespace

u32 boundTexture(int map) { return sMapTexture[map & 7]; }

void texLoaded(u32 at) {
    if (!capturing()) return;
    u8* c = findAurora(at, GX_AURORA_LOAD_TEXOBJ);
    if (!c) return;
    // u8 map, u64 data, u32 width, height, format, tlut, u8 mips, u32 object id, version, u64 content
    const u32 phys = physOf(reinterpret_cast<const void*>(be64(c + 1)));
    const u32 w = be32(c + 9), h = be32(c + 13), fmt = be32(c + 17);
    const bool mips = c[25] != 0;
    const u32 id = be32(c + 26), version = be32(c + 30);
    sMapTexture[c[0] & 7] = phys ^ (w << 20) ^ (h << 8) ^ fmt;
    if (phys == ~0u || id == 0 || w == 0 || h == 0 || w > 1024 || h > 1024) return;
    const u32 size = texSize(w, h, fmt, mips);
    sLoads++;
    if (drawLogActive()) detail::drawLog("[draws] texture %08X %ux%u fmt %u size %X%s", phys | 0x80000000u, w, h, fmt, size, size && copyBacked(phys, size) ? " copy-backed" : "");
    if (size == 0 || copyBacked(phys, size)) {
        sBacked++;
        return;
    }
    sSnapped++;
    if (const u8* copy = snapshot(u64(id) << 32 | version, phys, size)) put64(c + 34, u64(reinterpret_cast<uintptr_t>(copy)));
}

void tlutLoaded(u32 at) {
    if (!capturing()) return;
    u8* c = findAurora(at, GX_AURORA_LOAD_TLUT);
    if (!c) return;
    // u8 index, u64 data, u32 format, u16 entries, u32 object id, version
    const u32 phys = physOf(reinterpret_cast<const void*>(be64(c + 1)));
    const u32 entries = u32(c[13]) << 8 | c[14];
    const u32 id = be32(c + 15), version = be32(c + 19);
    if (phys == ~0u || id == 0 || entries == 0 || copyBacked(phys, entries * 2)) return;
    if (const u8* copy = snapshot(u64(id) << 32 | version | (1ull << 31), phys, entries * 2))
        put64(c + 1, u64(reinterpret_cast<uintptr_t>(copy)));
}

void efbCopied(u32 guestDest, u32 size) {
    const u32 phys = guestDest & 0x01FFFFFF;
    if (!capturing() || size == 0 || phys >= kMem1Size) return;
    size = std::min(size, kMem1Size - phys);
    // The copy replaces older copies inside its range.
    std::erase_if(sCopies, [&](const CopyDest& d) { return d.begin >= phys && d.end <= phys + size; });
    if (sCopies.size() >= 128) sCopies.erase(sCopies.begin());
    sCopies.push_back({phys, phys + size, hashRange(phys, size), sTapeNo});
}

// Copy-to-RAM wrote a copy's bytes into memory: still the copy (as Aurora treats it).
void efbWrittenBack(const void* dest, u32 size) {
    const u32 phys = physOf(dest);
    if (phys == ~0u) return;
    for (CopyDest& d : sCopies)
        if (phys < d.end && d.begin < phys + size) d.hash = hashRange(d.begin, d.end - d.begin);
}

namespace detail {

void texEndTape(std::vector<std::shared_ptr<const std::vector<u8>>>& keep, u64& copiedBytes) {
    keep = std::move(sUsed);
    sUsed.clear();
    copiedBytes = sCopied;
    static const bool sTrace = std::getenv("LM_INTERP_TRACE") != nullptr;
    if (sTrace)
        log("[interp] textures: %u loads, %u copy-backed, %u from copies (%llu bytes newly copied), %zu copy ranges",
            sLoads, sBacked, sSnapped, (unsigned long long)sCopied, sCopies.size());
    sLoads = sBacked = sSnapped = 0;
    sCopied = 0;
    sTapeNo++;
    // Forget copies no tape used for a while (the tapes that use them keep them alive).
    if ((sTapeNo & 31) == 0)
        for (auto* map : {&sById, &sByHash})
            for (auto it = map->begin(); it != map->end();) it = sTapeNo - it->second.lastTape > 120 ? map->erase(it) : ++it;
}

}  // namespace detail
}  // namespace recomp::interp
