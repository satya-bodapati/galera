/*
 * Copyright (C) 2010-2020 Codership Oy <info@codership.com>
 */

/*! @file ring buffer storage class */

#ifndef _gcache_rb_store_hpp_
#define _gcache_rb_store_hpp_

#include "gcache_memops.hpp"
#include "gcache_bh.hpp"
#include "gcache_types.hpp"

#include <gu_fdesc.hpp>
#include <gu_mmap.hpp>
#include <gu_uuid.hpp>

#include <string>

namespace gcache
{
    class RingBuffer : public MemOps
    {
    public:

        RingBuffer (const std::string& name,
                    size_t             size,
                    seqno2ptr_t&       seqno2ptr,
                    gu::UUID&          gid,
                    int                dbg,
                    bool               recover);

        ~RingBuffer ();

        void* malloc  (size_type size);

        void* realloc (void* ptr, size_type size);

        void  free    (BufferHeader* bh);

        void repossess(BufferHeader* bh)
        {
            assert(bh->size > 0);
            assert(bh->seqno_g != SEQNO_NONE);
            assert(bh->seqno_g != SEQNO_ILL);
            assert(bh->store == BUFFER_IN_RB);
            assert(bh->ctx == reinterpret_cast<BH_ctx_t>(this));
            assert(BH_is_released(bh)); // will be marked unreleased by caller

            // Buffer goes from size_rnd_ -> size_used_
            size_used_ += bh->size;
            size_rnd_ -= bh->size;
            assert(size_used_ <= size_cache_);
            assert_size_free();
        }

        void  discard (BufferHeader* const bh)
        {
            assert (BH_is_released(bh));
            assert (BUFFER_IN_RB == bh->store);

            // In general buffer should be released and then discarded,
            // so seqno_g should be != SEQNO_ILL. During releasing, the buffer
            // should have been marked as released and moved to size_rnd_.
            // However, recover() marks buffers to be discarded with SEQNO_ILL
            // leaving them in size_used_ not in size_rnd_.

            // return this buffer to size_free_ pool
            size_free_ += bh->size;
            if (bh->seqno_g != SEQNO_ILL){
              // it was released, but not discarded so it was in size_rnd_
              assert(size_rnd_ >= bh->size);
              size_rnd_ -= bh->size;
            }

            assert (size_free_ <= size_cache_);

            bh->seqno_g = SEQNO_ILL;
        }

        size_t size      () const { return size_cache_; }

        size_t rb_size   () const { return fd_.size(); }

        const std::string& rb_name() const { return fd_.name(); }

        void  reset();

        void  seqno_reset(bool zero_out = false);

        /* returns true when successfully discards all seqnos in range */
        bool  discard_seqnos(seqno2ptr_t::iterator i_begin,
                             seqno2ptr_t::iterator i_end);

        /* returns true when successfully discards all seqnos up to s */
        bool  discard_seqno(seqno_t s)
        {
            return discard_seqnos(seqno2ptr_.begin(), seqno2ptr_.find(s + 1));
        }

        void print (std::ostream& os) const;

        static size_t pad_size()
        {
            RingBuffer* rb(0);
            // cppcheck-suppress nullPointer
            return (PREAMBLE_LEN * sizeof(*(rb->preamble_)) +
                    // cppcheck-suppress nullPointer
                    HEADER_LEN   * sizeof(*(rb->header_)));
        }

        void assert_size_free() const
        {
#ifndef NDEBUG
            assert(size_used_ + size_free_ + size_rnd_ == size_cache_);
            if (next_ >= first_)
            {
                /* start_  first_      next_    end_
                 *   |       |###########|       |      */
                assert(size_free_ >= (size_cache_ - (next_ - first_)));
            }
            else
            {
                /* start_  next_       first_   end_
                 *   |#######|           |#####| |      */
                assert(size_free_ >= size_t(first_ - next_));
            }
            assert (size_free_ <= size_cache_);
#endif
        }

        void assert_size_trail() const
        {
#ifndef NDEBUG
            if (next_ >= first_)
                assert(0 == size_trail_);
            else
                assert(size_trail_ >= sizeof(BufferHeader));
#endif
        }

        void assert_sizes() const
        {
            assert_size_trail();
            assert_size_free();
        }

#ifdef PXC
        size_t allocated_pool_size ();

        void set_freeze_purge_at_seqno(seqno_t seqno)
        {
            freeze_purge_at_seqno_ = seqno;
        }

        bool skip_purge(seqno_t seqno)
        {
            return ((freeze_purge_at_seqno_ == SEQNO_ILL)
                    ? (false) : (seqno >= freeze_purge_at_seqno_));
        }
#endif /* PXC */

        void set_debug(int const dbg) { debug_ = dbg & DEBUG; }

#ifdef GCACHE_RB_UNIT_TEST
        ptrdiff_t offset(const void* const ptr) const
        {
            return static_cast<const uint8_t*>(ptr) - start_;
        }
#endif

    private:

        static size_t const PREAMBLE_LEN = 1024;
        static size_t const HEADER_LEN = 32;

        // 0 - undetermined version
        // 1 - initial version, no buffer alignment
        // 2 - buffer alignemnt to GU_WORD_BYTES
        static int    const VERSION = 2;

        static int    const DEBUG = 2; // debug flag

        gu::FileDescriptor fd_;
        gu::MMap           mmap_;
        char*        const preamble_; // ASCII text preamble
        int64_t*     const header_;   // cache binary header
        uint8_t*     const start_;    // start of cache area
        uint8_t*     const end_;      // first byte after cache area
        uint8_t*           first_;    // pointer to the first (oldest) buffer
        uint8_t*           next_;     // pointer to the next free space

        seqno2ptr_t&       seqno2ptr_;
        gu::UUID&          gid_;

#ifdef PXC
        size_t             max_used_; // maximal memory usage (in bytes)
        seqno_t            freeze_purge_at_seqno_;
#endif /* PXC */

        /*
          Buffer can be in one of the following states:
          active    - holds data
          released  - holds data, however application is not interested with it right now.
                      Can be discarded if application needs to store new data, but if not
                      discarded yet, it can be repossesed and became 'active' 
          discarded - application explicite claimed that will never be interested with this
                      buffer again, so will never try to reposses it.

          Cache buffer consists of the following 'areas':
          (note that these are not real areas and nothing is moved. We just keep track
          of counters that track how much memory is used to keep buffers in particular state)
          size_free_  - memory that is not occupied by any buffer
          size_used_  - 'active' buffers
          size_rnd_   - 'released' but not 'discarded' buffers

          size_trail_ - chunk of memory at the end of RB that is not large enough to hold
                        the buffer. This size is includded in size_free_ calculation.

          Buffer can be 'moved' between blocks like:
          size_free_ -> size_used_ -> size_rnd_ -> size_free_
                                                -> size_used_

          size_cache_ = size_free_ + size_used_ + size_rnd_
        */
        size_t       const size_cache_;
        size_t             size_free_;
        size_t             size_used_;
        size_t             size_rnd_;
        size_t             size_trail_;

        int                debug_;

        bool               open_;

        BufferHeader* get_new_buffer (size_type size);

        void          constructor_common();

        /* preamble fields */
        static std::string const PR_KEY_VERSION;
        static std::string const PR_KEY_GID;
        static std::string const PR_KEY_SEQNO_MAX;
        static std::string const PR_KEY_SEQNO_MIN;
        static std::string const PR_KEY_OFFSET;
        static std::string const PR_KEY_SYNCED;

        void          write_preamble(bool synced);
        void          open_preamble(bool recover);
        void          close_preamble();

        // returns lower bound (not inclusive) of valid seqno range
        int64_t       scan(off_t offset, int scan_step);
        void          recover(off_t offset, int version);

        void          estimate_space(bool zero_out = false);

        RingBuffer(const gcache::RingBuffer&);
        RingBuffer& operator=(const gcache::RingBuffer&);

#ifdef GCACHE_RB_UNIT_TEST
    public:
        uint8_t* start() const { return start_; }
#endif
    };

    inline std::ostream& operator<< (std::ostream& os, const RingBuffer& rb)
    {
        rb.print(os);
        return os;
    }

} /* namespace gcache */

#endif /* _gcache_rb_store_hpp_ */
