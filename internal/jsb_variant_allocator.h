﻿#ifndef GODOTJS_VARIANT_ALLOCATOR_H
#define GODOTJS_VARIANT_ALLOCATOR_H

#include "jsb_internal_pch.h"
#include "jsb_macros.h"

namespace jsb::internal
{
    class VariantAllocator
    {
        bool use_front_ = true;
        Vector<Variant*> front_;
        Vector<Variant*> back_;
        SpinLock spin_lock_;

#if JSB_DEBUG
        SafeNumeric<uint32_t> alive_variants_num_;
#endif

#if JSB_WITH_V8 || JSB_WITH_JAVASCRIPTCORE
        // v8 and jsc implementation of deleter are probably called from JS gc threads
        // so we need thread-safe version
        PagedAllocator<Variant, true> paged_allocator_;
#else
        // web and quickjs implementation of deleter are called from owner thread only
        PagedAllocator<Variant, false> paged_allocator_;
#endif

    public:
#if JSB_DEBUG
        jsb_force_inline uint32_t get_allocated_num() const { return alive_variants_num_.get(); }
#else
        // intentionally ignored in release mode
        jsb_force_inline uint32_t get_allocated_num() const { return 0; }
#endif

#if JSB_DEBUG
        ~VariantAllocator()
        {
            spin_lock_.lock();
            jsb_notice(front_.is_empty() && back_.is_empty(), "the pending queue is not empty");
            spin_lock_.unlock();
            jsb_notice(get_allocated_num() == 0, "variant pool leaked");
        }
#endif

        jsb_force_inline Variant* alloc(const Variant& p_templet)
        {
            Variant* rval = alloc();
            *rval = p_templet;
            return rval;
        }

        jsb_force_inline Variant* alloc() { increment(); return paged_allocator_.alloc(); }

        //NOTE safe to call from other threads only if p_var is not reference-based type
        jsb_force_inline void free(Variant* p_var) { decrement(); paged_allocator_.free(p_var); }

        // gc thread
        void free_safe(Variant* p_var)
        {
            spin_lock_.lock();
            if (use_front_) front_.push_back(p_var);
            else back_.push_back(p_var);
            spin_lock_.unlock();
        }

        // should only be called on owner thread
        void drain()
        {
            spin_lock_.lock();
            Vector<Variant*>& queue = use_front_ ? front_ : back_;
            use_front_ = !use_front_;
            spin_lock_.unlock();

            if (!queue.is_empty())
            {
                for (Variant* variant : queue)
                {
                    free(variant);
                }
                queue.clear();
            }
        }

    private:
#if JSB_DEBUG
        jsb_force_inline void increment() { alive_variants_num_.increment(); }
        jsb_force_inline void decrement() { alive_variants_num_.decrement(); }
#else
        jsb_force_inline void increment() {}
        jsb_force_inline void decrement() {}
#endif
    };
}

#endif
