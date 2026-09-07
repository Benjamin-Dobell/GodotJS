#ifndef GODOTJS_TESTS_JSB_SARRAY_H
#define GODOTJS_TESTS_JSB_SARRAY_H

#include "jsb_test_helpers.h"

#include <atomic>
#include <thread>
#include <vector>

// all internal::SArray test cases
namespace jsb::tests
{
    inline String get_test_name() { return "xxx.yyy.zzz.000"; }

    struct Movable
    {
        int anything = 0;
        void* ptr = nullptr;

        ~Movable() { MESSAGE("Movable destructor"); }
        Movable() { MESSAGE("Movable default constructor"); }
        Movable(int p_anything) : anything(p_anything), ptr(nullptr) { MESSAGE("Movable int constructor");}
        Movable(int p_anything, void* p_ptr) : anything(p_anything), ptr(p_ptr) { MESSAGE("Movable full constructor"); }
        Movable(Movable&& other) noexcept : anything(other.anything), ptr(other.ptr)
        {
            MESSAGE("Movable move constructor");
            other.anything = 0;
            other.ptr = nullptr;
        }
        Movable& operator=(Movable&& other) noexcept
        {
            if (this != &other)
            {
                MESSAGE("Movable move assignment");
                anything = other.anything;
                ptr = other.ptr;
                other.anything = 0;
                other.ptr = nullptr;
            }
            return *this;
        }
        Movable(const Movable& other) : anything(other.anything), ptr(other.ptr)
        { MESSAGE("Movable copy constructor"); }
        Movable& operator=(const Movable& other)
        {
            if (this != &other)
            {
                MESSAGE("Movable copy assignment");
                anything = other.anything;
                ptr = other.ptr;
            }
            return *this;
        }
    };

    struct Slot
    {
        StringName name;
        Movable anything;

        Slot() = default;
        Slot(const StringName& p_name, int p_anything) : name(p_name), anything(p_anything) {}
        Slot(Slot&& other) noexcept = default;
        Slot& operator=(Slot&& other) noexcept = default;
        Slot(const Slot& other) = default;
        Slot& operator=(const Slot& other) = default;
    };

    TEST_CASE("[jsb.internal] SArray move_to_back")
    {
        internal::SArray<Movable> sarray;
        static constexpr int kCount = 10;
        for (int i = 0; i < kCount; ++i) { sarray.add(i); }
        CHECK(sarray.size() == kCount);
        for (int i = 0; i < 5; ++i)
        {
            CHECK(sarray.size() == kCount);
            internal::Index64 id = sarray.get_first_index();
            for (int p = rand() % (kCount / 2); p >= 0; --p)
            {
                id = sarray.get_next_index(id);
            }
            sarray.move_to_back(id);
            CHECK(sarray.get_last_index() == id);
            CHECK(sarray.size() == kCount);
        }
        internal::Index64 index = sarray.get_first_index();
        int dc = 0;
        while (index != internal::Index64::none())
        {
            index = sarray.get_next_index(index);
            dc++;
        }
        CHECK(sarray.size() == dc);
    }

    TEST_CASE("[jsb.internal] SArray Movable test")
    {
        internal::SArray<Movable> sarray;
        MESSAGE("STEP ----");
        {
            Movable copy = 1;
            sarray.add(copy);
            copy.anything = -1;
        }
        MESSAGE("STEP ----");
        sarray.add(2);
        sarray.add(3);
        sarray.clear();
        MESSAGE("STEP ----");
        sarray.add(4);
    }
    
    TEST_CASE("[jsb.internal] SArray StringName test")
    {
        internal::SArray<Slot> sarray;
        {
            const StringName name = get_test_name();
            HashMap<StringName, Slot> map;
            map.insert(get_test_name(), { name, 0 });
            sarray.add({ get_test_name(), 1});
        }
        {
            const StringName name = get_test_name();
            sarray.add({ get_test_name(), 1});
            sarray.add({ name, 2});
            sarray.clear();
        }
    }

    TEST_CASE("[jsb.internal] SArray concurrent scoped readers")
    {
        internal::SArray<int> sarray;
        const internal::Index64 value_id = sarray.add(42);
        const internal::SArray<int>& read_only_sarray = sarray;

        static constexpr int kThreadCount = 8;
        static constexpr int kIterationsPerThread = 100000;
        std::atomic<int> ready_count = 0;
        std::atomic<int> failures = 0;
        std::atomic<bool> start = false;
        std::vector<std::thread> readers;
        readers.reserve(kThreadCount);

        for (int thread_index = 0; thread_index < kThreadCount; ++thread_index)
        {
            readers.emplace_back([&]()
            {
                ready_count.fetch_add(1, std::memory_order_relaxed);
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }

                for (int iteration = 0; iteration < kIterationsPerThread; ++iteration)
                {
                    auto value = read_only_sarray.get_value_scoped(value_id);
                    if (!value || *value != 42)
                    {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }

                    switch (iteration % 4)
                    {
                        case 0:
                            // Exercise destructor release with a live scoped pointer.
                            break;
                        case 1:
                            value = nullptr;
                            break;
                        case 2:
                        {
                            auto moved_value = std::move(value);
                            if (!moved_value || value || *moved_value != 42)
                            {
                                failures.fetch_add(1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        default:
                        {
                            auto copied_value = value;
                            if (!copied_value || *copied_value != 42)
                            {
                                failures.fetch_add(1, std::memory_order_relaxed);
                            }
                            copied_value = nullptr;
                            value = nullptr;
                            break;
                        }
                    }
                }
            });
        }

        while (ready_count.load(std::memory_order_acquire) != kThreadCount)
        {
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);

        for (std::thread& reader : readers)
        {
            reader.join();
        }

        CHECK(failures.load(std::memory_order_relaxed) == 0);
        CHECK(sarray.remove_at(value_id));
    }
}

#endif
