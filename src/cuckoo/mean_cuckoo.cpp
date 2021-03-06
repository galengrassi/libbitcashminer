/*
 * Copyright (c) 2013-2018 John Tromp
 * Copyright (C) 2018 The Merit Foundation
 * Copyright (C) 2018 The BitCash developers
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either vedit_refsion 3 of the License, or
 * (at your option) any later vedit_refsion.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * Botan library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU General Public License in all respects for
 * all of the code used other than Botan. If you modify file(s) with
 * this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do
 * so, delete this exception statement from your version. If you delete
 * this exception statement from all source files in the program, then
 * also delete it here.
 */
#include "bitcash/cuckoo/mean_cuckoo.h"

#include "bitcash/crypto/siphash.h"
#include "bitcash/crypto/siphashxN.h"
#include "bitcash/blake2/blake2.h"
#include <sstream>
#include <bitset>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdint>
#undef min
#undef max
#undef small
// algorithm/performance parameters

// The node bits are logically split into 3 groups:
// XBITS 'X' bits (most significant), YBITS 'Y' bits, and ZBITS 'Z' bits (least significant)
// Here we have the default XBITS=YBITS=7, ZBITS=15 summing to EDGEBITS=29
// nodebits   XXXXXXX YYYYYYY ZZZZZZZZZZZZZZZ
// bit%10     8765432 1098765 432109876543210
// bit/10     2222222 2111111 111110000000000

// The matrix solver stores all edges in a matrix of NX * NX buckets,
// where NX = 2^XBITS is the number of possible values of the 'X' bits.
// Edge i between nodes ui = siphash24(2*i) and vi = siphash24(2*i+1)
// resides in the bucket at (uiX,viX)
// In each trimming round, either a matrix row or a matrix column (NX buckets)
// is bucket sorted on uY or vY respectively, and then within each bucket
// uZ or vZ values are counted and edges with a count of only one are eliminated,
// while remaining edges are bucket sorted back on vX or uX respectively.
// When sufficiently many edges have been eliminated, a pair of compression
// rounds remap surviving Y,Z values in each row or column into 15 bit
// combined YZ values, allowing the remaining rounds to avoid the sorting on Y,
// and directly count YZ values in a cache friendly 32KB.
// A final pair of compression rounds remap YZ values from 15 into 11 bits.

#ifdef __AVX2__


#ifndef NSIPHASH
#define NSIPHASH 8
#endif

#else

#ifndef NSIPHASH
#define NSIPHASH 1
#endif

#endif

namespace bitcash
{
    namespace cuckoo
    {
        const int MAXPATHLEN = 8192;
        /** Minimum number of edge bits for cuckoo miner - block.nEdgeBits value */
        const std::uint16_t MIN_EDGE_BITS = 16;
        /** Maximum number of edge bits for cuckoo miner - block.nEdgeBits value */
        const std::uint16_t MAX_EDGE_BITS = 31;

        // for p close to 0, Pr(X>=k) < e^{-n*p*eps^2} where k=n*p*(1+eps)
        // see https://en.wikipedia.org/wiki/Binomial_distribution#Tail_bounds
        // eps should be at least 1/sqrt(n*p/64)
        // to give negligible bad odds of e^-64.

        // 1/32 reduces odds of overflowing z bucket on 2^30 nodes to 2^14*e^-32
        // (less than 1 in a billion) in theory. not so in practice (fails first at mean30 -n 1549)
#ifndef BIGEPS
#define BIGEPS 5 / 64
#endif

        // 184/256 is safely over 1-e(-1) ~ 0.63 trimming fraction
#ifndef TRIMFRAC256
#define TRIMFRAC256 184
#endif

        // convenience function for extracting siphash keys from header
        void setHeader(const char *header, const std::uint32_t headerlen, crypto::siphash_keys *keys)
        {
            char hdrkey[32];
            blake2b((void *)hdrkey, sizeof(hdrkey), (const void *)header, headerlen, 0, 0);
            crypto::setkeys(keys, hdrkey);
        }

        class Barrier
        {
            public:
                explicit Barrier(std::size_t threadsIn) : threads(threadsIn),
                nCount(threadsIn),
                nGeneration(0)
            {
            }

                void Wait()
                {
                    std::unique_lock<std::mutex> lLock{mMutex};
                    auto lGen = nGeneration;
                    if (!--nCount) {
                        nGeneration++;
                        nCount = threads;
                        cv.notify_all();
                    } else {
                        cv.wait(lLock, [this, lGen] { return lGen != nGeneration; });
                    }
                }

            private:
                std::mutex mMutex;
                std::condition_variable cv;
                std::size_t threads;
                std::size_t nCount;
                std::size_t nGeneration;
        };

        template <std::uint8_t EDGEBITS, std::uint8_t XBITS>
            struct Params {
                // prepare params for algorithm
                const static std::uint32_t EDGEMASK = (1LU << EDGEBITS) - 1U;

                const static std::uint8_t YBITS = XBITS;
                const static std::uint32_t NX = 1 << XBITS;
                const static std::uint32_t XMASK = NX - 1;

                const static std::uint32_t NY = 1 << YBITS;
                const static std::uint32_t YMASK = NY - 1;

                const static std::uint32_t XYBITS = XBITS + YBITS;
                const static std::uint32_t NXY = 1 << XYBITS;

                const static std::uint32_t ZBITS = EDGEBITS - XYBITS;
                const static std::uint32_t NZ = 1 << ZBITS;
                const static std::uint32_t ZMASK = NZ - 1;

                const static std::uint32_t YZBITS = EDGEBITS - XBITS;
                const static std::uint32_t NYZ = 1 << YZBITS;
                const static std::uint32_t YZMASK = NYZ - 1;

                const static std::uint32_t YZ1BITS = YZBITS < 15 ? YZBITS : 15;
                const static std::uint32_t NYZ1 = 1 << YZ1BITS;
                const static std::uint32_t YZ1MASK = NYZ1 - 1;

                const static std::uint32_t Z1BITS = YZ1BITS - YBITS;
                const static std::uint32_t NZ1 = 1 << Z1BITS;
                const static std::uint32_t Z1MASK = NZ1 - 1;

                const static std::uint32_t YZ2BITS = YZBITS < 11 ? YZBITS : 11;
                const static std::uint32_t NYZ2 = 1 << YZ2BITS;
                const static std::uint32_t YZ2MASK = NYZ2 - 1;

                const static std::uint32_t Z2BITS = YZ2BITS - YBITS;
                const static std::uint32_t NZ2 = 1 << Z2BITS;
                const static std::uint32_t Z2MASK = NZ2 - 1;

                const static std::uint32_t YZZBITS = YZBITS + ZBITS;
                const static std::uint32_t YZZ1BITS = YZ1BITS + ZBITS;

                const static std::uint8_t COMPRESSROUND = EDGEBITS <= 15 ? 0 : (EDGEBITS < 30 ? 14 : 22);
                const static std::uint8_t EXPANDROUND = EDGEBITS < 30 ? COMPRESSROUND : 8;

                const static std::uint8_t BIGSIZE = EDGEBITS <= 15 ? 4 : 5;
                const static std::uint8_t BIGSIZE0 = EDGEBITS < 30 ? 4 : BIGSIZE;
                const static std::uint8_t SMALLSIZE = BIGSIZE;
                const static std::uint8_t BIGGERSIZE = EDGEBITS < 30 ? BIGSIZE : BIGSIZE + 1;

                const static std::uint32_t BIGSLOTBITS = BIGSIZE * 8;
                const static std::uint32_t SMALLSLOTBITS = SMALLSIZE * 8;
                const static std::uint64_t BIGSLOTMASK = (1ULL << BIGSLOTBITS) - 1ULL;
                const static std::uint64_t SMALLSLOTMASK = (1ULL << SMALLSLOTBITS) - 1ULL;
                const static std::uint32_t BIGSLOTBITS0 = BIGSIZE0 * 8;
                const static std::uint64_t BIGSLOTMASK0 = (1ULL << BIGSLOTBITS0) - 1ULL;

                const static std::uint32_t NONYZBITS = BIGSLOTBITS0 - YZBITS;
                const static std::uint32_t NNONYZ = 1 << NONYZBITS;

                const static std::uint32_t NTRIMMEDZ = NZ * TRIMFRAC256 / 256;
                const static std::uint32_t ZBUCKETSLOTS = NZ + NZ * BIGEPS;
                const static std::uint32_t ZBUCKETSIZE = ZBUCKETSLOTS * BIGSIZE0;
                const static std::uint32_t TBUCKETSIZE = ZBUCKETSLOTS * BIGSIZE;

                const static bool NEEDSYNC = BIGSIZE0 == 4 && EDGEBITS > 27;

                // grow with cube root of size, hardly affected by trimming
                // const static std::uint32_t CUCKOO_SIZE = 2 * NX * NYZ2;
                const static std::uint32_t CUCKOO_SIZE = 2 * NX * NYZ2;
            };

        template <std::uint8_t EDGEBITS, std::uint8_t XBITS, std::uint32_t BUCKETSIZE>
            struct zbucket {
                using P = Params<EDGEBITS, XBITS>;
                std::uint32_t size;
                const static std::uint32_t RENAMESIZE = 2 * P::NZ2 + 2 * (P::COMPRESSROUND ? P::NZ1 : 0);
                union alignas(16) {
                    std::uint8_t bytes[BUCKETSIZE];
                    struct {
                        std::uint32_t words[BUCKETSIZE / sizeof(std::uint32_t) - RENAMESIZE];
                        std::uint32_t renameu1[P::NZ2];
                        std::uint32_t renamev1[P::NZ2];
                        std::uint32_t renameu[P::COMPRESSROUND ? P::NZ1 : 0];
                        std::uint32_t renamev[P::COMPRESSROUND ? P::NZ1 : 0];
                    };
                };
                std::uint32_t setsize(std::uint8_t const* end)
                {
                    size = end - bytes;
                    assert(size <= BUCKETSIZE);
                    return size;
                }
            };

        template <std::uint8_t EDGEBITS, std::uint8_t XBITS, std::uint32_t BUCKETSIZE>
            using yzbucket = zbucket<EDGEBITS, XBITS, BUCKETSIZE>[Params<EDGEBITS, XBITS>::NY];

        template <std::uint8_t EDGEBITS, std::uint8_t XBITS, std::uint32_t BUCKETSIZE>
            using matrix = yzbucket<EDGEBITS, XBITS, BUCKETSIZE>[Params<EDGEBITS, XBITS>::NX];

        template <typename offset_t, std::uint8_t EDGEBITS, std::uint8_t XBITS, std::uint32_t BUCKETSIZE>
            struct indexer {
                using P = Params<EDGEBITS, XBITS>;

                offset_t index[P::NX];

                void matrixv(const std::uint32_t y)
                {
                    const yzbucket<EDGEBITS, XBITS, BUCKETSIZE>* foo = 0;
                    for (std::uint32_t x = 0; x < P::NX; x++)
                        index[x] = foo[x][y].bytes - (std::uint8_t*)foo;
                }
                offset_t storev(yzbucket<EDGEBITS, XBITS, BUCKETSIZE>* buckets, const std::uint32_t y)
                {
                    std::uint8_t const* base = (std::uint8_t*)buckets;
                    offset_t sumsize = 0;
                    for (std::uint32_t x = 0; x < P::NX; x++) {
                        sumsize += buckets[x][y].setsize(base + index[x]);
                    }
                    return sumsize;
                }
                void matrixu(const std::uint32_t x)
                {
                    const yzbucket<EDGEBITS, XBITS, BUCKETSIZE>* foo = 0;
                    for (std::uint32_t y = 0; y < P::NY; y++)
                        index[y] = foo[x][y].bytes - (std::uint8_t*)foo;
                }
                offset_t storeu(yzbucket<EDGEBITS, XBITS, BUCKETSIZE>* buckets, const std::uint32_t x)
                {
                    std::uint8_t const* base = (std::uint8_t*)buckets;
                    offset_t sumsize = 0;
                    for (std::uint32_t y = 0; y < P::NY; y++)
                        sumsize += buckets[x][y].setsize(base + index[y]);
                    return sumsize;
                }
            };
#ifdef __GNUC__
#define likely(x) __builtin_expect((x) != 0, 1)
#define unlikely(x) __builtin_expect((x), 0)
#else 
#define likely(x) (x)
#define unlikely(x) (x)
#endif

        // break circular reference with forward declaration

        template <typename offset_t, std::uint8_t EDGEBITS, std::uint8_t XBITS>
            class edgetrimmer;

        template <typename offset_t, std::uint8_t EDGEBITS, std::uint8_t XBITS>
            class solver_ctx;

        template <typename offset_t, std::uint8_t EDGEBITS, std::uint8_t XBITS>
            void etworker(edgetrimmer<offset_t, EDGEBITS, XBITS>* et, std::uint32_t id)
            {
                et->trimmer(id);
            }

        template <typename offset_t, std::uint8_t EDGEBITS, std::uint8_t XBITS>
            void matchworker(solver_ctx<offset_t, EDGEBITS, XBITS>* solver, std::uint32_t id)
            {
                solver->matchUnodes(id);
            }

        template <typename T>
            constexpr T& cmax(T& a, T& b)
            {
                return a > b ? a : b;
            }

        template <std::uint8_t EDGEBITS, std::uint8_t XBITS>
            using zbucket8 = std::uint8_t[2 * cmax(Params<EDGEBITS, XBITS>::NZ, Params<EDGEBITS, XBITS>::NYZ1)];

        template <std::uint8_t EDGEBITS, std::uint8_t XBITS>
            using zbucket16 = std::uint16_t[Params<EDGEBITS, XBITS>::NTRIMMEDZ];

        template <std::uint8_t EDGEBITS, std::uint8_t XBITS>
            using zbucket32 = std::uint32_t[Params<EDGEBITS, XBITS>::NTRIMMEDZ];

        // maintains set of trimmable edges
        template <typename offset_t, std::uint8_t EDGEBITS, std::uint8_t XBITS>
            class edgetrimmer
            {
                public:
                    using P = Params<EDGEBITS, XBITS>;
                    using zbucket8P = std::uint8_t[2 * cmax(Params<EDGEBITS, XBITS>::NZ, Params<EDGEBITS, XBITS>::NYZ1)];
                    using zbucket16P = zbucket16<EDGEBITS, XBITS>;
                    using zbucket32P = zbucket32<EDGEBITS, XBITS>;
                    using zbucketZ = zbucket<EDGEBITS, XBITS, P::ZBUCKETSIZE>;
                    using yzbucketZ = yzbucket<EDGEBITS, XBITS, P::ZBUCKETSIZE>;
                    using yzbucketT = yzbucket<EDGEBITS, XBITS, P::TBUCKETSIZE>;
                    using indexerZ = indexer<offset_t, EDGEBITS, XBITS, P::ZBUCKETSIZE>;
                    using indexerT = indexer<offset_t, EDGEBITS, XBITS, P::TBUCKETSIZE>;

                    crypto::siphash_keys sip_keys;
                    yzbucketZ* buckets;
                    yzbucketT* tbuckets;
                    zbucket32P* tedges;
                    zbucket16P* tzs;
                    zbucket8P* tdegs;
                    offset_t* tcounts;
                    std::uint8_t threads;
                    ctpl::thread_pool& pool;
                    std::uint32_t nTrims;
                    Barrier* barry;

                    using BIGTYPE0 = offset_t;

                    void touch(std::uint8_t* p, const offset_t n)
                    {
                        for (offset_t i = 0; i < n; i += 4096)
                            *(std::uint32_t*)(p + i) = 0;
                    }

                    edgetrimmer(
                            ctpl::thread_pool& poolIn,
                            size_t threadsIn,
                            const std::uint32_t nTrimsIn) : pool{poolIn}, nTrims{nTrimsIn}
                    {                    

                        threads = threadsIn;

                        buckets = new yzbucketZ[P::NX];
                        touch((std::uint8_t*)buckets, sizeof(zbucket<EDGEBITS, XBITS, P::ZBUCKETSIZE>) * P::NX * P::NY);
                        tbuckets = new yzbucketT[threads];
                        touch((std::uint8_t*)tbuckets, threads * sizeof(yzbucketT));

                        tedges = new zbucket32P[threads];
                        tdegs = new zbucket8P[threads];
                        tzs = new zbucket16P[threads];
                        tcounts = new offset_t[threads];

                        barry = new Barrier(threads);
                    }
                    ~edgetrimmer()
                    {
                        delete[] buckets;
                        delete[] tbuckets;
                        delete[] tedges;
                        delete[] tdegs;
                        delete[] tzs;
                        delete[] tcounts;
                        delete barry;
                    }
                    offset_t count() const
                    {
                        offset_t cnt = 0;
                        for (std::uint32_t t = 0; t < threads; t++)
                            cnt += tcounts[t];
                        return cnt;
                    }

#if NSIPHASH == 8

                    template <int x, int i>
                        void store(
                                std::uint8_t const* base,
                                std::uint32_t& ux,
                                indexerZ& dst,
                                std::uint32_t last[],
                                const std::uint32_t edge,
                                __m256i v,
                                __m256i w)
                        {
                            if (!P::NEEDSYNC) {
                                ux = _mm256_extract_epi32(v, x);
                                *(std::uint64_t*)(base + dst.index[ux]) = _mm256_extract_epi64(w, i % 4);
                                dst.index[ux] += P::BIGSIZE0;
                            } else {
                                std::uint32_t zz = _mm256_extract_epi32(w, x);

                                if (i || likely(zz)) {
                                    ux = _mm256_extract_epi32(v, x);
                                    for (; unlikely(last[ux] + P::NNONYZ <= edge + i); last[ux] += P::NNONYZ, dst.index[ux] += P::BIGSIZE0)
                                        *(std::uint32_t*)(base + dst.index[ux]) = 0;
                                    *(std::uint32_t*)(base + dst.index[ux]) = zz;
                                    dst.index[ux] += P::BIGSIZE0;
                                    last[ux] = edge + i;
                                }
                            }
                        }
#endif

                    void genUnodes(const std::uint32_t id, const std::uint32_t uorv)
                    {
                        std::uint32_t last[P::NX];

                        std::uint8_t const* base = (std::uint8_t*)buckets;
                        indexerZ dst;
                        const std::uint32_t starty = P::NY * id / threads;
                        const std::uint32_t endy = P::NY * (id + 1) / threads;

                        std::uint32_t edge = starty << P::YZBITS;
                        std::uint32_t endedge = edge + P::NYZ;

#if NSIPHASH == 8
                        static const __m256i vxmask = {P::XMASK, P::XMASK, P::XMASK, P::XMASK};
                        static const __m256i vyzmask = {P::YZMASK, P::YZMASK, P::YZMASK, P::YZMASK};
                        const __m256i vinit = _mm256_set_epi64x(
                                sip_keys.k1 ^ 0x7465646279746573ULL,
                                sip_keys.k0 ^ 0x6c7967656e657261ULL,
                                sip_keys.k1 ^ 0x646f72616e646f6dULL,
                                sip_keys.k0 ^ 0x736f6d6570736575ULL);
                        __m256i v0, v1, v2, v3, v4, v5, v6, v7;
                        const std::uint32_t e2 = 2 * edge + uorv;
                        __m256i vpacket0 = _mm256_set_epi64x(e2 + 6, e2 + 4, e2 + 2, e2 + 0);
                        __m256i vpacket1 = _mm256_set_epi64x(e2 + 14, e2 + 12, e2 + 10, e2 + 8);
                        static const __m256i vpacketinc = {16, 16, 16, 16};
                        std::uint64_t e1 = edge;
                        __m256i vhi0 = _mm256_set_epi64x((e1 + 3) << P::YZBITS, (e1 + 2) << P::YZBITS, (e1 + 1) << P::YZBITS, (e1 + 0) << P::YZBITS);
                        __m256i vhi1 = _mm256_set_epi64x((e1 + 7) << P::YZBITS, (e1 + 6) << P::YZBITS, (e1 + 5) << P::YZBITS, (e1 + 4) << P::YZBITS);
                        static const __m256i vhiinc = {8 << P::YZBITS, 8 << P::YZBITS, 8 << P::YZBITS, 8 << P::YZBITS};
#endif

                        offset_t sumsize = 0;
                        for (std::uint32_t my = starty; my < endy; my++, endedge += P::NYZ) {
                            dst.matrixv(my);

                            if (P::NEEDSYNC) {
                                for (std::uint32_t x = 0; x < P::NX; x++) {
                                    last[x] = edge;
                                }
                            }
                            // edge is a "nonce" for sipnode()
                            for (; edge < endedge; edge += NSIPHASH) {
                                // bit        28..21     20..13    12..0
                                // node       XXXXXX     YYYYYY    ZZZZZ

#if NSIPHASH == 1
                                const std::uint32_t node = _sipnode(&sip_keys, P::EDGEMASK, edge, uorv);
                                const std::uint32_t ux = node >> P::YZBITS;
                                const BIGTYPE0 zz = (BIGTYPE0)edge << P::YZBITS | (node & P::YZMASK);

                                if (!P::NEEDSYNC) {
                                    // bit        39..21     20..13    12..0
                                    // write        edge     YYYYYY    ZZZZZ
                                    *(BIGTYPE0*)(base + dst.index[ux]) = zz;
                                    dst.index[ux] += P::BIGSIZE0;
                                } else {
                                    if (zz) {
                                        for (; unlikely(last[ux] + P::NNONYZ <= edge); last[ux] += P::NNONYZ, dst.index[ux] += P::BIGSIZE0)
                                            *(std::uint32_t*)(base + dst.index[ux]) = 0;
                                        *(std::uint32_t*)(base + dst.index[ux]) = zz;
                                        dst.index[ux] += P::BIGSIZE0;
                                        last[ux] = edge;
                                    }
                                }
#elif NSIPHASH == 8
                                v3 = _mm256_permute4x64_epi64(vinit, 0xFF);
                                v0 = _mm256_permute4x64_epi64(vinit, 0x00);
                                v1 = _mm256_permute4x64_epi64(vinit, 0x55);
                                v2 = _mm256_permute4x64_epi64(vinit, 0xAA);
                                v7 = _mm256_permute4x64_epi64(vinit, 0xFF);
                                v4 = _mm256_permute4x64_epi64(vinit, 0x00);
                                v5 = _mm256_permute4x64_epi64(vinit, 0x55);
                                v6 = _mm256_permute4x64_epi64(vinit, 0xAA);

                                v3 = XOR(v3, vpacket0);
                                v7 = XOR(v7, vpacket1);
                                SIPROUNDX8;
                                SIPROUNDX8;
                                v0 = XOR(v0, vpacket0);
                                v4 = XOR(v4, vpacket1);
                                v2 = XOR(v2, _mm256_broadcastq_epi64(_mm_cvtsi64_si128(0xff)));
                                v6 = XOR(v6, _mm256_broadcastq_epi64(_mm_cvtsi64_si128(0xff)));
                                SIPROUNDX8;
                                SIPROUNDX8;
                                SIPROUNDX8;
                                SIPROUNDX8;
                                v0 = XOR(XOR(v0, v1), XOR(v2, v3));
                                v4 = XOR(XOR(v4, v5), XOR(v6, v7));

                                vpacket0 = _mm256_add_epi64(vpacket0, vpacketinc);
                                vpacket1 = _mm256_add_epi64(vpacket1, vpacketinc);
                                v1 = _mm256_srli_epi64(v0, P::YZBITS) & vxmask;
                                v5 = _mm256_srli_epi64(v4, P::YZBITS) & vxmask;
                                v0 = (v0 & vyzmask) | vhi0;
                                v4 = (v4 & vyzmask) | vhi1;
                                vhi0 = _mm256_add_epi64(vhi0, vhiinc);
                                vhi1 = _mm256_add_epi64(vhi1, vhiinc);

                                std::uint32_t ux;

                                store<0, 0>(base, ux, dst, last, edge, v1, v0);
                                store<2, 1>(base, ux, dst, last, edge, v1, v0);
                                store<4, 2>(base, ux, dst, last, edge, v1, v0);
                                store<6, 3>(base, ux, dst, last, edge, v1, v0);
                                store<0, 4>(base, ux, dst, last, edge, v5, v4);
                                store<2, 5>(base, ux, dst, last, edge, v5, v4);
                                store<4, 6>(base, ux, dst, last, edge, v5, v4);
                                store<6, 7>(base, ux, dst, last, edge, v5, v4);
#else
#error not implemented
#endif
                            }

                            if (P::NEEDSYNC) {
                                for (std::uint32_t ux = 0; ux < P::NX; ux++) {
                                    for (; last[ux] < endedge - P::NNONYZ; last[ux] += P::NNONYZ) {
                                        *(std::uint32_t*)(base + dst.index[ux]) = 0;
                                        dst.index[ux] += P::BIGSIZE0;
                                    }
                                }
                            }

                            sumsize += dst.storev(buckets, my);
                        }
                        tcounts[id] = sumsize / P::BIGSIZE0;
                    }

                    // Porcess butckets and discard nodes with one edge for it (means it won't be in a cycle)
                    // Generate new paired nodes for remaining nodes generated in genUnodes step
                    void genVnodes(const std::uint32_t id, const std::uint32_t uorv)
                    {
#if NSIPHASH == 8
                        static const __m256i vxmask = {P::XMASK, P::XMASK, P::XMASK, P::XMASK};
                        static const __m256i vyzmask = {P::YZMASK, P::YZMASK, P::YZMASK, P::YZMASK};
                        const __m256i vinit = _mm256_set_epi64x(
                                sip_keys.k1 ^ 0x7465646279746573ULL,
                                sip_keys.k0 ^ 0x6c7967656e657261ULL,
                                sip_keys.k1 ^ 0x646f72616e646f6dULL,
                                sip_keys.k0 ^ 0x736f6d6570736575ULL);
                        __m256i vpacket0, vpacket1, vhi0, vhi1;
                        __m256i v0, v1, v2, v3, v4, v5, v6, v7;
#endif

                        static const std::uint32_t NONDEGBITS = std::min(40u, 2 * P::YZBITS) - P::ZBITS; // 28
                        static const std::uint32_t NONDEGMASK = (1 << NONDEGBITS) - 1;
                        indexerZ dst;
                        indexerT small;

                        offset_t sumsize = 0;
                        std::uint8_t const* base = (std::uint8_t*)buckets;
                        std::uint8_t const* small0 = (std::uint8_t*)tbuckets[id];
                        const std::uint32_t startux = P::NX * id / threads;
                        const std::uint32_t endux = P::NX * (id + 1) / threads;

                        for (std::uint32_t ux = startux; ux < endux; ux++) { // matrix x == ux
                            small.matrixu(0);
                            for (std::uint32_t my = 0; my < P::NY; my++) {
                                std::uint32_t edge = my << P::YZBITS;
                                std::uint8_t* readbig = buckets[ux][my].bytes;
                                std::uint8_t const* endreadbig = readbig + buckets[ux][my].size;
                                for (; readbig < endreadbig; readbig += P::BIGSIZE0) {
                                    // bit     39/31..21     20..13    12..0
                                    // read         edge     UYYYYY    UZZZZ   within UX partition
                                    BIGTYPE0 e = *(BIGTYPE0*)readbig;
                                    if (P::BIGSIZE0 > 4) {
                                        e &= P::BIGSLOTMASK0;
                                    } else if (P::NEEDSYNC) {
                                        if (unlikely(!e)) {
                                            edge += P::NNONYZ;
                                            continue;
                                        }
                                    }
                                    // restore edge generated in genUnodes
                                    edge += ((std::uint32_t)(e >> P::YZBITS) - edge) & (P::NNONYZ - 1);
                                    const std::uint32_t uy = (e >> P::ZBITS) & P::YMASK;
                                    // bit         39..13     12..0
                                    // write         edge     UZZZZ   within UX UY partition
                                    *(std::uint64_t*)(small0 + small.index[uy]) = ((std::uint64_t)edge << P::ZBITS) | (e & P::ZMASK);
                                    small.index[uy] += P::SMALLSIZE;
                                }
                            }

                            // counts of zz's for this ux
                            std::uint8_t* degs = tdegs[id];
                            small.storeu(tbuckets + id, 0);
                            dst.matrixu(ux);
                            for (std::uint32_t uy = 0; uy < P::NY; uy++) {
                                memset(degs, 0xff, P::NZ);
                                std::uint8_t *readsmall = tbuckets[id][uy].bytes, *endreadsmall = readsmall + tbuckets[id][uy].size;

                                for (std::uint8_t* rdsmall = readsmall; rdsmall < endreadsmall; rdsmall += P::SMALLSIZE) {
                                    degs[*(std::uint32_t*)rdsmall & P::ZMASK]++;
                                }

                                std::uint16_t* zs = tzs[id];
                                std::uint32_t* edges0;
                                edges0 = tedges[id]; // list of nodes with 2+ edges
                                std::uint32_t *edges = edges0, edge = 0;

                                for (std::uint8_t* rdsmall = readsmall; rdsmall < endreadsmall; rdsmall += P::SMALLSIZE) {
                                    // bit         39..13     12..0
                                    // read          edge     UZZZZ    sorted by UY within UX partition
                                    const std::uint64_t e = *(std::uint64_t*)rdsmall;

                                    edge += ((e >> P::ZBITS) - edge) & NONDEGMASK;
                                    *edges = edge;
                                    const std::uint32_t z = e & P::ZMASK;
                                    *zs = z;

                                    // check if array of ZZs counts (degs[]) has value not equal to 0 (means we have one edge for that node)
                                    // if it's the only edge, then it would be rewritten in zs and edges arrays in next iteration (skipped)
                                    const std::uint32_t delta = degs[z] ? 1 : 0;
                                    edges += delta;
                                    zs += delta;
                                }
                                assert(edges - edges0 < P::NTRIMMEDZ);
                                const std::uint16_t* readz = tzs[id];
                                const std::uint32_t* readedge = edges0;
                                std::int64_t uy34 = (std::int64_t)uy << P::YZZBITS;

#if NSIPHASH == 8
                                const __m256i vuy34 = {uy34, uy34, uy34, uy34};
                                const __m256i vuorv = {uorv, uorv, uorv, uorv};
                                for (; readedge <= edges - NSIPHASH; readedge += NSIPHASH, readz += NSIPHASH) {
                                    v3 = _mm256_permute4x64_epi64(vinit, 0xFF);
                                    v0 = _mm256_permute4x64_epi64(vinit, 0x00);
                                    v1 = _mm256_permute4x64_epi64(vinit, 0x55);
                                    v2 = _mm256_permute4x64_epi64(vinit, 0xAA);
                                    v7 = _mm256_permute4x64_epi64(vinit, 0xFF);
                                    v4 = _mm256_permute4x64_epi64(vinit, 0x00);
                                    v5 = _mm256_permute4x64_epi64(vinit, 0x55);
                                    v6 = _mm256_permute4x64_epi64(vinit, 0xAA);

                                    vpacket0 = _mm256_slli_epi64(_mm256_cvtepu32_epi64(*(__m128i*)readedge), 1) | vuorv;
                                    vhi0 = vuy34 | _mm256_slli_epi64(_mm256_cvtepu16_epi64(_mm_set_epi64x(0, *(std::uint64_t*)readz)), P::YZBITS);
                                    vpacket1 = _mm256_slli_epi64(_mm256_cvtepu32_epi64(*(__m128i*)(readedge + 4)), 1) | vuorv;
                                    vhi1 = vuy34 | _mm256_slli_epi64(_mm256_cvtepu16_epi64(_mm_set_epi64x(0, *(std::uint64_t*)(readz + 4))), P::YZBITS);

                                    v3 = XOR(v3, vpacket0);
                                    v7 = XOR(v7, vpacket1);
                                    SIPROUNDX8;
                                    SIPROUNDX8;
                                    v0 = XOR(v0, vpacket0);
                                    v4 = XOR(v4, vpacket1);
                                    v2 = XOR(v2, _mm256_broadcastq_epi64(_mm_cvtsi64_si128(0xff)));
                                    v6 = XOR(v6, _mm256_broadcastq_epi64(_mm_cvtsi64_si128(0xff)));
                                    SIPROUNDX8;
                                    SIPROUNDX8;
                                    SIPROUNDX8;
                                    SIPROUNDX8;
                                    v0 = XOR(XOR(v0, v1), XOR(v2, v3));
                                    v4 = XOR(XOR(v4, v5), XOR(v6, v7));

                                    v1 = _mm256_srli_epi64(v0, P::YZBITS) & vxmask;
                                    v5 = _mm256_srli_epi64(v4, P::YZBITS) & vxmask;
                                    v0 = vhi0 | (v0 & vyzmask);
                                    v4 = vhi1 | (v4 & vyzmask);

                                    std::uint32_t vx;
#define STORE(i, v, x, w)                                                \
                                    vx = _mm256_extract_epi32(v, x);                                     \
                                    *(std::uint64_t*)(base + dst.index[vx]) = _mm256_extract_epi64(w, i % 4); \
                                    dst.index[vx] += P::BIGSIZE;
                                    STORE(0, v1, 0, v0);
                                    STORE(1, v1, 2, v0);
                                    STORE(2, v1, 4, v0);
                                    STORE(3, v1, 6, v0);
                                    STORE(4, v5, 0, v4);
                                    STORE(5, v5, 2, v4);
                                    STORE(6, v5, 4, v4);
                                    STORE(7, v5, 6, v4);
                                }
#endif

                                for (; readedge < edges; readedge++, readz++) { // process up to 7 leftover edges if NSIPHASH==8
                                    const std::uint32_t node = _sipnode(&sip_keys, P::EDGEMASK, *readedge, uorv);
                                    const std::uint32_t vx = node >> P::YZBITS; // & XMASK;

                                    // bit        39..34    33..21     20..13     12..0
                                    // write      UYYYYY    UZZZZZ     VYYYYY     VZZZZ   within VX partition
                                    // prev bucket info generated in genUnodes is overwritten here,
                                    // as we store U and V nodes in one value (Yz and Zs; Xs are indices in a matrix)
                                    // edge is discarded here, as we do not need it anymore
                                    *(std::uint64_t*)(base + dst.index[vx]) = uy34 | ((std::uint64_t)*readz << P::YZBITS) | (node & P::YZMASK);
                                    dst.index[vx] += P::BIGSIZE;
                                }
                            }
                            sumsize += dst.storeu(buckets, ux);
                        }
                        tcounts[id] = sumsize / P::BIGSIZE;
                    }

                    template <std::uint32_t SRCSIZE, std::uint32_t DSTSIZE, bool TRIMONV>
                        void trimedges(const std::uint32_t id, const std::uint32_t round)
                        {
                            const std::uint32_t SRCSLOTBITS = std::min(SRCSIZE * 8, 2 * P::YZBITS);
                            const std::uint64_t SRCSLOTMASK = (1ULL << SRCSLOTBITS) - 1ULL;
                            const std::uint32_t SRCPREFBITS = SRCSLOTBITS - P::YZBITS;
                            const std::uint32_t SRCPREFMASK = (1 << SRCPREFBITS) - 1;
                            const std::uint32_t DSTSLOTBITS = std::min(DSTSIZE * 8, 2 * P::YZBITS);
                            const std::uint64_t DSTSLOTMASK = (1ULL << DSTSLOTBITS) - 1ULL;
                            const std::uint32_t DSTPREFBITS = DSTSLOTBITS - P::YZZBITS;
                            const std::uint32_t DSTPREFMASK = (1 << DSTPREFBITS) - 1;
                            indexerZ dst;
                            indexerT small;

                            offset_t sumsize = 0;
                            std::uint8_t const* base = (std::uint8_t*)buckets;
                            std::uint8_t const* small0 = (std::uint8_t*)tbuckets[id];
                            const std::uint32_t startvx = P::NY * id / threads;
                            const std::uint32_t endvx = P::NY * (id + 1) / threads;
                            for (std::uint32_t vx = startvx; vx < endvx; vx++) {
                                small.matrixu(0);
                                for (std::uint32_t ux = 0; ux < P::NX; ux++) {
                                    std::uint32_t uxyz = ux << P::YZBITS;
                                    zbucketZ& zb = TRIMONV ? buckets[ux][vx] : buckets[vx][ux];
                                    const std::uint8_t *readbig = zb.bytes, *endreadbig = readbig + zb.size;
                                    for (; readbig < endreadbig; readbig += SRCSIZE) {
                                        // bit        39..34    33..21     20..13     12..0
                                        // write      UYYYYY    UZZZZZ     VYYYYY     VZZZZ   within VX partition
                                        const std::uint64_t e = *(std::uint64_t*)readbig & SRCSLOTMASK;
                                        uxyz += ((std::uint32_t)(e >> P::YZBITS) - uxyz) & SRCPREFMASK;
                                        const std::uint32_t vy = (e >> P::ZBITS) & P::YMASK;
                                        // bit     41/39..34    33..26     25..13     12..0
                                        // write      UXXXXX    UYYYYY     UZZZZZ     VZZZZ   within VX VY partition
                                        *(std::uint64_t*)(small0 + small.index[vy]) = ((std::uint64_t)uxyz << P::ZBITS) | (e & P::ZMASK);
                                        uxyz &= ~P::ZMASK;
                                        small.index[vy] += DSTSIZE;
                                    }
                                    if (unlikely(uxyz >> P::YZBITS != ux)) {
                                        assert(false);
                                    }
                                }
                                std::uint8_t* degs = tdegs[id];
                                small.storeu(tbuckets + id, 0);
                                TRIMONV ? dst.matrixv(vx) : dst.matrixu(vx);
                                for (std::uint32_t vy = 0; vy < P::NY; vy++) {
                                    const std::uint64_t vy34 = (std::uint64_t)vy << P::YZZBITS;
                                    memset(degs, 0xff, P::NZ);
                                    std::uint8_t *readsmall = tbuckets[id][vy].bytes, *endreadsmall = readsmall + tbuckets[id][vy].size;
                                    for (std::uint8_t* rdsmall = readsmall; rdsmall < endreadsmall; rdsmall += DSTSIZE)
                                        degs[*(std::uint32_t*)rdsmall & P::ZMASK]++;
                                    std::uint32_t ux = 0;
                                    for (std::uint8_t* rdsmall = readsmall; rdsmall < endreadsmall; rdsmall += DSTSIZE) {
                                        // bit     41/39..34    33..26     25..13     12..0
                                        // read       UXXXXX    UYYYYY     UZZZZZ     VZZZZ   within VX VY partition
                                        // bit        39..37    36..30     29..15     14..0      with XBITS==YBITS==7
                                        // read       UXXXXX    UYYYYY     UZZZZZ     VZZZZ   within VX VY partition
                                        const std::uint64_t e = *(std::uint64_t*)rdsmall & DSTSLOTMASK;
                                        ux += ((std::uint32_t)(e >> P::YZZBITS) - ux) & DSTPREFMASK;
                                        // bit    41/39..34    33..21     20..13     12..0
                                        // write     VYYYYY    VZZZZZ     UYYYYY     UZZZZ   within UX partition
                                        *(std::uint64_t*)(base + dst.index[ux]) = vy34 | ((e & P::ZMASK) << P::YZBITS) | ((e >> P::ZBITS) & P::YZMASK);
                                        dst.index[ux] += degs[e & P::ZMASK] ? DSTSIZE : 0;
                                    }
                                }
                                sumsize += TRIMONV ? dst.storev(buckets, vx) : dst.storeu(buckets, vx);
                            }
                            tcounts[id] = sumsize / DSTSIZE;
                        }

                    template <std::uint32_t SRCSIZE, std::uint32_t DSTSIZE, bool TRIMONV>
                        void trimrename(const std::uint32_t id, const std::uint32_t round)
                        {
                            const std::uint32_t SRCSLOTBITS = std::min(SRCSIZE * 8, (TRIMONV ? P::YZBITS : P::YZ1BITS) + P::YZBITS);
                            const std::uint64_t SRCSLOTMASK = (1ULL << SRCSLOTBITS) - 1ULL;
                            const std::uint32_t SRCPREFBITS = SRCSLOTBITS - P::YZBITS;
                            const std::uint32_t SRCPREFMASK = (1 << SRCPREFBITS) - 1;
                            const std::uint32_t SRCPREFBITS2 = SRCSLOTBITS - P::YZZBITS;
                            const std::uint32_t SRCPREFMASK2 = (1 << SRCPREFBITS2) - 1;
                            indexerZ dst;
                            indexerT small;
                            static std::uint32_t maxnnid = 0;

                            offset_t sumsize = 0;
                            std::uint8_t const* base = (std::uint8_t*)buckets;
                            std::uint8_t const* small0 = (std::uint8_t*)tbuckets[id];
                            const std::uint32_t startvx = P::NY * id / threads;
                            const std::uint32_t endvx = P::NY * (id + 1) / threads;
                            for (std::uint32_t vx = startvx; vx < endvx; vx++) {
                                small.matrixu(0);
                                for (std::uint32_t ux = 0; ux < P::NX; ux++) {
                                    std::uint32_t uyz = 0;
                                    zbucketZ& zb = TRIMONV ? buckets[ux][vx] : buckets[vx][ux];
                                    const std::uint8_t *readbig = zb.bytes, *endreadbig = readbig + zb.size;
                                    for (; readbig < endreadbig; readbig += SRCSIZE) {
                                        // bit        39..37    36..22     21..15     14..0
                                        // write      UYYYYY    UZZZZZ     VYYYYY     VZZZZ   within VX partition  if TRIMONV
                                        // bit            36...22     21..15     14..0
                                        // write          VYYYZZ'     UYYYYY     UZZZZ   within UX partition  if !TRIMONV
                                        const std::uint64_t e = *(std::uint64_t*)readbig & SRCSLOTMASK;
                                        if (TRIMONV)
                                            uyz += ((std::uint32_t)(e >> P::YZBITS) - uyz) & SRCPREFMASK;
                                        else
                                            uyz = e >> P::YZBITS;
                                        const std::uint32_t vy = (e >> P::ZBITS) & P::YMASK;
                                        // bit        39..37    36..30     29..15     14..0
                                        // write      UXXXXX    UYYYYY     UZZZZZ     VZZZZ   within VX VY partition  if TRIMONV
                                        // bit            36...30     29...15     14..0
                                        // write          VXXXXXX     VYYYZZ'     UZZZZ   within UX UY partition  if !TRIMONV
                                        *(std::uint64_t*)(small0 + small.index[vy]) = ((std::uint64_t)(ux << (TRIMONV ? P::YZBITS : P::YZ1BITS) | uyz) << P::ZBITS) | (e & P::ZMASK);
                                        if (TRIMONV)
                                            uyz &= ~P::ZMASK;
                                        small.index[vy] += SRCSIZE;
                                    }
                                }
                                std::uint16_t* degs = (std::uint16_t*)tdegs[id];
                                small.storeu(tbuckets + id, 0);
                                TRIMONV ? dst.matrixv(vx) : dst.matrixu(vx);
                                std::uint32_t newnodeid = 0;
                                std::uint32_t* renames = TRIMONV ? buckets[0][vx].renamev : buckets[vx][0].renameu;
                                std::uint32_t* endrenames = renames + P::NZ1;
                                for (std::uint32_t vy = 0; vy < P::NY; vy++) {
                                    memset(degs, 0xff, 2 * P::NZ);
                                    std::uint8_t *readsmall = tbuckets[id][vy].bytes, *endreadsmall = readsmall + tbuckets[id][vy].size;
                                    for (std::uint8_t* rdsmall = readsmall; rdsmall < endreadsmall; rdsmall += SRCSIZE)
                                        degs[*(std::uint32_t*)rdsmall & P::ZMASK]++;
                                    std::uint32_t ux = 0;
                                    std::uint32_t nrenames = 0;
                                    for (std::uint8_t* rdsmall = readsmall; rdsmall < endreadsmall; rdsmall += SRCSIZE) {
                                        // bit        39..37    36..30     29..15     14..0
                                        // read       UXXXXX    UYYYYY     UZZZZZ     VZZZZ   within VX VY partition  if TRIMONV
                                        // bit            36...30     29...15     14..0
                                        // read           VXXXXXX     VYYYZZ'     UZZZZ   within UX UY partition  if !TRIMONV
                                        const std::uint64_t e = *(std::uint64_t*)rdsmall & SRCSLOTMASK;
                                        if (TRIMONV)
                                            ux += ((std::uint32_t)(e >> P::YZZBITS) - ux) & SRCPREFMASK2;
                                        else
                                            ux = e >> P::YZZ1BITS;
                                        const std::uint32_t vz = e & P::ZMASK;
                                        std::uint16_t vdeg = degs[vz];
                                        if (vdeg) {
                                            if (vdeg < 32) {
                                                degs[vz] = vdeg = 32 + nrenames++;
                                                *renames++ = vy << P::ZBITS | vz;
                                                if (renames == endrenames) {
                                                    endrenames += (TRIMONV ? sizeof(yzbucketZ) : sizeof(zbucketZ)) / sizeof(std::uint32_t);
                                                    renames = endrenames - P::NZ1;
                                                }
                                            }
                                            // bit       36..22     21..15     14..0
                                            // write     VYYZZ'     UYYYYY     UZZZZ   within UX partition  if TRIMONV
                                            if (TRIMONV)
                                                *(std::uint64_t*)(base + dst.index[ux]) = ((std::uint64_t)(newnodeid + vdeg - 32) << P::YZBITS) | ((e >> P::ZBITS) & P::YZMASK);
                                            else
                                                *(std::uint32_t*)(base + dst.index[ux]) = ((newnodeid + vdeg - 32) << P::YZ1BITS) | ((e >> P::ZBITS) & P::YZ1MASK);
                                            dst.index[ux] += DSTSIZE;
                                        }
                                    }
                                    newnodeid += nrenames;
                                    if (TRIMONV && unlikely(ux >> SRCPREFBITS2 != P::XMASK >> SRCPREFBITS2)) {
                                        assert(false);
                                    }
                                }
                                if (newnodeid > maxnnid)
                                    maxnnid = newnodeid;
                                sumsize += TRIMONV ? dst.storev(buckets, vx) : dst.storeu(buckets, vx);
                            }
                            assert(maxnnid < P::NYZ1);
                            tcounts[id] = sumsize / DSTSIZE;
                        }

                    template <bool TRIMONV>
                        void trimedges1(const std::uint32_t id, const std::uint32_t round)
                        {
                            indexerZ dst;

                            offset_t sumsize = 0;
                            std::uint8_t* degs = tdegs[id];
                            std::uint8_t const* base = (std::uint8_t*)buckets;
                            const std::uint32_t startvx = P::NY * id / threads;
                            const std::uint32_t endvx = P::NY * (id + 1) / threads;
                            for (std::uint32_t vx = startvx; vx < endvx; vx++) {
                                TRIMONV ? dst.matrixv(vx) : dst.matrixu(vx);
                                memset(degs, 0xff, P::NYZ1);
                                for (std::uint32_t ux = 0; ux < P::NX; ux++) {
                                    zbucketZ& zb = TRIMONV ? buckets[ux][vx] : buckets[vx][ux];
                                    std::uint32_t *readbig = zb.words, *endreadbig = readbig + zb.size / sizeof(std::uint32_t);
                                    for (; readbig < endreadbig; readbig++)
                                        degs[*readbig & P::YZ1MASK]++;
                                }
                                for (std::uint32_t ux = 0; ux < P::NX; ux++) {
                                    zbucketZ& zb = TRIMONV ? buckets[ux][vx] : buckets[vx][ux];
                                    std::uint32_t *readbig = zb.words, *endreadbig = readbig + zb.size / sizeof(std::uint32_t);
                                    for (; readbig < endreadbig; readbig++) {
                                        // bit       29..22    21..15     14..7     6..0
                                        // read      UYYYYY    UZZZZ'     VYYYY     VZZ'   within VX partition
                                        const std::uint32_t e = *readbig;
                                        const std::uint32_t vyz = e & P::YZ1MASK;
                                        // bit       29..22    21..15     14..7     6..0
                                        // write     VYYYYY    VZZZZ'     UYYYY     UZZ'   within UX partition
                                        *(std::uint32_t*)(base + dst.index[ux]) = (vyz << P::YZ1BITS) | (e >> P::YZ1BITS);
                                        dst.index[ux] += degs[vyz] ? sizeof(std::uint32_t) : 0;
                                    }
                                }
                                sumsize += TRIMONV ? dst.storev(buckets, vx) : dst.storeu(buckets, vx);
                            }
                            tcounts[id] = sumsize / sizeof(std::uint32_t);
                        }

                    template <bool TRIMONV>
                        void trimrename1(const std::uint32_t id, const std::uint32_t round)
                        {
                            indexerZ dst;
                            static std::uint32_t maxnnid = 0;

                            offset_t sumsize = 0;
                            std::uint16_t* degs = (std::uint16_t*)tdegs[id];
                            std::uint8_t const* base = (std::uint8_t*)buckets;
                            const std::uint32_t startvx = P::NY * id / threads;
                            const std::uint32_t endvx = P::NY * (id + 1) / threads;
                            for (std::uint32_t vx = startvx; vx < endvx; vx++) {
                                TRIMONV ? dst.matrixv(vx) : dst.matrixu(vx);
                                memset(degs, 0xff, 2 * P::NYZ1);
                                for (std::uint32_t ux = 0; ux < P::NX; ux++) {
                                    zbucketZ& zb = TRIMONV ? buckets[ux][vx] : buckets[vx][ux];
                                    std::uint32_t *readbig = zb.words, *endreadbig = readbig + zb.size / sizeof(std::uint32_t);
                                    for (; readbig < endreadbig; readbig++)
                                        degs[*readbig & P::YZ1MASK]++;
                                }
                                std::uint32_t newnodeid = 0;
                                std::uint32_t* renames = TRIMONV ? buckets[0][vx].renamev1 : buckets[vx][0].renameu1;
                                std::uint32_t* endrenames = renames + P::NZ2;
                                for (std::uint32_t ux = 0; ux < P::NX; ux++) {
                                    zbucketZ& zb = TRIMONV ? buckets[ux][vx] : buckets[vx][ux];
                                    std::uint32_t *readbig = zb.words, *endreadbig = readbig + zb.size / sizeof(std::uint32_t);
                                    for (; readbig < endreadbig; readbig++) {
                                        // bit       29...15     14...0
                                        // read      UYYYZZ'     VYYZZ'   within VX partition
                                        const std::uint32_t e = *readbig;
                                        const std::uint32_t vyz = e & P::YZ1MASK;
                                        std::uint16_t vdeg = degs[vyz];
                                        if (vdeg) {
                                            if (vdeg < 32) {
                                                degs[vyz] = vdeg = 32 + newnodeid++;
                                                *renames++ = vyz;
                                                if (renames == endrenames) {
                                                    endrenames += (TRIMONV ? sizeof(yzbucketZ) : sizeof(zbucketZ)) / sizeof(std::uint32_t);
                                                    renames = endrenames - P::NZ2;
                                                }
                                            }
                                            // bit       25...15     14...0
                                            // write     VYYZZZ"     UYYZZ'   within UX partition
                                            *(std::uint32_t*)(base + dst.index[ux]) = ((vdeg - 32) << (TRIMONV ? P::YZ1BITS : P::YZ2BITS)) | (e >> P::YZ1BITS);
                                            dst.index[ux] += sizeof(std::uint32_t);
                                        }
                                    }
                                }
                                if (newnodeid > maxnnid)
                                    maxnnid = newnodeid;
                                sumsize += TRIMONV ? dst.storev(buckets, vx) : dst.storeu(buckets, vx);
                            }
                            assert(maxnnid < P::NYZ2);
                            tcounts[id] = sumsize / sizeof(std::uint32_t);
                        }

                    void trim()
                    {
                        if (threads == 1) {
                            trimmer(0);
                            return;
                        }

                        std::vector<std::future<void>> jobs;
                        for (int t = 0; t < threads; t++) {
                            jobs.push_back(
                                    pool.push([this, t](int id) {
                                        etworker<offset_t, EDGEBITS, XBITS>(this, t);
                                        }));
                        }

                        for(auto& j : jobs) {
                            j.wait();
                        }
                    }

                    void trimmer(std::uint32_t id)
                    {
                        genUnodes(id, 0);
                        barry->Wait();
                        genVnodes(id, 1);
                        for (std::uint32_t round = 2; round < nTrims - 2; round += 2) {
                            barry->Wait();
                            if (round < P::COMPRESSROUND) {
                                if (round < P::EXPANDROUND)
                                    trimedges<P::BIGSIZE, P::BIGSIZE, true>(id, round);
                                else if (round == P::EXPANDROUND)
                                    trimedges<P::BIGSIZE, P::BIGGERSIZE, true>(id, round);
                                else
                                    trimedges<P::BIGGERSIZE, P::BIGGERSIZE, true>(id, round);
                            } else if (round == P::COMPRESSROUND) {
                                trimrename<P::BIGGERSIZE, P::BIGGERSIZE, true>(id, round);
                            } else
                                trimedges1<true>(id, round);
                            barry->Wait();
                            if (round < P::COMPRESSROUND) {
                                if (round + 1 < P::EXPANDROUND)
                                    trimedges<P::BIGSIZE, P::BIGSIZE, false>(id, round + 1);
                                else if (round + 1 == P::EXPANDROUND)
                                    trimedges<P::BIGSIZE, P::BIGGERSIZE, false>(id, round + 1);
                                else
                                    trimedges<P::BIGGERSIZE, P::BIGGERSIZE, false>(id, round + 1);
                            } else if (round == P::COMPRESSROUND) {
                                trimrename<P::BIGGERSIZE, sizeof(std::uint32_t), false>(id, round + 1);
                            } else
                                trimedges1<false>(id, round + 1);
                        }
                        barry->Wait();
                        trimrename1<true>(id, nTrims - 2);
                        barry->Wait();
                        trimrename1<false>(id, nTrims - 1);
                    }
            };

        int nonce_cmp(const void* a, const void* b)
        {
            return *(std::uint32_t*)a - *(std::uint32_t*)b;
        }

        template <typename offset_t, std::uint8_t EDGEBITS, std::uint8_t XBITS>
            class solver_ctx
            {
                public:
                    using P = Params<EDGEBITS, XBITS>;
                    using zbucket8P = zbucket8<EDGEBITS, XBITS>;
                    using zbucket16P = zbucket16<EDGEBITS, XBITS>;
                    using zbucket32P = zbucket32<EDGEBITS, XBITS>;
                    using zbucketZ = zbucket<EDGEBITS, XBITS, P::ZBUCKETSIZE>;
                    using yzbucketT = yzbucket<EDGEBITS, XBITS, P::TBUCKETSIZE>;

                    edgetrimmer<offset_t, EDGEBITS, XBITS>* trimmer;
                    std::uint32_t* cuckoo = 0;
                    std::vector<std::uint32_t> cycleus;
                    std::vector<std::uint32_t> cyclevs;
                    std::bitset<P::NXY> uxymap;
                    std::vector<std::uint32_t> sols; // concatanation of all proof's indices
                    ctpl::thread_pool& pool;
                    size_t threads;
                    std::uint8_t proofSize;

                    solver_ctx(
                            ctpl::thread_pool& poolIn,
                            size_t threadsIn,
                            const char* header,
                            const std::uint32_t headerlen,
                            const std::uint32_t nTrims,
                            const std::uint8_t proofSizeIn) : pool{poolIn}, threads{threadsIn}, proofSize{proofSizeIn}
                    {
                        trimmer = new edgetrimmer<offset_t, EDGEBITS, XBITS>(pool, threadsIn, nTrims);

                        cycleus.reserve(proofSize);
                        cyclevs.reserve(proofSize);

                        setHeader(header, headerlen, &trimmer->sip_keys);

                        cuckoo = 0;
                    }

                    ~solver_ctx()
                    {
                        delete trimmer;
                    }

                    std::uint64_t sharedbytes() const
                    {
                        return sizeof(matrix<EDGEBITS, XBITS, P::ZBUCKETSIZE>);
                    }

                    std::uint32_t threadbytes() const
                    {
                        return sizeof(yzbucketT) + sizeof(zbucket8P) + sizeof(zbucket16P) + sizeof(zbucket32P);
                    }

                    void recordedge(const std::uint32_t i, const std::uint32_t u2, const std::uint32_t v2)
                    {
                        const std::uint32_t u1 = u2 / 2;
                        const std::uint32_t ux = u1 >> P::YZ2BITS;
                        std::uint32_t uyz = trimmer->buckets[ux][(u1 >> P::Z2BITS) & P::YMASK].renameu1[u1 & P::Z2MASK];
                        assert(uyz < P::NYZ1);
                        const std::uint32_t v1 = v2 / 2;
                        const std::uint32_t vx = v1 >> P::YZ2BITS;
                        std::uint32_t vyz = trimmer->buckets[(v1 >> P::Z2BITS) & P::YMASK][vx].renamev1[v1 & P::Z2MASK];
                        assert(vyz < P::NYZ1);

                        if (P::COMPRESSROUND > 0) {
                            uyz = trimmer->buckets[ux][uyz >> P::Z1BITS].renameu[uyz & P::Z1MASK];
                            vyz = trimmer->buckets[vyz >> P::Z1BITS][vx].renamev[vyz & P::Z1MASK];
                        }

                        const std::uint32_t u = ((ux << P::YZBITS) | uyz) << 1;
                        const std::uint32_t v = ((vx << P::YZBITS) | vyz) << 1 | 1;

                        cycleus[i] = u / 2;
                        cyclevs[i] = v / 2;
                        uxymap[u / 2 >> P::ZBITS] = 1;
                    }

                    void solution(const std::uint32_t* us, std::uint32_t nu, const std::uint32_t* vs, std::uint32_t nv)
                    {
                        std::uint32_t ni = 0;
                        recordedge(ni++, *us, *vs);
                        while (nu--)
                            recordedge(ni++, us[(nu + 1) & ~1], us[nu | 1]); // u's in even position; v's in odd
                        while (nv--)
                            recordedge(ni++, vs[nv | 1], vs[(nv + 1) & ~1]); // u's in odd position; v's in even

                        sols.resize(sols.size() + proofSize);

                        std::vector<std::future<void>> jobs;
                        for (size_t t = 0; t < threads; t++) {
                            jobs.push_back(
                                    pool.push(
                                        [this, t](int id) {
                                            matchworker<offset_t, EDGEBITS, XBITS>(this, t);
                                        }));
                        }

                        for (auto& j : jobs) {
                            j.wait();
                        }

                        auto start = sols.begin() + (sols.size() - proofSize);
                        std::sort(start, start + proofSize); 
                    }

                    static const std::uint32_t CUCKOO_NIL = ~0;

                    std::uint32_t path(std::uint32_t u, std::uint32_t* us) const
                    {
                        std::uint32_t nu, u0 = u;
                        for (nu = 0; u != CUCKOO_NIL; u = cuckoo[u]) {
                            if (nu >= MAXPATHLEN) {
                                while (nu-- && us[nu] != u)
                                    ;
                                break;
                            }
                            us[nu++] = u;
                        }
                        return nu - 1;
                    }

                    bool findcycles()
                    {
                        std::uint32_t us[MAXPATHLEN], vs[MAXPATHLEN];

                        bool found = false;
                        for (std::uint32_t vx = 0; vx < P::NX; vx++) {
                            for (std::uint32_t ux = 0; ux < P::NX; ux++) {
                                zbucketZ& zb = trimmer->buckets[ux][vx];
                                std::uint32_t *readbig = zb.words, *endreadbig = readbig + zb.size / sizeof(std::uint32_t);
                                for (; readbig < endreadbig; readbig++) {
                                    // bit        21..11     10...0
                                    // write      UYYZZZ'    VYYZZ'   within VX partition
                                    const std::uint32_t e = *readbig;
                                    const std::uint32_t uxyz = (ux << P::YZ2BITS) | (e >> P::YZ2BITS);
                                    const std::uint32_t vxyz = (vx << P::YZ2BITS) | (e & P::YZ2MASK);

                                    const std::uint32_t u0 = uxyz << 1, v0 = (vxyz << 1) | 1;
                                    if (u0 != CUCKOO_NIL) {
                                        std::uint32_t nu = path(u0, us);
                                        std::uint32_t nv = path(v0, vs);
                                        if (us[nu] == vs[nv]) {
                                            const std::uint32_t min = nu < nv ? nu : nv;
                                            for (nu -= min, nv -= min; us[nu] != vs[nv]; nu++, nv++)
                                                ;
                                            const std::uint32_t len = nu + nv + 1;
                                            if (len == proofSize) {
                                                solution(us, nu, vs, nv);
                                                found = true;
                                            }
                                        } else if (nu < nv) {
                                            while (nu--)
                                                cuckoo[us[nu + 1]] = us[nu];
                                            cuckoo[u0] = v0;
                                        } else {
                                            while (nv--)
                                                cuckoo[vs[nv + 1]] = vs[nv];
                                            cuckoo[v0] = u0;
                                        }
                                    }
                                }
                            }
                        }

                        return found;
                    }

                    bool solve()
                    {
                        assert((std::uint64_t)P::CUCKOO_SIZE * sizeof(std::uint32_t) <= trimmer->threads * sizeof(yzbucketT));
                        trimmer->trim();
                        cuckoo = (std::uint32_t*)trimmer->tbuckets;
                        memset(cuckoo, CUCKOO_NIL, P::CUCKOO_SIZE * sizeof(std::uint32_t));

                        return findcycles();
                    }

                    void* matchUnodes(std::uint32_t threadId)
                    {
                        const std::uint32_t starty = P::NY * threadId / trimmer->threads;
                        const std::uint32_t endy = P::NY * (threadId + 1) / trimmer->threads;

                        std::uint32_t edge = starty << P::YZBITS;
                        std::uint32_t endedge = edge + P::NYZ;

#if NSIPHASH == 8
                        static const __m256i vnodemask = {P::EDGEMASK, P::EDGEMASK, P::EDGEMASK, P::EDGEMASK};
                        const __m256i vinit = _mm256_set_epi64x(
                                trimmer->sip_keys.k1 ^ 0x7465646279746573ULL,
                                trimmer->sip_keys.k0 ^ 0x6c7967656e657261ULL,
                                trimmer->sip_keys.k1 ^ 0x646f72616e646f6dULL,
                                trimmer->sip_keys.k0 ^ 0x736f6d6570736575ULL);
                        __m256i v0, v1, v2, v3, v4, v5, v6, v7;
                        const std::uint32_t e2 = 2 * edge;
                        __m256i vpacket0 = _mm256_set_epi64x(e2 + 6, e2 + 4, e2 + 2, e2 + 0);
                        __m256i vpacket1 = _mm256_set_epi64x(e2 + 14, e2 + 12, e2 + 10, e2 + 8);
                        static const __m256i vpacketinc = {16, 16, 16, 16};
#endif

                        for (std::uint32_t my = starty; my < endy; my++, endedge += P::NYZ) {
                            for (; edge < endedge; edge += NSIPHASH) {
                                // bit        28..21     20..13    12..0
                                // node       XXXXXX     YYYYYY    ZZZZZ
#if NSIPHASH == 1
                                const std::uint32_t nodeu = _sipnode(&trimmer->sip_keys, P::EDGEMASK, edge, 0);
                                if (uxymap[nodeu >> P::ZBITS]) {
                                    for (std::uint32_t j = 0; j < proofSize; j++) {
                                        if (cycleus[j] == nodeu && cyclevs[j] == _sipnode(&trimmer->sip_keys, P::EDGEMASK, edge, 1)) {
                                            sols[sols.size() - proofSize + j] = edge;
                                        }
                                    }
                                }
                                // bit        39..21     20..13    12..0
                                // write        edge     YYYYYY    ZZZZZ
#elif NSIPHASH == 8
                                v3 = _mm256_permute4x64_epi64(vinit, 0xFF);
                                v0 = _mm256_permute4x64_epi64(vinit, 0x00);
                                v1 = _mm256_permute4x64_epi64(vinit, 0x55);
                                v2 = _mm256_permute4x64_epi64(vinit, 0xAA);
                                v7 = _mm256_permute4x64_epi64(vinit, 0xFF);
                                v4 = _mm256_permute4x64_epi64(vinit, 0x00);
                                v5 = _mm256_permute4x64_epi64(vinit, 0x55);
                                v6 = _mm256_permute4x64_epi64(vinit, 0xAA);

                                v3 = XOR(v3, vpacket0);
                                v7 = XOR(v7, vpacket1);
                                SIPROUNDX8;
                                SIPROUNDX8;
                                v0 = XOR(v0, vpacket0);
                                v4 = XOR(v4, vpacket1);
                                v2 = XOR(v2, _mm256_broadcastq_epi64(_mm_cvtsi64_si128(0xff)));
                                v6 = XOR(v6, _mm256_broadcastq_epi64(_mm_cvtsi64_si128(0xff)));
                                SIPROUNDX8;
                                SIPROUNDX8;
                                SIPROUNDX8;
                                SIPROUNDX8;
                                v0 = XOR(XOR(v0, v1), XOR(v2, v3));
                                v4 = XOR(XOR(v4, v5), XOR(v6, v7));

                                vpacket0 = _mm256_add_epi64(vpacket0, vpacketinc);
                                vpacket1 = _mm256_add_epi64(vpacket1, vpacketinc);
                                v0 = v0 & vnodemask;
                                v4 = v4 & vnodemask;
                                v1 = _mm256_srli_epi64(v0, P::ZBITS);
                                v5 = _mm256_srli_epi64(v4, P::ZBITS);

                                std::uint32_t uxy;
#define MATCH(i, v, x, w)                                                                                  \
                                uxy = _mm256_extract_epi32(v, x);                                                                      \
                                if (uxymap[uxy]) {                                                                                     \
                                    std::uint32_t u = _mm256_extract_epi32(w, x);                                                           \
                                    for (std::uint32_t j = 0; j < proofSize; j++) {                                                         \
                                        if (cycleus[j] == u && cyclevs[j] == _sipnode(&trimmer->sip_keys, P::EDGEMASK, edge + i, 1)) { \
                                            sols[sols.size() - proofSize + j] = edge + i;                                              \
                                        }                                                                                              \
                                    }                                                                                                  \
                                }
                                MATCH(0, v1, 0, v0);
                                MATCH(1, v1, 2, v0);
                                MATCH(2, v1, 4, v0);
                                MATCH(3, v1, 6, v0);
                                MATCH(4, v5, 0, v4);
                                MATCH(5, v5, 2, v4);
                                MATCH(6, v5, 4, v4);
                                MATCH(7, v5, 6, v4);
#else
#error not implemented
#endif
                            }
                        }

                        return 0;
                    }
            };

        template <typename offset_t, std::uint8_t EDGEBITS, std::uint8_t XBITS>
            bool run(
                    const char* hex_header_hash,
                    uint32_t hex_header_hash_len,
                    std::uint8_t proofSize,
                    Cycles& cycles,
                    size_t threads,
                    ctpl::thread_pool& pool)
            {
                assert(hex_header_hash != nullptr);
                assert(hex_header_hash_len > 0);
                assert(EDGEBITS >= MIN_EDGE_BITS && EDGEBITS <= MAX_EDGE_BITS);

                std::uint32_t nTrims = EDGEBITS >= 30 ? 96 : 68;

                solver_ctx<offset_t, EDGEBITS, XBITS> ctx{
                    pool,
                        threads,
                        hex_header_hash,
                        static_cast<std::uint32_t>(hex_header_hash_len),
                        nTrims,
                        proofSize};

                bool found = ctx.solve();

                if (found) {
                    for(int i = 0; i < ctx.sols.size() / proofSize; i++) {
                        Cycle cycle;
                        copy(
                                ctx.sols.begin() + (i * proofSize),
                                ctx.sols.begin() + (i * proofSize) + proofSize,
                                inserter(cycle, cycle.begin()));
                        cycles.emplace_back(cycle);
                    }
                }

                return found;
            }

        bool FindCycles(
                const char* hex_header_hash,
                uint32_t hex_header_hash_len,
                std::uint8_t edgeBits,
                std::uint8_t proofSize,
                Cycles& cycles,
                size_t threads,
                ctpl::thread_pool& pool)
        {
            switch (edgeBits) {
                case 16: return run<std::uint32_t, 16u, 0u>(hex_header_hash, hex_header_hash_len, proofSize, cycles, threads, pool);
                case 17: return run<std::uint32_t, 17u, 1u>(hex_header_hash, hex_header_hash_len, proofSize, cycles, threads, pool);
                case 18: return run<std::uint32_t, 18u, 1u>(hex_header_hash, hex_header_hash_len, proofSize, cycles, threads, pool);
                case 19: return run<std::uint32_t, 19u, 2u>(hex_header_hash, hex_header_hash_len, proofSize, cycles, threads, pool);
                case 20: return run<std::uint32_t, 20u, 2u>(hex_header_hash, hex_header_hash_len, proofSize, cycles, threads, pool);
                case 21: return run<std::uint32_t, 21u, 3u>(hex_header_hash, hex_header_hash_len, proofSize, cycles, threads, pool);
                case 22: return run<std::uint32_t, 22u, 3u>(hex_header_hash, hex_header_hash_len, proofSize, cycles, threads, pool);
                case 23: return run<std::uint32_t, 23u, 4u>(hex_header_hash, hex_header_hash_len, proofSize, cycles, threads, pool);
                case 24: return run<std::uint32_t, 24u, 4u>(hex_header_hash, hex_header_hash_len, proofSize, cycles, threads, pool);
                case 25: return run<std::uint32_t, 25u, 5u>(hex_header_hash, hex_header_hash_len, proofSize, cycles, threads, pool);
                case 26: return run<std::uint32_t, 26u, 5u>(hex_header_hash, hex_header_hash_len, proofSize, cycles, threads, pool);
                case 27: return run<std::uint32_t, 27u, 6u>(hex_header_hash, hex_header_hash_len, proofSize, cycles, threads, pool);
                case 28: return run<std::uint32_t, 28u, 6u>(hex_header_hash, hex_header_hash_len, proofSize, cycles, threads, pool);
                case 29: return run<std::uint32_t, 29u, 7u>(hex_header_hash, hex_header_hash_len, proofSize, cycles, threads, pool);
                case 30: return run<std::uint64_t, 30u, 8u>(hex_header_hash, hex_header_hash_len, proofSize, cycles, threads, pool);
                case 31: return run<std::uint64_t, 31u, 8u>(hex_header_hash, hex_header_hash_len, proofSize, cycles, threads, pool);

                default:
                         std::stringstream s;
                         s << __func__ << ": EDGEBITS equal to " << edgeBits << " is not supported";
                         throw std::runtime_error{s.str()};
            }
        }
    } //namespace cuckoo
} //namespace bitcash
